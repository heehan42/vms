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
#include <vector>

#include "core/event_loop.h"
#include "core/network_interfaces.h"
#include "core/termination_signal_watcher.h"
#include "crypto/certificate.h"
#include "crypto/dtls_context.h"
#include "http/http_server.h"
#include "webrtc/dtls_transport.h"
#include "webrtc/ice_lite_agent.h"
#include "webrtc/signaling.h"

namespace fs = std::filesystem;

using vms::core::EventLoop;
using vms::core::TerminationSignalWatcher;
using vms::crypto::Certificate;
using vms::crypto::DtlsContext;
using vms::http::HttpRequest;
using vms::http::HttpServer;
using vms::http::HttpServerConfig;
using vms::webrtc::DtlsTransport;
using vms::webrtc::IceLiteAgent;
using vms::webrtc::IceLiteAgentConfig;
using vms::webrtc::Signaling;


namespace {


// 명령행 사용법
constexpr std::string_view kUsage =
    "사용법: vms_web_server [--port <1-65535>] [--web-root <디렉터리>]\n"
    "                      [--ice-port <1-65535>] [--ice-address <IPv4 주소>]...";


// 명령행으로 받는 서버 전체의 실행 설정.
struct ServerOptions {
    // HTTP 서버 설정
    HttpServerConfig http;


    // ICE용 UDP 포트
    std::uint16_t ice_port = 50000;


    // --ice-address로 지정한 주소. 비어 있으면 인터페이스 주소를 자동으로 조회한다.
    std::vector<in_addr> ice_addresses;
};


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
//   - 서버 설정: 생략한 항목은 기본값(포트 8080, 웹 루트 ./web, ICE 포트 50000, ICE 주소 자동)이다.
//   - 오류 메시지: 알 수 없는 인자, 값이 없는 옵션, 잘못된 포트나 IPv4 주소
//
// --ice-address는 여러 번 쓸 수 있으며, 쓴 순서가 candidate 우선순위 순서가 된다.
std::expected<ServerOptions, std::string> ParseCommandLine(std::span<char* const> arguments) {
    ServerOptions options;
    options.http = HttpServerConfig{.port = 8080, .web_root = "web"};

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view option = arguments[index];
        if (option != "--port" && option != "--web-root" && option != "--ice-port" &&
            option != "--ice-address") {
            return std::unexpected(std::format("알 수 없는 인자: {}", option));
        }
        if (index + 1 >= arguments.size()) {
            return std::unexpected(std::format("{} 옵션에 값이 없습니다", option));
        }

        const std::string_view value = arguments[++index];
        if (option == "--port" || option == "--ice-port") {
            const auto port = ParsePort(value);
            if (!port) {
                return std::unexpected(port.error());
            }
            (option == "--port" ? options.http.port : options.ice_port) = *port;
        } else if (option == "--web-root") {
            options.http.web_root = value;
        } else {
            const auto address = vms::core::ParseIpv4Address(value);
            if (!address) {
                return std::unexpected(std::format("잘못된 IPv4 주소: {}", value));
            }
            options.ice_addresses.push_back(*address);
        }
    }
    return options;
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


// ICE 주소를 정한다. 지정한 주소가 있으면 그대로 쓰고, 없으면 인터페이스 주소를 조회한다.
//
// specified: --ice-address로 지정한 주소
// 반환값: 사용할 주소 목록. 조회에 실패하거나 사용할 주소가 하나도 없으면 오류 메시지
std::expected<std::vector<in_addr>, std::string> ResolveIceAddresses(
    std::vector<in_addr> specified) {
    if (!specified.empty()) {
        return specified;
    }
    auto addresses = vms::core::ListNonLoopbackIpv4Addresses();
    if (!addresses) {
        return std::unexpected(
            std::format("인터페이스 주소 조회 실패: {}", addresses.error().message()));
    }
    if (addresses->empty()) {
        return std::unexpected("루프백이 아닌 IPv4 주소가 없습니다. --ice-address로 지정하세요");
    }
    return *std::move(addresses);
}


}  // namespace


int main(int argc, char* argv[]) {
    auto options =
        ParseCommandLine(std::span<char* const>(argv + 1, static_cast<std::size_t>(argc - 1)));
    if (!options) {
        std::println(stderr, "{}\n{}", options.error(), kUsage);
        return 1;
    }

    auto web_root = ResolveWebRoot(options->http.web_root);
    if (!web_root) {
        std::println(stderr, "{}", web_root.error());
        return 1;
    }
    options->http.web_root = *web_root;

    auto ice_addresses = ResolveIceAddresses(std::move(options->ice_addresses));
    if (!ice_addresses) {
        std::println(stderr, "{}", ice_addresses.error());
        return 1;
    }

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

    // 프로세스 수명 동안 같은 인증서를 써야 모든 answer의 fingerprint가 같다.
    auto certificate = Certificate::Generate();
    if (!certificate) {
        std::println(stderr, "서버 인증서 생성 실패: {}", certificate.error());
        return 1;
    }

    // 모든 세션의 DTLS 핸드셰이크가 이 인증서로 서버를 인증하고, 브라우저 인증서는 offer의
    // fingerprint와 비교한다. 에이전트가 참조하므로 에이전트보다 먼저 만들어 나중에 소멸하게 한다.
    auto dtls_context = DtlsContext::Create(*certificate, DtlsTransport::VerifyPeerCertificate);
    if (!dtls_context) {
        std::println(stderr, "DTLS 컨텍스트 생성 실패: {}", dtls_context.error());
        return 1;
    }

    auto ice_agent = IceLiteAgent::Start(
        *loop, IceLiteAgentConfig{.addresses = *std::move(ice_addresses), .port = options->ice_port},
        *dtls_context);
    if (!ice_agent) {
        std::println(stderr, "ICE 시작 실패: {}", ice_agent.error());
        return 1;
    }
    for (const auto& candidate : (*ice_agent)->Candidates()) {
        std::println(stderr, "ICE 대기 중: udp {}:{}", candidate.address, candidate.port);
    }

    // 웹 서버의 /offer 핸들러가 같은 프로세스의 ICE-lite 에이전트를 직접 호출한다.
    // 소멸은 선언 역순이므로 HttpServer가 Signaling보다, Signaling이 에이전트와 인증서보다 먼저 사라진다.
    Signaling signaling(**ice_agent, *certificate);
    vms::http::HttpPostHandlers post_handlers;
    post_handlers.emplace("/offer", [&signaling](const HttpRequest& request, std::string_view body) {
        return signaling.HandleOffer(request, body);
    });

    auto server = HttpServer::Start(*loop, options->http, std::move(post_handlers));
    if (!server) {
        std::println(stderr, "서버 시작 실패 (포트 {}): {}", options->http.port,
                     server.error().message());
        return 1;
    }
    std::println(stderr, "http://0.0.0.0:{}/ 에서 대기 중 (웹 루트: {})", options->http.port,
                 options->http.web_root.string());

    if (const std::error_code run_error = loop->Run()) {
        std::println(stderr, "이벤트 루프 오류: {}", run_error.message());
        return 1;
    }
    std::println(stderr, "서버를 종료했습니다");
    return 0;
}
