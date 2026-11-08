#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <variant>

#include "core/event_loop.h"
#include "core/unique_fd.h"
#include "http/file_reader.h"
#include "http/http_request.h"
#include "http/http_response.h"


namespace vms::http {


// 요청 본문의 최대 크기(바이트). Content-Length가 이 값을 넘으면 본문을 받지 않고 413으로 응답한다.
inline constexpr std::size_t kMaxRequestBodySize = 64 * 1024;


// GET·POST 엔드포인트 핸들러가 돌려주는 응답 내용.
struct HttpEndpointResponse {
    // 응답 상태 코드
    HttpStatus status = HttpStatus::kOk;


    // 응답의 Content-Type
    std::string content_type;


    // 응답 본문. 그대로 보낸다.
    std::string body;


    // 상태 코드와 사유 문구를 본문으로 하는 오류 응답 내용을 만든다.
    //
    // status: 오류 상태 코드
    // 반환값: 정적 파일 경로의 오류 응답과 같은 형식(text/plain, "404 Not Found\n")의 응답 내용
    static HttpEndpointResponse Error(HttpStatus status);
};


// POST 엔드포인트 핸들러.
//
// 헤더를 파싱한 요청과 Content-Length만큼 받은 본문을 인자로 받는다. 본문은 호출 동안에만 유효하다.
// 이벤트 루프 스레드에서 동기적으로 호출되므로 오래 걸리는 작업을 하면 안 된다.
using HttpPostHandler =
    std::move_only_function<HttpEndpointResponse(const HttpRequest& request, std::string_view body)>;


// 요청 경로(쿼리 문자열 제외)를 키로 하는 POST 엔드포인트 핸들러 표. 예: "/offer"
//
// 서버가 시작한 뒤에는 표를 바꾸지 않으므로, 연결이 보관한 핸들러 포인터는 서버 수명 동안 유효하다.
using HttpPostHandlers = std::unordered_map<std::string, HttpPostHandler>;


// GET 엔드포인트 핸들러가 응답 본문 대신 보내라고 돌려주는 파일.
//
// 서버는 정적 파일과 같은 방식으로 파일 읽기 스레드에서 파일을 읽어 응답하며, Range 요청도 같은 규칙으로
// 처리한다.
struct HttpFileResponse {
    // 보낼 파일. 서버는 경로를 다시 검사하지 않으므로 핸들러가 경로 안전성을 확인한 값이어야 한다.
    std::filesystem::path path;


    // 응답의 Content-Type
    std::string content_type;
};


// GET 엔드포인트 핸들러의 결과. 바로 보낼 응답 내용, 또는 파일 읽기 스레드가 읽어 보낼 파일이다.
using HttpGetResult = std::variant<HttpEndpointResponse, HttpFileResponse>;


// GET 엔드포인트 핸들러.
//
// 헤더를 파싱한 요청을 인자로 받는다. 요청 대상에는 쿼리 문자열이 그대로 남아 있다.
// 이벤트 루프 스레드에서 동기적으로 호출되므로 오래 걸리는 작업을 하면 안 된다. 디스크를 읽어야 하는
// 응답은 HttpFileResponse를 돌려준다. HttpEndpointResponse를 돌려주는 함수도 그대로 쓸 수 있다.
using HttpGetHandler = std::move_only_function<HttpGetResult(const HttpRequest& request)>;


// 요청 경로(쿼리 문자열 제외)를 키로 하는 GET 엔드포인트 핸들러 표. 예: "/api/videos"
//
// 표에 있는 경로는 웹 루트에 같은 경로의 파일이 있어도 핸들러가 응답한다.
using HttpGetHandlers = std::unordered_map<std::string, HttpGetHandler>;


// 서버가 정적 파일 대신 핸들러로 응답할 엔드포인트 표.
//
// 한 경로는 get과 post 중 한쪽에만 등록해야 한다. 두 표에 모두 있으면 POST 엔드포인트로 취급한다.
// 서버가 시작한 뒤에는 표를 바꾸지 않으므로, 연결이 보관한 핸들러 포인터는 서버 수명 동안 유효하다.
struct HttpEndpoints {
    // GET 엔드포인트 핸들러 표
    HttpGetHandlers get;


