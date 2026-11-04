#include <signal.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/event_loop.h"
#include "core/json.h"
#include "core/network_interfaces.h"
#include "core/termination_signal_watcher.h"
#include "crypto/certificate.h"
#include "crypto/dtls_context.h"
#include "http/http_server.h"
#include "media/camera_list.h"
#include "media/video_library.h"
#include "rtsp/rtsp_camera_client.h"
#include "rtsp/rtsp_url.h"
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
using vms::media::CameraList;
using vms::media::VideoLibrary;
using vms::rtsp::RtspCameraClient;
using vms::rtsp::RtspCameraClientConfig;
using vms::rtsp::RtspUrl;
using vms::webrtc::DtlsTransport;
using vms::webrtc::IceLiteAgent;
using vms::webrtc::IceLiteAgentConfig;
using vms::webrtc::Signaling;


namespace {


// 명령행 사용법
constexpr std::string_view kUsage =
    "사용법: vms_web_server [--port <1-65535>] [--web-root <디렉터리>]\n"
    "                      [--ice-port <1-65535>] [--ice-address <IPv4 주소>]...\n"
    "                      [--video-dir <H.264 Annex-B 파일 디렉터리>] [--video-fps <1-120>]\n"
    "                      [--camera <이름>=rtsp://<호스트>[:<포트>][/<경로>]]...";


// --video-fps를 생략했을 때의 재생 속도
constexpr int kDefaultVideoFps = 30;


// --video-fps로 받을 수 있는 최대 재생 속도
constexpr int kMaxVideoFps = 120;


// --camera로 등록한 카메라 하나.
struct CameraOption {
    // 카메라 이름. 비어 있지 않은 UTF-8이며 다른 카메라와 겹치지 않는다.
    std::string name;


    // 해석한 카메라 URL
    RtspUrl url;
};


// 명령행으로 받는 서버 전체의 실행 설정.
struct ServerOptions {
    // HTTP 서버 설정
    HttpServerConfig http;


    // ICE용 UDP 포트
    std::uint16_t ice_port = 50000;


    // --ice-address로 지정한 주소. 비어 있으면 인터페이스 주소를 자동으로 조회한다.
    std::vector<in_addr> ice_addresses;


    // --video-dir로 지정한 동영상 디렉터리. 비어 있으면 동영상 없이 실행하며 미디어를 보내지 않는다.
    std::optional<fs::path> video_dir;


    // 동영상 재생 속도(초당 프레임 수). 목록의 모든 동영상에 적용한다.
    int video_fps = kDefaultVideoFps;


    // --camera로 등록한 카메라. 쓴 순서대로이며 목록 순서가 된다.
    std::vector<CameraOption> cameras;
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


// 재생 속도 문자열을 숫자로 바꾼다.
//
// text: 10진수 초당 프레임 수 문자열
// 반환값: 1~120 범위의 값. 숫자가 아니거나 범위를 벗어나면 오류 메시지
std::expected<int, std::string> ParseVideoFps(std::string_view text) {
    int fps = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), fps);
    if (error != std::errc() || end != text.data() + text.size() || fps < 1 || fps > kMaxVideoFps) {
        return std::unexpected(std::format("잘못된 재생 속도: {} (1~{} 정수)", text, kMaxVideoFps));
    }
    return fps;
}


