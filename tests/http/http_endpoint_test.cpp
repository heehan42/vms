#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "core/event_loop.h"
#include "core/unique_fd.h"
#include "http/http_server.h"
#include "test_support.h"

using vms::core::EventLoop;
using vms::core::UniqueFd;
using vms::http::HttpEndpointResponse;
using vms::http::HttpEndpoints;
using vms::http::HttpRequest;
using vms::http::HttpServer;
using vms::http::HttpServerConfig;
using vms::http::HttpStatus;
using vms::test::Expect;

namespace fs = std::filesystem;
using namespace std::chrono_literals;


namespace {


// 테스트 서버의 HTTP 포트. HTTP_ENDPOINT_TEST_PORT 환경 변수로 바꿀 수 있다.
//
// 반환값: 환경 변수 값 또는 기본값 18180
std::uint16_t TestPort() {
    const char* value = std::getenv("HTTP_ENDPOINT_TEST_PORT");
    return value != nullptr ? static_cast<std::uint16_t>(std::atoi(value)) : 18180;
}


// 응답을 기다리는 최대 시간. 서버가 응답하지 않아도 테스트가 멈추지 않게 한다.
constexpr auto kResponseTimeout = 2s;


// 서버에 원본 요청 바이트를 보내고, 서버가 연결을 닫을 때까지 루프를 돌려 응답 전체를 받는다.
//
// loop: 서버가 등록된 이벤트 루프
// port: 서버 포트
// request: 보낼 요청 바이트
// 반환값: 받은 응답 바이트. 연결에 실패하면 빈 문자열
//
// 클라이언트 소켓은 blocking으로 연결·송신한다. 연결 요청은 커널이 받아 두므로 루프를 돌리기 전에도
// 성공한다. 응답은 루프 안에서 읽는다.
std::string Exchange(EventLoop& loop, std::uint16_t port, std::string_view request) {
    UniqueFd socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::send(socket.Get(), request.data(), request.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(request.size())) {
        return {};
    }

    std::string response;
    loop.Add(socket.Get(), EPOLLIN, [&](std::uint32_t) {
        std::array<char, 4096> buffer;
        const ssize_t size = ::recv(socket.Get(), buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (size > 0) {
            response.append(buffer.data(), static_cast<std::size_t>(size));
        } else {
            loop.Stop();
        }
    });
    const EventLoop::TimerId timeout = loop.AddTimer(std::chrono::duration_cast<std::chrono::milliseconds>(kResponseTimeout),
                                                     [&loop] { loop.Stop(); });
    loop.Run();
    loop.CancelTimer(timeout);
    loop.Remove(socket.Get());
    return response;
}


// 응답의 상태 줄을 돌려준다.
//
// response: 응답 바이트
// 반환값: 첫 줄(CRLF 제외)
std::string_view StatusLine(std::string_view response) { return response.substr(0, response.find("\r\n")); }


// 응답 본문을 돌려준다.
//
// response: 응답 바이트
// 반환값: 빈 줄 뒤의 바이트. 빈 줄이 없으면 빈 문자열
std::string_view Body(std::string_view response) {
    const std::size_t header_end = response.find("\r\n\r\n");
    return header_end == std::string_view::npos ? std::string_view() : response.substr(header_end + 4);
}


}  // namespace


int main() {
    std::error_code error;
    std::string root_template = (fs::temp_directory_path(error) / "http_endpoint_test.XXXXXX").string();
    if (error || ::mkdtemp(root_template.data()) == nullptr) {
        Expect(false, "임시 웹 루트 생성");
        return vms::test::ExitCode();
    }
    const fs::path web_root = root_template;
    fs::create_directories(web_root / "api", error);
    std::ofstream(web_root / "api" / "test") << "static file";
    std::ofstream(web_root / "index.html") << "<p>index</p>";

    auto loop = EventLoop::Create();
    if (!loop) {
        Expect(false, "이벤트 루프 생성");
        return vms::test::ExitCode();
    }

    // 핸들러가 받은 요청 대상을 기록해, 쿼리 문자열이 그대로 전달되는지 확인한다.
    std::string received_target;
    HttpEndpoints endpoints;
    endpoints.get.emplace("/api/test", [&received_target](const HttpRequest& request) {
        received_target = request.target;
        return HttpEndpointResponse{.status = HttpStatus::kOk, .content_type = "application/json", .body = "{}"};
    });
    endpoints.post.emplace("/submit", [](const HttpRequest&, std::string_view body) {
        return HttpEndpointResponse{.status = HttpStatus::kOk, .content_type = "text/plain", .body = std::string(body)};
    });

    const std::uint16_t port = TestPort();
    auto server = HttpServer::Start(*loop, HttpServerConfig{.port = port, .web_root = web_root}, std::move(endpoints));
    if (!server) {
        Expect(false, "서버 시작");
        fs::remove_all(web_root, error);
        return vms::test::ExitCode();
    }

    const std::string get_response = Exchange(*loop, port, "GET /api/test?x=1 HTTP/1.1\r\nHost: t\r\n\r\n");
    Expect(StatusLine(get_response) == "HTTP/1.1 200 OK", "GET 엔드포인트 200");
    Expect(Body(get_response) == "{}", "웹 루트의 같은 경로 파일 대신 핸들러가 응답");
    Expect(get_response.find("Content-Type: application/json\r\n") != std::string::npos, "핸들러의 Content-Type");
    Expect(received_target == "/api/test?x=1", "핸들러에 쿼리를 포함한 요청 대상 전달");

    const std::string post_response =
        Exchange(*loop, port, "POST /api/test HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n");
    Expect(StatusLine(post_response) == "HTTP/1.1 405 Method Not Allowed", "GET 엔드포인트에 POST는 405");
    Expect(post_response.find("Allow: GET\r\n") != std::string::npos, "405의 Allow: GET");

    const std::string submit_response =
        Exchange(*loop, port, "POST /submit HTTP/1.1\r\nHost: t\r\nContent-Length: 2\r\n\r\nhi");
    Expect(Body(submit_response) == "hi", "POST 엔드포인트는 그대로 동작");

    const std::string static_response = Exchange(*loop, port, "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    Expect(Body(static_response) == "<p>index</p>", "등록하지 않은 경로는 정적 파일");

    server->reset();
    fs::remove_all(web_root, error);
    return vms::test::ExitCode();
}