    // POST 엔드포인트 핸들러 표
    HttpPostHandlers post;
};


// HTTP 서버 실행 설정.
struct HttpServerConfig {
    // 연결을 받을 TCP 포트. 모든 네트워크 인터페이스(0.0.0.0)에서 받는다.
    std::uint16_t port = 8080;


    // 정적 파일을 제공할 디렉터리
    std::filesystem::path web_root;
};


// 이벤트 루프 위에서 TCP 연결을 받아 요청 하나에 응답하고 연결을 닫는 HTTP/1.1 서버.
//
// 등록된 POST 경로는 본문을 받아 해당 핸들러로, 등록된 GET 경로는 해당 핸들러로, 그 밖의 경로는 GET
// 요청만 웹 루트의 정적 파일로 응답하고, 처리할 수 없는 요청에는 오류 상태 코드로 응답한다.
// 정적 파일과 GET 핸들러가 돌려준 HttpFileResponse(파일 응답)는 서버가 소유한 파일 읽기 스레드에서
// 열고 읽으므로, 그동안 루프 스레드는 다른 연결과 이벤트를 계속 처리한다.
// 리스닝 소켓과 모든 연결 소켓을 소유하며, 소멸할 때 루프 등록을 해제하고 닫는다.
// 루프 핸들러가 이 객체의 주소를 캡처하므로 std::unique_ptr로만 다루며 이동하지 않는다.
// 연결된 EventLoop보다 먼저 소멸해야 한다.
class HttpServer {
public:
    // 리스닝 소켓을 열고 루프에 등록해 연결을 받기 시작한다.
    //
    // loop: 소켓을 등록할 이벤트 루프. 반환된 서버보다 오래 살아 있어야 한다.
    // config: 포트와 웹 루트. web_root는 존재하는 디렉터리라고 가정한다.
    // endpoints: GET·POST 엔드포인트 핸들러 표. 서버가 소유하며, 핸들러가 참조하는 객체는
    //   서버보다 오래 살아 있어야 한다.
    // read_function: 파일 읽기 스레드가 파일 응답을 만들 때 쓰는 함수. 테스트가 파일 읽기를 멈춰 두려고
    //   바꾸며, 그 밖에는 기본값을 쓴다.
    // 반환값: 시작된 서버. 소켓 생성, bind(포트 사용 중 등), listen, 등록에 실패하면 시스템 오류 코드
    static std::expected<std::unique_ptr<HttpServer>, std::error_code> Start(
        core::EventLoop& loop, HttpServerConfig config, HttpEndpoints endpoints = {},
        FileReader::ReadFunction read_function = ReadFileForResponse);


    // 파일 읽기 스레드를 멈추고, 모든 소켓의 루프 등록을 해제하고 닫는다.
    //
    // 처리 중이던 응답은 보내지 않고 버린다. 파일 읽기 스레드가 실행 중인 작업이 있으면 그 작업이 끝날
    // 때까지 기다린다. 루프에 남은 결과 전달 루프 작업은 나중에 실행되어도 아무것도 하지 않는다.
    ~HttpServer();


    // 핸들러가 캡처한 주소를 유지해야 하므로 복사를 금지한다.
    HttpServer(const HttpServer&) = delete;


    // 핸들러가 캡처한 주소를 유지해야 하므로 복사 대입을 금지한다.
    HttpServer& operator=(const HttpServer&) = delete;


private:
    // 연결 하나의 처리 상태. 정의는 구현 파일에 있다.
    struct Connection;


    // 연결 이벤트를 처리한 뒤 연결을 유지할지 닫을지
    enum class ConnectionAction {
        // 다음 이벤트를 기다린다
        kKeepOpen,

        // 연결의 등록을 해제하고 소켓을 닫는다
        kClose,
    };


