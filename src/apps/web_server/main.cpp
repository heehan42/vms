#include <signal.h>

#include <charconv>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>

#include "core/event_loop.h"
#include "core/termination_signal_watcher.h"
#include "http/http_server.h"

namespace fs = std::filesystem;

using vms::core::EventLoop;
using vms::core::TerminationSignalWatcher;
using vms::http::HttpServer;
using vms::http::HttpServerConfig;


namespace {


// 명령행 사용법
constexpr std::string_view kUsage = "사용법: vms_web_server [--port <1-65535>] [--web-root <디렉터리>]";


// 포트 문자열을 숫자로 바꾼다.
//
// text: 10진수 포트 문자열
// 반환값: 1~65535 범위의 포트. 숫자가 아니거나 범위를 벗어나면 오류 메시지
std::expected<std::uint16_t, std::string> ParsePort(std::string_view text) {
    unsigned int port = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
    if (error != std::errc() || end != text.data() + text.size() || port == 0 || port > 65535) {
        return std::unexpected(std::format("잘못된 포트: {}", text));
    }
    return static_cast<std::uint16_t>(port);
}


// 명령행 인자를 서버 설정으로 바꾼다.
//
// arguments: 프로그램 이름을 제외한 인자 목록
// 반환값: 해석 결과
//   - 서버 설정: 생략한 항목은 기본값(포트 8080, 웹 루트 ./web)이다.
//   - 오류 메시지: 알 수 없는 인자, 값이 없는 옵션, 잘못된 포트
std::expected<HttpServerConfig, std::string> ParseCommandLine(std::span<char* const> arguments) {
    HttpServerConfig config{.port = 8080, .web_root = "web"};

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view option = arguments[index];
        if (option != "--port" && option != "--web-root") {
            return std::unexpected(std::format("알 수 없는 인자: {}", option));
        }
        if (index + 1 >= arguments.size()) {
            return std::unexpected(std::format("{} 옵션에 값이 없습니다", option));
        }

        const std::string_view value = arguments[++index];
        if (option == "--port") {
            const auto port = ParsePort(value);
            if (!port) {
                return std::unexpected(port.error());
            }
            config.port = *port;
        } else {
            config.web_root = value;
        }
    }
    return config;
}


// 웹 루트가 디렉터리인지 확인하고 절대 경로로 바꾼다.
//
// web_root: 명령행에서 받은 웹 루트
// 반환값: 웹 루트의 절대 경로. 디렉터리가 아니거나 경로를 확인할 수 없으면 오류 메시지
//
// 절대 경로로 바꿔 두면 로그에 실제 위치가 나와, 실행 위치에 따른 혼동을 줄인다.
std::expected<fs::path, std::string> ResolveWebRoot(const fs::path& web_root) {
    std::error_code error;
    const fs::path absolute_root = fs::absolute(web_root, error);
    if (error) {
        return std::unexpected(std::format("웹 루트 경로 확인 실패 ({}): {}", web_root.string(),
                                           error.message()));
    }
    if (!fs::is_directory(absolute_root, error)) {
        return std::unexpected(
            std::format("웹 루트가 디렉터리가 아닙니다: {}", absolute_root.string()));
    }
    return absolute_root;
}


}  // namespace


int main(int argc, char* argv[]) {
    auto config = ParseCommandLine(std::span<char* const>(argv + 1, static_cast<std::size_t>(argc - 1)));
    if (!config) {
        std::println(stderr, "{}\n{}", config.error(), kUsage);
        return 1;
    }

    auto web_root = ResolveWebRoot(config->web_root);
    if (!web_root) {
        std::println(stderr, "{}", web_root.error());
        return 1;
    }
    config->web_root = *web_root;

    auto loop = EventLoop::Create();
    if (!loop) {
        std::println(stderr, "이벤트 루프 생성 실패: {}", loop.error().message());
        return 1;
    }

    // 종료 시그널을 받으면 루프를 멈추고, 아래의 정상 종료 경로로 이어지게 한다.
    auto signal_watcher = TerminationSignalWatcher::Start(*loop, [&loop](int signal_number) {
        std::println(stderr, "{} 수신, 서버를 종료합니다",
                     signal_number == SIGINT ? "SIGINT" : "SIGTERM");
        loop->Stop();
    });
    if (!signal_watcher) {
        std::println(stderr, "종료 시그널 설정 실패: {}", signal_watcher.error().message());
        return 1;
    }

    auto server = HttpServer::Start(*loop, *config);
    if (!server) {
        std::println(stderr, "서버 시작 실패 (포트 {}): {}", config->port, server.error().message());
        return 1;
    }
    std::println(stderr, "http://0.0.0.0:{}/ 에서 대기 중 (웹 루트: {})", config->port,
                 config->web_root.string());

    if (const std::error_code run_error = loop->Run()) {
        std::println(stderr, "이벤트 루프 오류: {}", run_error.message());
        return 1;
    }
    std::println(stderr, "서버를 종료했습니다");
    return 0;
}
