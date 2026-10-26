#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

#include "core/event_loop.h"
#include "core/unique_fd.h"


namespace vms::http {


// HTTP 서버 실행 설정.
struct HttpServerConfig {
    // 연결을 받을 TCP 포트. 모든 네트워크 인터페이스(0.0.0.0)에서 받는다.
    std::uint16_t port = 8080;


    // 정적 파일을 제공할 디렉터리
    std::filesystem::path web_root;
};


// 이벤트 루프 위에서 TCP 연결을 받아 요청 하나에 응답하고 연결을 닫는 HTTP/1.1 서버.
//
// GET 요청에는 웹 루트의 정적 파일로, 그 밖의 요청에는 오류 상태 코드로 응답한다.
// 리스닝 소켓과 모든 연결 소켓을 소유하며, 소멸할 때 루프 등록을 해제하고 닫는다.
// 루프 핸들러가 이 객체의 주소를 캡처하므로 std::unique_ptr로만 다루며 이동하지 않는다.
// 연결된 EventLoop보다 먼저 소멸해야 한다.
class HttpServer {
public:
    // 리스닝 소켓을 열고 루프에 등록해 연결을 받기 시작한다.
    //
    // loop: 소켓을 등록할 이벤트 루프. 반환된 서버보다 오래 살아 있어야 한다.
    // config: 포트와 웹 루트. web_root는 존재하는 디렉터리라고 가정한다.
    // 반환값: 시작된 서버. 소켓 생성, bind(포트 사용 중 등), listen, 등록에 실패하면 시스템 오류 코드
    static std::expected<std::unique_ptr<HttpServer>, std::error_code> Start(
        core::EventLoop& loop, HttpServerConfig config);


    // 모든 소켓의 루프 등록을 해제하고 닫는다. 처리 중이던 응답은 보내지 않고 버린다.
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


    // 열린 리스닝 소켓으로 서버를 만든다. Start()에서만 호출한다.
    //
    // loop: 소켓을 등록할 이벤트 루프
    // config: 실행 설정
    // listen_socket: 소유권을 넘겨받을 리스닝 소켓
    HttpServer(core::EventLoop& loop, HttpServerConfig config, core::UniqueFd listen_socket);


    // 대기 중인 연결을 모두 받아 루프에 등록한다.
    //
    // 리스닝 소켓이 읽기 가능할 때 호출된다.
    void AcceptConnections();


    // 연결 소켓의 이벤트를 상태에 맞게 처리하고, 처리가 끝났으면 연결을 닫는다.
    //
    // socket_fd: 이벤트가 발생한 연결 소켓
    // events: 발생한 epoll 이벤트 비트
    void HandleConnectionEvent(int socket_fd, std::uint32_t events);


    // 요청 데이터를 읽고, 요청 헤더가 완성되면 응답을 준비해 송신 상태로 바꾼다.
    //
    // connection: 요청 수신 중인 연결
    // 반환값: 피어가 먼저 닫았거나 읽기 오류가 나면 kClose
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


    // 요청 헤더 영역을 파싱하고 라우팅해 응답 바이트를 만들고, 요청 로그를 남긴다.
    //
    // request_header: 빈 줄까지 포함한 요청 헤더 영역
    // 반환값: 보낼 응답 바이트
    std::string BuildResponseForRequest(std::string_view request_header) const;


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


    // 연결을 받는 리스닝 소켓
    core::UniqueFd listen_socket_;


    // 열린 연결. 키는 연결 소켓 fd이며, 값이 소켓을 소유한다.
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
};


}  // namespace vms::http