    // 열린 리스닝 소켓으로 서버를 만들고 파일 읽기 스레드를 시작한다. Start()에서만 호출한다.
    //
    // loop: 소켓을 등록할 이벤트 루프
    // config: 실행 설정
    // endpoints: 소유권을 넘겨받을 엔드포인트 핸들러 표
    // listen_socket: 소유권을 넘겨받을 리스닝 소켓
    // read_function: 파일 읽기 스레드에 넘길 실행 함수
    HttpServer(core::EventLoop& loop, HttpServerConfig config, HttpEndpoints endpoints,
               core::UniqueFd listen_socket, FileReader::ReadFunction read_function);


    // 대기 중인 연결을 모두 받아 루프에 등록한다.
    //
    // 리스닝 소켓이 읽기 가능할 때 호출된다.
    void AcceptConnections();


    // 연결 소켓의 이벤트를 상태에 맞게 처리하고, 처리가 끝났으면 연결을 닫는다.
    //
    // socket_fd: 이벤트가 발생한 연결 소켓
    // events: 발생한 epoll 이벤트 비트
    void HandleConnectionEvent(int socket_fd, std::uint32_t events);


    // 요청 데이터를 읽고, 요청(헤더와 필요한 본문)이 완성되면 응답을 준비해 송신 상태로 바꾼다.
    //
    // connection: 요청 헤더 또는 본문을 수신 중인 연결
    // 반환값: 피어가 먼저 닫았거나 읽기 오류가 나면 kClose
    //
    // 헤더가 완성되면 라우팅을 결정한다. 본문을 기다려야 하면 kReceivingBody로, 파일 응답이면
    // kWaitingForFile로 바꾸고, 바로 응답할 수 있으면(GET 엔드포인트, 조기 오류) 송신 상태로 바꾼다.
    ConnectionAction ReceiveRequest(Connection& connection);


    // 준비된 응답 중 아직 보내지 않은 부분을 보낸다.
    //
    // connection: 응답 송신 중인 연결
    // 반환값: 응답을 모두 보냈거나 송신 오류가 나면 kClose
    ConnectionAction SendResponse(Connection& connection);


    // 응답 바이트를 연결에 저장하고, 쓰기 가능 이벤트를 기다리는 송신 상태로 바꾼다.
    //
    // connection: 응답할 연결
    // response: 보낼 응답 바이트
    // 반환값: 감시 이벤트 변경에 실패하면 kClose
    ConnectionAction StartSendingResponse(Connection& connection, std::string response);


    // 파일 읽기 작업을 파일 읽기 스레드에 넣고, 결과를 기다리는 상태로 바꾼다.
    //
    // connection: 파일 응답을 보낼 연결
    // request: 파싱한 요청. 결과가 도착했을 때 요청 로그를 남기려고 연결에 보관한다.
    // file: 보낼 파일과 Content-Type
    // 반환값: 감시 이벤트 변경에 실패하면 kClose
    //
    // 기다리는 동안 감시 이벤트를 0으로 둔다. epoll은 EPOLLHUP과 EPOLLERR를 감시 이벤트와 관계없이
    // 전달하므로, 그 이벤트가 오면 결과를 기다리지 않고 연결을 닫는다.
    ConnectionAction StartWaitingForFile(Connection& connection, HttpRequest request, HttpFileResponse file);


    // 파일 읽기 스레드가 넘긴 결과를 기다리던 연결에 보낸다.
    //
    // result: 파일 읽기 결과
    //
    // 루프 스레드에서 호출된다. 결과의 fd에 해당하는 연결이 없거나, 연결 번호가 다르거나, 연결이 결과를
    // 기다리는 상태가 아니면 결과를 버리고 요청 로그도 남기지 않는다.
    void HandleFileReadResult(FileReadResult result);


    // 헤더 단계의 라우팅 결과. 세 필드 중 정확히 하나만 값이 있다.
    struct RouteDecision {
        // 본문 없이 바로 보낼 응답 바이트(GET 엔드포인트, 경로 검사 실패, 조기 오류)
        std::optional<std::string> immediate_response;


        // 본문을 받은 뒤 호출할 POST 핸들러. endpoints_.post 안의 값을 가리킨다.
        HttpPostHandler* post_handler = nullptr;


