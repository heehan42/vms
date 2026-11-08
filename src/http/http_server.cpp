#include "http/http_server.h"

#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <array>
#include <span>
#include <cerrno>
#include <print>
#include <string>
#include <utility>

#include "core/system_error.h"
#include "http/byte_range.h"
#include "http/http_request.h"
#include "http/http_response.h"
#include "http/mime_type.h"
#include "http/static_files.h"


namespace vms::http {


namespace {


// 한 번의 recv로 읽는 최대 바이트 수
constexpr std::size_t kReceiveChunkSize = 4096;


// 요청 대상에서 쿼리 문자열을 뺀 경로를 돌려준다.
//
// target: 요청 대상 원문. 예: "/offer?x=1"
// 반환값: target 안의 경로 부분. 예: "/offer". target이 살아 있는 동안에만 유효하다.
std::string_view RequestPath(std::string_view target) {
    return target.substr(0, target.find('?'));
}


// 요청 한 건의 처리 결과를 "메서드 대상 상태코드" 형식으로 로그에 남긴다.
//
// request: 처리한 요청
// status: 응답 상태 코드
void LogRequest(const HttpRequest& request, HttpStatus status) {
    std::println(stderr, "{} {} {}", request.method, request.target, StatusCodeNumber(status));
}


// 0.0.0.0:port에 바인드한 논블로킹 리스닝 소켓을 만든다.
//
// port: 바인드할 TCP 포트
// 반환값: listen 상태의 소켓. 실패하면 시스템 오류 코드
std::expected<core::UniqueFd, std::error_code> OpenListenSocket(std::uint16_t port) {
    core::UniqueFd listen_socket(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!listen_socket.IsValid()) {
        return std::unexpected(core::LastSystemError());
    }

    // 서버를 재시작할 때 이전 연결의 TIME_WAIT 때문에 bind가 실패하지 않게 한다.
    const int enable = 1;
    if (::setsockopt(listen_socket.Get(), SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) != 0) {
        return std::unexpected(core::LastSystemError());
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (::bind(listen_socket.Get(), reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0) {
        return std::unexpected(core::LastSystemError());
    }
    if (::listen(listen_socket.Get(), SOMAXCONN) != 0) {
        return std::unexpected(core::LastSystemError());
    }
    return listen_socket;
}


}  // namespace


// 연결 하나의 처리 상태.
//
// 헤더 수신 -> (본문 수신 또는 파일 읽기 결과 대기) -> 응답 송신 -> 종료 순서로만 진행하며,
// 응답 하나를 보내면 연결을 닫는다.
struct HttpServer::Connection {
    // 연결의 처리 단계
    enum class State {
        // 요청 헤더 영역이 완성될 때까지 수신한다. EPOLLIN을 감시한다.
        kReceivingHeader,

        // POST 엔드포인트 요청의 본문이 Content-Length만큼 모일 때까지 수신한다. EPOLLIN을 감시한다.
        kReceivingBody,

        // 파일 읽기 스레드가 파일 응답의 결과를 넘길 때까지 기다린다. 감시 이벤트는 0이며,
        // 감시 이벤트와 관계없이 전달되는 EPOLLHUP·EPOLLERR만 받는다.
        kWaitingForFile,

        // 준비된 응답을 모두 보낼 때까지 송신한다. EPOLLOUT을 감시한다.
        kSendingResponse,
    };


    // 연결 소켓. 연결이 닫힐 때 함께 닫힌다.
    core::UniqueFd socket;


    // 서버가 붙인 연결 번호. 파일 읽기 결과가 같은 fd 번호를 받은 새 연결로 가지 않게 확인하는 데 쓴다.
    std::uint64_t id = 0;


    // 현재 처리 단계
    State state = State::kReceivingHeader;


    // 지금까지 받은 요청 바이트.
    //
    // kReceivingHeader에서는 헤더 영역을, kReceivingBody에서는 헤더 뒤의 본문만 담는다.
    std::string received;


    // 처리 중인 요청의 헤더. kReceivingBody와 kWaitingForFile 단계에서만 값이 있다.
    //
    // kReceivingBody에서는 content_length가 항상 있다. kWaitingForFile에서는 결과가 도착했을 때
    // 요청 로그를 남기는 데 쓴다.
    std::optional<HttpRequest> request;


    // 본문을 받은 뒤 호출할 핸들러. kReceivingBody 단계에서만 값이 있으며 서버의 핸들러 표를 가리킨다.
    HttpPostHandler* post_handler = nullptr;


    // 보낼 응답 바이트. kSendingResponse 단계에서만 쓴다.
    std::string response;


    // response 중 이미 보낸 바이트 수
    std::size_t sent_size = 0;
};


std::expected<std::unique_ptr<HttpServer>, std::error_code> HttpServer::Start(
    core::EventLoop& loop, HttpServerConfig config, HttpEndpoints endpoints,
    FileReader::ReadFunction read_function) {
    auto listen_socket = OpenListenSocket(config.port);
    if (!listen_socket) {
        return std::unexpected(listen_socket.error());
    }

    // 핸들러가 서버 주소를 캡처하므로, 주소가 고정되는 힙 객체를 만든 뒤 등록한다.
    std::unique_ptr<HttpServer> server(new HttpServer(loop, std::move(config), std::move(endpoints),
                                                      *std::move(listen_socket), std::move(read_function)));
    HttpServer* const server_address = server.get();
    const std::error_code add_error =
        loop.Add(server->listen_socket_.Get(), EPOLLIN,
                 [server_address](std::uint32_t) { server_address->AcceptConnections(); });
    if (add_error) {
        return std::unexpected(add_error);
    }
    return server;
}


HttpServer::HttpServer(core::EventLoop& loop, HttpServerConfig config, HttpEndpoints endpoints,
                       core::UniqueFd listen_socket, FileReader::ReadFunction read_function)
    : loop_(loop),
      config_(std::move(config)),
      endpoints_(std::move(endpoints)),
      listen_socket_(std::move(listen_socket)),
      lifetime_marker_(std::make_shared<char>()) {
    // 결과 처리 함수는 파일 읽기 스레드가 복사해 루프 작업에 담으므로, 서버가 소멸한 뒤에 실행될 수 있다.
    // 서버 소멸과 루프 작업 실행은 둘 다 루프 스레드에서 일어나므로 만료 확인과 소멸이 겹치지 않는다.
    const std::weak_ptr<char> server_lifetime = lifetime_marker_;
    file_reader_ = std::make_unique<FileReader>(
        loop_,
        [server_lifetime, this](FileReadResult result) {
            if (server_lifetime.expired()) {
                return;
            }
            HandleFileReadResult(std::move(result));
        },
        std::move(read_function));
}


HttpServer::~HttpServer() {
    // 파일 읽기 스레드를 먼저 멈춰 새 결과가 루프에 넘어오지 않게 한 뒤, 이미 넘어온 결과 전달 루프 작업이
    // 서버 주소를 쓰지 않도록 수명 표시를 파괴한다.
    file_reader_.reset();
    lifetime_marker_.reset();

    for (const auto& [socket_fd, connection] : connections_) {
        loop_.Remove(socket_fd);
    }
    loop_.Remove(listen_socket_.Get());
}


void HttpServer::AcceptConnections() {
    while (true) {
        core::UniqueFd socket(
            ::accept4(listen_socket_.Get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
        if (!socket.IsValid()) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::println(stderr, "연결 수락 실패: {}", core::LastSystemError().message());
            }
            return;
        }

        const int socket_fd = socket.Get();
        const std::error_code add_error =
            loop_.Add(socket_fd, EPOLLIN, [this, socket_fd](std::uint32_t events) {
                HandleConnectionEvent(socket_fd, events);
            });
        if (add_error) {
            std::println(stderr, "연결 등록 실패: {}", add_error.message());
            continue;
        }

        auto connection = std::make_unique<Connection>();
        connection->socket = std::move(socket);
        connection->id = ++last_connection_id_;
        connections_.emplace(socket_fd, std::move(connection));
    }
}


void HttpServer::HandleConnectionEvent(int socket_fd, std::uint32_t events) {
    const auto found = connections_.find(socket_fd);
    if (found == connections_.end()) {
        return;
    }
    Connection& connection = *found->second;

    // EPOLLHUP, EPOLLERR는 감시 이벤트와 무관하게 전달되며,
    // 이어지는 recv/send가 실패를 반환하므로 현재 단계의 처리로 함께 다룬다.
    ConnectionAction action = ConnectionAction::kKeepOpen;
    switch (connection.state) {
        case Connection::State::kReceivingHeader:
        case Connection::State::kReceivingBody:
            action = ReceiveRequest(connection);
            break;
        case Connection::State::kWaitingForFile:
            // 감시 이벤트가 0이므로 EPOLLHUP·EPOLLERR만 온다. 클라이언트가 연결을 끊었으므로 결과를 기다리지
            // 않고 닫는다. 이미 넣은 파일 읽기 작업의 결과는 도착했을 때 HandleFileReadResult가 버린다.
            action = ConnectionAction::kClose;
            break;
        case Connection::State::kSendingResponse:
            if ((events & (EPOLLOUT | EPOLLHUP | EPOLLERR)) != 0) {
                action = SendResponse(connection);
            }
            break;
    }

    if (action == ConnectionAction::kClose) {
        CloseConnection(socket_fd);
    }
}


HttpServer::ConnectionAction HttpServer::ReceiveRequest(Connection& connection) {
    std::array<char, kReceiveChunkSize> chunk;
    const ssize_t received_size = ::recv(connection.socket.Get(), chunk.data(), chunk.size(), 0);
    if (received_size < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return ConnectionAction::kKeepOpen;
        }
        return ConnectionAction::kClose;
    }

    // 요청을 끝까지 보내기 전에 피어가 연결을 닫았으므로 응답할 대상이 없다.
    if (received_size == 0) {
        return ConnectionAction::kClose;
    }
    connection.received.append(chunk.data(), static_cast<std::size_t>(received_size));

    if (connection.state == Connection::State::kReceivingHeader) {
        const auto header_end = FindRequestHeaderEnd(connection.received);
        if (!header_end) {
            std::println(stderr, "- - {}", StatusCodeNumber(header_end.error()));
            return StartSendingResponse(connection, BuildErrorResponse(header_end.error()));
        }
        if (!*header_end) {
            return ConnectionAction::kKeepOpen;
        }

        auto request =
            ParseRequestHeader(std::string_view(connection.received).substr(0, **header_end));
        if (!request) {
            std::println(stderr, "- - {}", StatusCodeNumber(request.error()));
            return StartSendingResponse(connection, BuildErrorResponse(request.error()));
        }
        RouteDecision decision = RouteRequestHeader(*request);
        if (decision.immediate_response) {
            return StartSendingResponse(connection, *std::move(decision.immediate_response));
        }
        if (decision.file_response) {
            return StartWaitingForFile(connection, *std::move(request), *std::move(decision.file_response));
        }

        // 헤더와 함께 도착한 본문 바이트만 남기고, 이후 수신분을 이어 붙여 본문 완성을 판정한다.
        connection.received.erase(0, **header_end);
        connection.request = *std::move(request);
        connection.post_handler = decision.post_handler;
        connection.state = Connection::State::kReceivingBody;
    }

    const std::size_t body_size = *connection.request->content_length;
    if (connection.received.size() < body_size) {
        return ConnectionAction::kKeepOpen;
    }
    const std::string_view body = std::string_view(connection.received).substr(0, body_size);
    return StartSendingResponse(connection,
                                CallPostHandler(*connection.post_handler, *connection.request, body));
}


HttpServer::ConnectionAction HttpServer::SendResponse(Connection& connection) {
    const std::string_view unsent = std::string_view(connection.response).substr(connection.sent_size);

    // MSG_NOSIGNAL: 피어가 닫은 소켓에 써도 SIGPIPE로 프로세스가 종료되지 않고 EPIPE를 받는다.
    const ssize_t sent_size = ::send(connection.socket.Get(), unsent.data(), unsent.size(),
                                     MSG_NOSIGNAL);
    if (sent_size < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return ConnectionAction::kKeepOpen;
        }
        return ConnectionAction::kClose;
    }