// --camera 값을 카메라 이름과 URL로 나눈다.
//
// value: "<이름>=<URL>" 형식의 값
// registered: 이미 등록한 카메라. 이름 중복 검사에 쓴다.
// 반환값: 등록할 카메라. '='가 없거나, 이름이 비었거나 UTF-8이 아니거나 이미 있거나, URL이 잘못되었으면 오류 메시지
//
// 첫 '='에서 나누므로 URL에는 '='가 들어가도 된다. 카메라 이름은 파일 동영상 이름과 겹쳐도 된다.
std::expected<CameraOption, std::string> ParseCameraOption(std::string_view value,
                                                           const std::vector<CameraOption>& registered) {
    const std::size_t separator = value.find('=');
    if (separator == std::string_view::npos) {
        return std::unexpected(std::format("--camera 값은 <이름>=<URL> 형식이어야 합니다: {}", value));
    }
    const std::string_view name = value.substr(0, separator);
    if (name.empty()) {
        return std::unexpected(std::format("--camera의 카메라 이름이 비어 있습니다: {}", value));
    }
    if (!vms::core::IsValidUtf8(name)) {
        return std::unexpected("--camera의 카메라 이름이 UTF-8이 아닙니다");
    }
    if (std::ranges::any_of(registered, [name](const CameraOption& camera) { return camera.name == name; })) {
        return std::unexpected(std::format("--camera의 카메라 이름이 겹칩니다: {}", name));
    }
    auto url = vms::rtsp::ParseRtspUrl(value.substr(separator + 1));
    if (!url) {
        return std::unexpected(std::format("--camera {}의 URL 오류: {}", name, url.error()));
    }
    return CameraOption{.name = std::string(name), .url = *std::move(url)};
}