        // 파일 읽기 스레드에서 읽어 보낼 파일(정적 파일, GET 핸들러의 HttpFileResponse)
        std::optional<HttpFileResponse> file_response;
    };


    // 파싱한 요청 헤더로 라우팅을 결정한다.
    //
    // request: 파싱한 요청 헤더
    // 반환값: 바로 보낼 응답 바이트, 본문을 받은 뒤 호출할 POST 핸들러, 또는 읽어 보낼 파일
    //
    // 바로 보낼 응답을 만들면 요청 로그를 남긴다. 파일 응답의 로그는 결과가 도착했을 때 남긴다.
    // 허용 메서드는 POST 표에 있으면 POST, 그 밖에는 GET이다.
    // 판정 순서는 메서드(405), 전송 인코딩(501), 본문 크기(413), GET 엔드포인트, 정적 파일,
    // 본문 길이 유무(411)다.
    RouteDecision RouteRequestHeader(const HttpRequest& request);


    // 정적 파일 요청의 경로를 문자열로만 검사해 읽어 보낼 파일을 정한다.
    //
    // request: 정적 파일 경로를 담은 GET 요청
    // 반환값: 웹 루트 안의 파일과 확장자로 정한 Content-Type. 경로 검사에 실패하면 오류 응답 바이트이며,
    //         이때 요청 로그를 남긴다.
    RouteDecision RouteStaticFile(const HttpRequest& request) const;


    // 본문이 완성된 POST 요청을 핸들러로 처리해 응답 바이트를 만들고 요청 로그를 남긴다.
    //
    // handler: RouteRequestHeader가 고른 핸들러
    // request: 본문을 기다린 요청
    // body: Content-Length만큼의 본문
    // 반환값: 핸들러 결과로 만든 응답 바이트
    static std::string CallPostHandler(HttpPostHandler& handler, const HttpRequest& request,
                                       std::string_view body);


    // GET 요청을 핸들러로 처리한다.
    //
    // handler: RouteRequestHeader가 고른 핸들러
    // request: 핸들러 경로로 온 GET 요청
    // 반환값: 핸들러가 HttpEndpointResponse를 돌려주면 그 응답 바이트(요청 로그를 남김),
    //         HttpFileResponse를 돌려주면 읽어 보낼 파일
    static RouteDecision CallGetHandler(HttpGetHandler& handler, const HttpRequest& request);


    // 연결의 루프 등록을 해제하고 소켓을 닫는다.
    //
    // socket_fd: 닫을 연결 소켓
    //
    // 호출 후에는 해당 Connection 참조가 무효가 되므로 더 이상 사용하면 안 된다.
    void CloseConnection(int socket_fd);


    // 소켓을 등록한 이벤트 루프
    core::EventLoop& loop_;


    // 실행 설정
    HttpServerConfig config_;


    // GET·POST 엔드포인트 핸들러 표. 키는 쿼리 문자열을 뺀 요청 경로다.
    HttpEndpoints endpoints_;


    // 연결을 받는 리스닝 소켓
    core::UniqueFd listen_socket_;


    // 열린 연결. 키는 연결 소켓 fd이며, 값이 소켓을 소유한다.
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;


    // 마지막으로 발급한 연결 번호. 연결마다 1부터 1씩 늘려 붙이며 다시 쓰지 않는다.
    std::uint64_t last_connection_id_ = 0;


    // 서버가 살아 있는지 결과 전달 루프 작업이 확인하는 수명 표시.
    //
    // 루프 스레드에서만 만들고 파괴한다. 결과 처리 함수는 이 객체의 std::weak_ptr과 서버 주소를 캡처하고,
    // 실행할 때 weak_ptr이 만료되었으면 서버 주소를 쓰지 않는다. 소멸자에서 file_reader_ 다음에 파괴한다.
    std::shared_ptr<char> lifetime_marker_;


    // 파일 응답을 만드는 파일 읽기 스레드. 소멸자에서 다른 멤버보다 먼저 파괴한다.
    std::unique_ptr<FileReader> file_reader_;
};


}  // namespace vms::http
