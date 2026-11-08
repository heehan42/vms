#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "core/event_loop.h"
#include "core/unique_fd.h"
#include "http/http_server.h"
#include "test_support.h"

using vms::core::EventLoop;
using vms::core::UniqueFd;
using vms::http::HttpEndpointResponse;
using vms::http::FileReadResult;
using vms::http::FileReadTask;
using vms::http::HttpEndpoints;
using vms::http::HttpFileResponse;
using vms::http::HttpGetResult;
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


// 서버에 연결하고 원본 요청 바이트를 보낸다.
//
// port: 서버 포트
// request: 보낼 요청 바이트
// 반환값: 요청을 보낸 클라이언트 소켓. 연결이나 송신에 실패하면 빈 UniqueFd
//
// 클라이언트 소켓은 blocking으로 연결·송신한다. 연결 요청은 커널이 받아 두므로 루프를 돌리기 전에도
// 성공한다.
UniqueFd SendRequest(std::uint16_t port, std::string_view request) {
    UniqueFd socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::send(socket.Get(), request.data(), request.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(request.size())) {
        return {};
    }
    return socket;
}


// 서버가 연결을 닫을 때까지 루프를 돌려 응답 전체를 받는다.
//
// loop: 서버가 등록된 이벤트 루프
// socket: 요청을 보낸 클라이언트 소켓. 빈 소켓이면 바로 빈 문자열을 돌려준다.
// 반환값: 받은 응답 바이트
std::string ReceiveResponse(EventLoop& loop, const UniqueFd& socket) {
    if (!socket.IsValid()) {
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


// 서버에 요청을 보내고, 서버가 연결을 닫을 때까지 루프를 돌려 응답 전체를 받는다.
//
// loop: 서버가 등록된 이벤트 루프
// port: 서버 포트
// request: 보낼 요청 바이트
// 반환값: 받은 응답 바이트. 연결에 실패하면 빈 문자열
std::string Exchange(EventLoop& loop, std::uint16_t port, std::string_view request) {
    const UniqueFd socket = SendRequest(port, request);
    return ReceiveResponse(loop, socket);
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


// 조건이 참이 될 때까지 루프를 돌린다. 1ms마다 조건을 확인한다.
//
// loop: 돌릴 이벤트 루프
// condition: 확인할 조건
// 반환값: 제한 시간(kResponseTimeout) 안에 조건이 참이 되면 true
bool RunLoopUntil(EventLoop& loop, const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + kResponseTimeout;
    bool is_satisfied = condition();
    std::function<void()> check;
    EventLoop::TimerId timer = 0;
    check = [&] {
        is_satisfied = condition();
        if (is_satisfied || std::chrono::steady_clock::now() >= deadline) {
            loop.Stop();
            return;
        }
        timer = loop.AddTimer(1ms, [&] { check(); });
    };
    if (!is_satisfied) {
        timer = loop.AddTimer(0ms, [&] { check(); });
        loop.Run();
        loop.CancelTimer(timer);
    }
    return is_satisfied;
}


// 지정한 시간 동안 루프를 돌린다.
//
// loop: 돌릴 이벤트 루프
// duration: 돌릴 시간
void RunLoopFor(EventLoop& loop, std::chrono::milliseconds duration) {
    loop.AddTimer(duration, [&loop] { loop.Stop(); });
    loop.Run();
}


// 현재 프로세스가 연 fd 수를 센다.
//
// 반환값: /proc/self/fd의 항목 수. 디렉터리를 읽는 데 쓴 fd 하나가 늘 포함되므로 비교에만 쓴다.
int CountOpenFds() {
    int count = 0;
    std::error_code error;
    for (auto iterator = fs::directory_iterator("/proc/self/fd", error); !error && iterator != fs::directory_iterator();
         iterator.increment(error)) {
        ++count;
    }
    return count;
}


// 테스트가 파일 읽기 스레드의 실행 함수를 원하는 시점까지 멈춰 두는 문.
//
// 실행 함수는 Pass()에서 문이 열릴 때까지 기다린다. 서버보다 오래 살아 있어야 한다.
class ReadGate {
public:
    // 문이 열릴 때까지 기다린 뒤 실제 파일 읽기를 실행한다. 파일 읽기 스레드에서 호출된다.
    //
    // task: 실행할 파일 읽기 작업
    // 반환값: ReadFileForResponse의 결과
    FileReadResult Pass(const FileReadTask& task) {
        {
            std::unique_lock lock(mutex_);
            ++entered_count_;
            opened_.wait(lock, [this] { return is_open_; });
        }
        return vms::http::ReadFileForResponse(task);
    }


    // 문을 열어 기다리는 실행 함수와 이후의 실행 함수를 통과시킨다.
    void Open() {
        {
            std::lock_guard lock(mutex_);
            is_open_ = true;
        }
        opened_.notify_all();
    }


    // 문을 닫아 이후의 실행 함수를 멈춘다.
    void Close() {
        std::lock_guard lock(mutex_);
        is_open_ = false;
    }


    // 지금까지 Pass()에 들어온 실행 함수의 수를 돌려준다.
    //
    // 반환값: 들어온 수. 문이 닫혀 있으면 마지막으로 들어온 실행 함수는 기다리는 중이다.
    int EnteredCount() {
        std::lock_guard lock(mutex_);
        return entered_count_;
    }


private:
    // 아래 상태를 보호하는 잠금
    std::mutex mutex_;


    // 문이 열렸음을 기다리는 실행 함수에 알린다.
    std::condition_variable opened_;


    // 문이 열려 있는지 여부. mutex_로 보호한다.
    bool is_open_ = false;


    // Pass()에 들어온 실행 함수의 수. mutex_로 보호한다.
    int entered_count_ = 0;
};


// 정적 파일의 범위 요청과 범위 무시를 검사한다.
//
// loop: 서버가 등록된 이벤트 루프
// port: 웹 루트에 크기 100인 video.bin이 있는 서버의 포트
// content: video.bin의 내용
void TestStaticFileRanges(EventLoop& loop, std::uint16_t port, const std::string& content) {
    const std::string range_response =
        Exchange(loop, port, "GET /video.bin HTTP/1.1\r\nHost: t\r\nRange: bytes=0-9\r\n\r\n");
    Expect(StatusLine(range_response) == "HTTP/1.1 206 Partial Content", "Range: bytes=0-9는 206");
    Expect(range_response.contains("\r\nContent-Range: bytes 0-9/100\r\n"), "206의 Content-Range");
    Expect(Body(range_response) == content.substr(0, 10), "206 본문은 처음 10바이트");

    const std::string whole_response = Exchange(loop, port, "GET /video.bin HTTP/1.1\r\nHost: t\r\n\r\n");
    Expect(StatusLine(whole_response) == "HTTP/1.1 200 OK", "범위 없음은 200");
    Expect(whole_response.contains("\r\nAccept-Ranges: bytes\r\n"), "200의 Accept-Ranges");
    Expect(Body(whole_response) == content, "200 본문은 파일 전체");

    const std::string if_range_response = Exchange(
        loop, port, "GET /video.bin HTTP/1.1\r\nHost: t\r\nRange: bytes=0-9\r\nIf-Range: \"x\"\r\n\r\n");
    Expect(StatusLine(if_range_response) == "HTTP/1.1 200 OK", "If-Range와 함께 온 Range는 200");
    Expect(Body(if_range_response) == content, "If-Range와 함께 온 Range는 파일 전체");

    const std::string missing_response = Exchange(loop, port, "GET /missing.bin HTTP/1.1\r\nHost: t\r\n\r\n");
    Expect(StatusLine(missing_response) == "HTTP/1.1 404 Not Found", "없는 파일은 파일 읽기 스레드에서 404");
}


// 파일 읽기가 멈춰 있는 동안 다른 연결의 GET 엔드포인트가 응답하는지 검사한다.
//
// loop: 서버가 등록된 이벤트 루프
// port: 실행 함수가 gate를 지나야 하는 서버의 포트
// gate: 닫혀 있는 문
void TestEndpointRespondsWhileReadIsBlocked(EventLoop& loop, std::uint16_t port, ReadGate& gate) {
    const int entered_before = gate.EnteredCount();
    const UniqueFd file_client = SendRequest(port, "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    Expect(RunLoopUntil(loop, [&] { return gate.EnteredCount() == entered_before + 1; }),
           "파일 읽기 작업이 실행 함수에 들어가야 한다");

    const std::string endpoint_response = Exchange(loop, port, "GET /api/test HTTP/1.1\r\nHost: t\r\n\r\n");
    Expect(Body(endpoint_response) == "{}", "파일 읽기가 멈춰 있어도 엔드포인트는 응답해야 한다");

    gate.Open();
    Expect(Body(ReceiveResponse(loop, file_client)) == "<p>index</p>", "문을 연 뒤 파일 응답을 받아야 한다");
    gate.Close();
}


// 결과를 기다리는 연결을 클라이언트가 끊으면 서버가 닫고, 나중에 도착한 결과를 같은 fd의 새 연결에
// 보내지 않는지 검사한다.
//
// loop: 서버가 등록된 이벤트 루프
// port: 실행 함수가 gate를 지나야 하는 서버의 포트
// gate: 닫혀 있는 문
void TestDropsResultOfClosedConnection(EventLoop& loop, std::uint16_t port, ReadGate& gate) {
    const int fds_before = CountOpenFds();
    const int entered_before = gate.EnteredCount();
    UniqueFd closed_client = SendRequest(port, "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    Expect(RunLoopUntil(loop, [&] { return gate.EnteredCount() == entered_before + 1; }),
           "끊을 연결의 파일 읽기 작업이 실행 함수에 들어가야 한다");

    // SO_LINGER 0으로 닫으면 RST를 보내므로 서버 소켓에 EPOLLHUP·EPOLLERR가 전달된다.
    const linger reset_on_close{.l_onoff = 1, .l_linger = 0};
    ::setsockopt(closed_client.Get(), SOL_SOCKET, SO_LINGER, &reset_on_close, sizeof(reset_on_close));
    closed_client.Reset();
    Expect(RunLoopUntil(loop, [&] { return CountOpenFds() == fds_before; }),
           "클라이언트가 끊은 연결을 서버가 닫아야 한다");

    // fd는 가장 작은 빈 번호부터 쓰이므로 새 연결은 끊긴 연결과 같은 fd 번호를 받는다.
    // 이 연결도 결과를 기다리는 상태로 만들어, 연결 번호를 확인하지 않으면 index.html을 받게 한다.
    // 실행 함수가 앞 작업에서 멈춰 있어 이 작업은 큐에서 기다리므로, 서버가 요청을 받아 큐에 넣을
    // 시간만큼 루프를 돌린다.
    const UniqueFd new_client = SendRequest(port, "GET /other.txt HTTP/1.1\r\nHost: t\r\n\r\n");
    RunLoopFor(loop, 20ms);
    gate.Open();
    Expect(Body(ReceiveResponse(loop, new_client)) == "other", "새 연결은 자기 요청의 응답만 받아야 한다");
    gate.Close();
}


// 파일 읽기가 멈춰 있는 동안 서버를 소멸시키면 실행 함수가 끝난 뒤 소멸이 끝나고,
// 이후 루프에서 남은 결과 전달 루프 작업이 실행되어도 오류가 없는지 검사한다.
//
// loop: 서버가 등록된 이벤트 루프
// port: 실행 함수가 gate를 지나야 하는 서버의 포트
// gate: 닫혀 있는 문
// server: 소멸시킬 서버
void TestDestroyWhileReadIsBlocked(EventLoop& loop, std::uint16_t port, ReadGate& gate,
                                   std::unique_ptr<HttpServer>& server) {
    const int entered_before = gate.EnteredCount();
    const UniqueFd client = SendRequest(port, "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    Expect(RunLoopUntil(loop, [&] { return gate.EnteredCount() == entered_before + 1; }),
           "파일 읽기 작업이 실행 함수에 들어가야 한다");

    std::thread opening_thread([&] {
        std::this_thread::sleep_for(30ms);
        gate.Open();
    });
    const auto started_at = std::chrono::steady_clock::now();
    server.reset();
    Expect(std::chrono::steady_clock::now() - started_at >= 20ms, "소멸자는 실행 중인 실행 함수를 기다려야 한다");
    opening_thread.join();

    RunLoopFor(loop, 20ms);
    Expect(true, "서버가 소멸한 뒤 남은 결과 전달 루프 작업을 실행해도 오류가 없어야 한다");
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
    std::ofstream(web_root / "other.txt") << "other";
    std::string video_content;
    for (int index = 0; index < 100; ++index) {
        video_content.push_back(static_cast<char>('a' + index % 26));
    }
    std::ofstream(web_root / "video.bin", std::ios::binary) << video_content;

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
    endpoints.get.emplace("/api/file", [&web_root](const HttpRequest&) -> HttpGetResult {
        return HttpFileResponse{.path = web_root / "other.txt", .content_type = "application/x-test"};
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

    const std::string file_endpoint_response = Exchange(*loop, port, "GET /api/file HTTP/1.1\r\nHost: t\r\n\r\n");
    Expect(StatusLine(file_endpoint_response) == "HTTP/1.1 200 OK", "파일 응답 핸들러 200");
    Expect(Body(file_endpoint_response) == "other", "핸들러가 돌려준 파일의 내용");
    Expect(file_endpoint_response.contains("\r\nContent-Type: application/x-test\r\n"),
           "핸들러가 정한 Content-Type");

    TestStaticFileRanges(*loop, port, video_content);
    server->reset();

    // 파일 읽기를 테스트가 정한 시점까지 멈춰 두는 서버. 문은 서버보다 먼저 만들어 나중에 파괴한다.
    ReadGate gate;
    HttpEndpoints gated_endpoints;
    gated_endpoints.get.emplace("/api/test", [](const HttpRequest&) {
        return HttpEndpointResponse{.status = HttpStatus::kOk, .content_type = "application/json", .body = "{}"};
    });
    const std::uint16_t gated_port = port + 1;
    auto gated_server = HttpServer::Start(*loop, HttpServerConfig{.port = gated_port, .web_root = web_root},
                                          std::move(gated_endpoints),
                                          [&gate](const FileReadTask& task) { return gate.Pass(task); });
    if (!gated_server) {
        Expect(false, "파일 읽기를 멈춰 두는 서버 시작");
        fs::remove_all(web_root, error);
        return vms::test::ExitCode();
    }
    TestEndpointRespondsWhileReadIsBlocked(*loop, gated_port, gate);
    TestDropsResultOfClosedConnection(*loop, gated_port, gate);
    TestDestroyWhileReadIsBlocked(*loop, gated_port, gate, *gated_server);

    fs::remove_all(web_root, error);
    return vms::test::ExitCode();
}