// 명령행 인자를 서버 설정으로 바꾼다.
//
// arguments: 프로그램 이름을 제외한 인자 목록
// 반환값: 해석 결과
//   - 서버 설정: 생략한 항목은 기본값(포트 8080, 웹 루트 ./web, ICE 포트 50000, ICE 주소 자동,
//     동영상 없음, 30fps, 카메라 없음)이다.
//   - 오류 메시지: 알 수 없는 인자, 값이 없는 옵션, 잘못된 포트·IPv4 주소·재생 속도·카메라
//
// --ice-address는 여러 번 쓸 수 있으며, 쓴 순서가 candidate 우선순위 순서가 된다. --camera도 여러 번 쓸 수 있으며,
// 쓴 순서가 카메라 목록 순서가 된다. 카메라 호스트 이름은 여기서 해석하지 않는다.
std::expected<ServerOptions, std::string> ParseCommandLine(std::span<char* const> arguments) {
    ServerOptions options;
    options.http = HttpServerConfig{.port = 8080, .web_root = "web"};

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view option = arguments[index];
        if (option != "--port" && option != "--web-root" && option != "--ice-port" &&
            option != "--ice-address" && option != "--video-dir" && option != "--video-fps" &&
            option != "--camera") {
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
        } else if (option == "--video-dir") {
            options.video_dir = fs::path(value);
        } else if (option == "--camera") {
            auto camera = ParseCameraOption(value, options.cameras);
            if (!camera) {
                return std::unexpected(camera.error());
            }
            options.cameras.push_back(*std::move(camera));
        } else if (option == "--video-fps") {
            const auto fps = ParseVideoFps(value);
            if (!fps) {
                return std::unexpected(fps.error());
            }
            options.video_fps = *fps;
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

    // 목록 오류는 서버를 띄우기 전에 알린다. 세션이 목록의 파일을 참조하므로 에이전트보다 먼저 만들어
    // 나중에 소멸하게 한다.
    VideoLibrary video_library;
    if (options->video_dir) {
        auto loaded = VideoLibrary::LoadDirectory(*options->video_dir);
        if (!loaded) {
            std::println(stderr, "{}", loaded.error());
            return 1;
        }
        for (const std::string& warning : loaded->warnings) {
            std::println(stderr, "경고: {}", warning);
        }
        video_library = std::move(loaded->library);
        for (const auto& entry : video_library.Entries()) {
            std::println(stderr, "동영상 로드: {}, 프레임 {}", entry.name, entry.file.Frames().size());
        }
        std::println(stderr, "동영상 목록: {}개, {}fps ({})", video_library.Entries().size(), options->video_fps,
                     options->video_dir->string());
    }

    // 카메라 호스트 이름은 getaddrinfo가 블로킹이므로 루프를 돌리기 전에 한 번만 해석한다. 세션이 카메라 소스를
    // 구독하므로 카메라 목록은 에이전트보다 먼저 만들어 나중에 소멸하게 한다.
    CameraList cameras;
    std::vector<RtspCameraClientConfig> camera_client_configs;
    for (CameraOption& camera : options->cameras) {
        const auto address = vms::rtsp::ResolveIpv4Address(camera.url.host);
        if (!address) {
            std::println(stderr, "--camera {}: {}", camera.name, address.error());
            return 1;
        }
        std::println(stderr, "카메라 등록: {}, {} (주소 {}:{})", camera.name, camera.url.request_url,
                     vms::core::FormatIpv4Address(*address), camera.url.port);
        cameras.Add(camera.name, camera.url.request_url);
        camera_client_configs.push_back(
            RtspCameraClientConfig{.camera_name = camera.name, .url = std::move(camera.url), .address = *address});
    }
    if (!cameras.Entries().empty()) {
        std::println(stderr, "카메라 목록: {}대", cameras.Entries().size());
    }

    auto loop = EventLoop::Create();
    if (!loop) {
        std::println(stderr, "이벤트 루프 생성 실패: {}", loop.error().message());
        return 1;
    }

    // 카메라 클라이언트는 루프와 카메라 소스를 참조하므로 둘보다 나중에 선언해 먼저 소멸하게 한다. 에이전트보다는
    // 나중에 소멸하지만, 에이전트가 소멸할 때 구독을 모두 해지하므로 끊김 알림이 소멸한 세션에 가지 않는다.
    std::vector<std::unique_ptr<RtspCameraClient>> camera_clients;

    // 종료 시그널을 받으면 카메라에 TEARDOWN을 보내고 루프를 멈춰, 아래의 정상 종료 경로로 이어지게 한다.
    auto signal_watcher = TerminationSignalWatcher::Start(*loop, [&loop, &camera_clients](int signal_number) {
        std::println(stderr, "{} 수신, 서버를 종료합니다",
                     signal_number == SIGINT ? "SIGINT" : "SIGTERM");
        for (const std::unique_ptr<RtspCameraClient>& client : camera_clients) {
            client->Stop();
        }
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

    auto ice_agent = IceLiteAgent::Start(*loop,
                                         IceLiteAgentConfig{.addresses = *std::move(ice_addresses),
                                                            .port = options->ice_port,
                                                            .video_fps = options->video_fps},
                                         *dtls_context);
    if (!ice_agent) {
        std::println(stderr, "ICE 시작 실패: {}", ice_agent.error());
        return 1;
    }
    for (const auto& candidate : (*ice_agent)->Candidates()) {
        std::println(stderr, "ICE 대기 중: udp {}:{}", candidate.address, candidate.port);
    }

    // 웹 서버의 /offer 핸들러가 같은 프로세스의 ICE-lite 에이전트를 직접 호출한다.
    // 소멸은 선언 역순이므로 HttpServer가 Signaling보다, Signaling이 에이전트, 인증서, 동영상·카메라 목록보다
    // 먼저 사라진다.
    Signaling signaling(**ice_agent, *certificate, video_library, cameras);
    vms::http::HttpEndpoints endpoints;
    endpoints.post.emplace("/offer", [&signaling](const HttpRequest& request, std::string_view body) {
        return signaling.HandleOffer(request, body);
    });
    endpoints.get.emplace("/api/sources", [&signaling](const HttpRequest&) { return signaling.HandleSourceList(); });

    auto server = HttpServer::Start(*loop, options->http, std::move(endpoints));
    if (!server) {
        std::println(stderr, "서버 시작 실패 (포트 {}): {}", options->http.port,
                     server.error().message());
        return 1;
    }
    std::println(stderr, "http://0.0.0.0:{}/ 에서 대기 중 (웹 루트: {})", options->http.port,
                 options->http.web_root.string());

    // 카메라 연결 실패는 시작 오류가 아니다. 클라이언트가 로그를 남기고 재연결 간격 뒤 다시 시도한다.
    for (std::size_t index = 0; index < camera_client_configs.size(); ++index) {
        camera_clients.push_back(RtspCameraClient::Create(*loop, std::move(camera_client_configs[index]),
                                                          cameras.Entries()[index]->source));
        camera_clients.back()->Start();
    }

    if (const std::error_code run_error = loop->Run()) {
        std::println(stderr, "이벤트 루프 오류: {}", run_error.message());
        return 1;
    }
    std::println(stderr, "서버를 종료했습니다");
    return 0;
}