    connection.sent_size += static_cast<std::size_t>(sent_size);
    if (connection.sent_size < connection.response.size()) {
        return ConnectionAction::kKeepOpen;
    }
    return ConnectionAction::kClose;
}


HttpServer::ConnectionAction HttpServer::StartSendingResponse(Connection& connection,
                                                              std::string response) {
    connection.received.clear();
    connection.request.reset();
    connection.post_handler = nullptr;
    connection.response = std::move(response);
    connection.sent_size = 0;
    connection.state = Connection::State::kSendingResponse;

    // 송신 단계에서는 더 이상 요청을 읽지 않으므로 쓰기 가능 이벤트만 기다린다.
    if (loop_.Modify(connection.socket.Get(), EPOLLOUT)) {
        return ConnectionAction::kClose;
    }
    return ConnectionAction::kKeepOpen;
}


HttpServer::ConnectionAction HttpServer::StartWaitingForFile(Connection& connection, HttpRequest request,
                                                             HttpFileResponse file) {
    connection.received.clear();
    connection.state = Connection::State::kWaitingForFile;
    if (loop_.Modify(connection.socket.Get(), 0)) {
        return ConnectionAction::kClose;
    }

    file_reader_->Push(FileReadTask{.socket_fd = connection.socket.Get(),
                                    .connection_id = connection.id,
                                    .path = std::move(file.path),
                                    .content_type = std::move(file.content_type),
                                    .range = SelectRequestedRange(request)});
    connection.request = std::move(request);
    return ConnectionAction::kKeepOpen;
}


void HttpServer::HandleFileReadResult(FileReadResult result) {
    const auto found = connections_.find(result.socket_fd);
    if (found == connections_.end()) {
        return;
    }
    Connection& connection = *found->second;
    // 기다리던 연결이 닫히고 같은 fd 번호를 새 연결이 받은 경우다.
    if (connection.id != result.connection_id || connection.state != Connection::State::kWaitingForFile) {
        return;
    }

    LogRequest(*connection.request, result.status);
    if (StartSendingResponse(connection, std::move(result.response)) == ConnectionAction::kClose) {
        CloseConnection(result.socket_fd);
    }
}


HttpServer::RouteDecision HttpServer::RouteRequestHeader(const HttpRequest& request) {
    // 오류 상태 코드로 응답 바이트를 만들고 로그를 남긴다.
    const auto respond_error = [&](HttpStatus status, std::span<const HttpHeader> extra_headers = {}) {
        LogRequest(request, status);
        return RouteDecision{.immediate_response = BuildErrorResponse(status, extra_headers),
                             .post_handler = nullptr,
                             .file_response = std::nullopt};
    };

    const std::string path(RequestPath(request.target));
    const auto found = endpoints_.post.find(path);
    const bool is_post_endpoint = found != endpoints_.post.end();
    const std::string_view allowed_method = is_post_endpoint ? "POST" : "GET";
    if (request.method != allowed_method) {
        const std::array allow_header = {HttpHeader{"Allow", allowed_method}};
        return respond_error(HttpStatus::kMethodNotAllowed, allow_header);
    }
    if (request.has_transfer_encoding) {
        return respond_error(HttpStatus::kNotImplemented);
    }
    if (request.content_length && *request.content_length > kMaxRequestBodySize) {
        return respond_error(HttpStatus::kContentTooLarge);
    }

    // GET 요청에 붙은 본문은 쓰지 않으므로 받지 않고 바로 응답한다.
    if (!is_post_endpoint) {
        const auto get_handler = endpoints_.get.find(path);
        return get_handler != endpoints_.get.end() ? CallGetHandler(get_handler->second, request)
                                                   : RouteStaticFile(request);
    }
    if (!request.content_length) {
        return respond_error(HttpStatus::kLengthRequired);
    }
    return RouteDecision{.immediate_response = std::nullopt, .post_handler = &found->second, .file_response = std::nullopt};
}


HttpServer::RouteDecision HttpServer::RouteStaticFile(const HttpRequest& request) const {
    auto file_path = ResolveStaticFilePath(config_.web_root, request.target);
    if (!file_path) {
        LogRequest(request, file_path.error());
        return RouteDecision{.immediate_response = BuildErrorResponse(file_path.error()),
                             .post_handler = nullptr,
                             .file_response = std::nullopt};
    }

    std::string content_type(ContentTypeForPath(*file_path));
    return RouteDecision{.immediate_response = std::nullopt,
                         .post_handler = nullptr,
                         .file_response = HttpFileResponse{.path = *std::move(file_path),
                                                           .content_type = std::move(content_type)}};
}


HttpEndpointResponse HttpEndpointResponse::Error(HttpStatus status) {
    return HttpEndpointResponse{
        .status = status, .content_type = std::string(kErrorContentType), .body = BuildErrorBody(status)};
}


std::string HttpServer::CallPostHandler(HttpPostHandler& handler, const HttpRequest& request,
                                        std::string_view body) {
    const HttpEndpointResponse result = handler(request, body);
    LogRequest(request, result.status);
    return BuildResponse(result.status, result.content_type, result.body);
}


HttpServer::RouteDecision HttpServer::CallGetHandler(HttpGetHandler& handler, const HttpRequest& request) {
    HttpGetResult result = handler(request);
    if (auto* file = std::get_if<HttpFileResponse>(&result)) {
        return RouteDecision{
            .immediate_response = std::nullopt, .post_handler = nullptr, .file_response = std::move(*file)};
    }

    const auto& response = std::get<HttpEndpointResponse>(result);
    LogRequest(request, response.status);
    return RouteDecision{.immediate_response = BuildResponse(response.status, response.content_type, response.body),
                         .post_handler = nullptr,
                         .file_response = std::nullopt};
}


void HttpServer::CloseConnection(int socket_fd) {
    loop_.Remove(socket_fd);
    connections_.erase(socket_fd);
}


}  // namespace vms::http
