#include "http/http_server.h"

#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <print>
#include <string>
#include <utility>

#include "core/system_error.h"
#include "http/http_request.h"
#include "http/http_response.h"
#include "http/mime_type.h"
#include "http/static_files.h"


namespace vms::http {


namespace {


// 한 번의 recv로 읽는 최대 바이트 수
constexpr std::size_t kReceiveChunkSize = 4096;


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
// 요청 수신 -> 응답 송신 -> 종료 순서로만 진행하며, 응답 하나를 보내면 연결을 닫는다.
struct HttpServer::Connection {
    // 연결의 처리 단계
    enum class State {
        // 요청 헤더 영역이 완성될 때까지 수신한다. EPOLLIN을 감시한다.
        kReceivingRequest,

        // 준비된 응답을 모두 보낼 때까지 송신한다. EPOLLOUT을 감시한다.
        kSendingResponse,
    };


    // 연결 소켓. 연결이 닫힐 때 함께 닫힌다.
    core::UniqueFd socket;


    // 현재 처리 단계
    State state = State::kReceivingRequest;


    // 지금까지 받은 요청 바이트. kReceivingRequest 단계에서만 쓴다.
    std::string received;


    // 보낼 응답 바이트. kSendingResponse 단계에서만 쓴다.
    std::string response;


    // response 중 이미 보낸 바이트 수
    std::size_t sent_size = 0;
};


std::expected<std::unique_ptr<HttpServer>, std::error_code> HttpServer::Start(
    core::EventLoop& loop, HttpServerConfig config) {
    auto listen_socket = OpenListenSocket(config.port);
    if (!listen_socket) {
        return std::unexpected(listen_socket.error());
    }

    // 핸들러가 서버 주소를 캡처하므로, 주소가 고정되는 힙 객체를 만든 뒤 등록한다.
    std::unique_ptr<HttpServer> server(
        new HttpServer(loop, std::move(config), *std::move(listen_socket)));
    HttpServer* const server_address = server.get();
    const std::error_code add_error =
        loop.Add(server->listen_socket_.Get(), EPOLLIN,
                 [server_address](std::uint32_t) { server_address->AcceptConnections(); });
    if (add_error) {
        return std::unexpected(add_error);
    }
    return server;
}


HttpServer::HttpServer(core::EventLoop& loop, HttpServerConfig config,
                       core::UniqueFd listen_socket)
    : loop_(loop), config_(std::move(config)), listen_socket_(std::move(listen_socket)) {}


HttpServer::~HttpServer() {
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
        case Connection::State::kReceivingRequest:
            action = ReceiveRequest(connection);
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

    const auto header_end = FindRequestHeaderEnd(connection.received);
    if (!header_end) {
        std::println(stderr, "- - {}", StatusCodeNumber(header_end.error()));
        return StartSendingResponse(connection, BuildErrorResponse(header_end.error()));
    }
    if (!*header_end) {
        return ConnectionAction::kKeepOpen;
    }

    const std::string_view request_header =
        std::string_view(connection.received).substr(0, **header_end);
    return StartSendingResponse(connection, BuildResponseForRequest(request_header));
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
    connection.response = std::move(response);
    connection.sent_size = 0;
    connection.state = Connection::State::kSendingResponse;

    // 송신 단계에서는 더 이상 요청을 읽지 않으므로 쓰기 가능 이벤트만 기다린다.
    if (loop_.Modify(connection.socket.Get(), EPOLLOUT)) {
        return ConnectionAction::kClose;
    }
    return ConnectionAction::kKeepOpen;
}


std::string HttpServer::BuildResponseForRequest(std::string_view request_header) const {
    const auto request = ParseRequestHeader(request_header);
    if (!request) {
        std::println(stderr, "- - {}", StatusCodeNumber(request.error()));
        return BuildErrorResponse(request.error());
    }

    // 응답 상태 코드를 정한 뒤 로그를 남기고 응답 바이트를 돌려준다.
    const auto respond = [&](HttpStatus status, std::string response) {
        std::println(stderr, "{} {} {}", request->method, request->target,
                     StatusCodeNumber(status));
        return response;
    };

    if (request->method != "GET") {
        constexpr std::array kAllowHeader = {HttpHeader{"Allow", "GET"}};
        return respond(HttpStatus::kMethodNotAllowed,
                       BuildErrorResponse(HttpStatus::kMethodNotAllowed, kAllowHeader));
    }

    const auto file_path = ResolveStaticFilePath(config_.web_root, request->target);
    if (!file_path) {
        return respond(file_path.error(), BuildErrorResponse(file_path.error()));
    }

    // 경로 확인 뒤 파일이 지워졌거나 읽기 권한이 없는 경우다.
    const auto content = ReadFileContent(*file_path);
    if (!content) {
        std::println(stderr, "파일 읽기 실패 ({}): {}", file_path->string(),
                     content.error().message());
        return respond(HttpStatus::kInternalServerError,
                       BuildErrorResponse(HttpStatus::kInternalServerError));
    }
    return respond(HttpStatus::kOk,
                   BuildResponse(HttpStatus::kOk, ContentTypeForPath(*file_path), *content));
}


void HttpServer::CloseConnection(int socket_fd) {
    loop_.Remove(socket_fd);
    connections_.erase(socket_fd);
}


}  // namespace vms::http
