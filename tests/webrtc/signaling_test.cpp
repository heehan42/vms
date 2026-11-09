#include "webrtc/signaling.h"

#include <stdlib.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "core/event_loop.h"
#include "core/network_interfaces.h"
#include "crypto/certificate.h"
#include "crypto/dtls_context.h"
#include "media/camera_list.h"
#include "media/video_library.h"
#include "test_support.h"
#include "webrtc/dtls_transport.h"
#include "webrtc/ice_lite_agent.h"

using vms::core::EventLoop;
using vms::crypto::Certificate;
using vms::crypto::DtlsContext;
using vms::http::HttpRequest;
using vms::http::HttpStatus;
using vms::media::CameraList;
using vms::media::VideoLibrary;
using vms::test::Expect;
using vms::webrtc::DtlsTransport;
using vms::webrtc::IceLiteAgent;
using vms::webrtc::IceLiteAgentConfig;
using vms::webrtc::Signaling;

namespace fs = std::filesystem;
using namespace std::string_view_literals;


namespace {


// SPS, PPS, IDR 슬라이스로 이루어진 프레임 하나짜리 H.264 Annex-B 바이트. 0 바이트가 있으므로 sv 리터럴로
// 길이를 정한다.
constexpr std::string_view kSingleFrameVideo =
    "\x00\x00\x01\x67\x42\xC0\x1F\xDA"
    "\x00\x00\x01\x68\xCE\x3C\x80"
    "\x00\x00\x01\x65\x88\x84\x21\x10"sv;


// first_mb_in_slice가 0인 일반 슬라이스 하나로 이루어진 프레임의 Annex-B 바이트
constexpr std::string_view kNextFrame = "\x00\x00\x01\x41\x9A\x02\x03"sv;


// "시험 \"1\".h264"의 UTF-8 바이트. offer의 name 퍼센트 인코딩 검사에 쓴다.
constexpr std::string_view kKoreanVideoId = "\xEC\x8B\x9C\xED\x97\x98 \"1\".h264";


// 파일 전체를 문자열로 읽는다.
//
// path: 읽을 파일
// 반환값: 파일 내용. 열 수 없으면 빈 문자열
std::string ReadFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), {});
}


// 지정한 대상으로 offer 요청을 처리하고 상태 코드를 돌려준다.
//
// signaling: 요청을 처리할 엔드포인트
// target: 요청 대상
// offer: 본문 SDP
// content_type: 요청의 Content-Type
// 반환값: 응답 상태 코드
HttpStatus PostOffer(Signaling& signaling, std::string_view target, std::string_view offer,
                     std::string_view content_type = "application/sdp") {
    HttpRequest request;
    request.method = "POST";
    request.target = std::string(target);
    request.content_length = offer.size();
    request.content_type = std::string(content_type);
    return signaling.HandleOffer(request, offer).status;
}


// type·name 매개변수에 따라 offer를 받아들이거나 거부하는지 검사한다.
//
// signaling: 동영상 세 개(a.h264, b.h264, kKoreanVideoId)와 카메라 두 대(front, a.h264)가 있는 엔드포인트
// offer: Chrome offer 견본
void TestOfferSourceParameter(Signaling& signaling, std::string_view offer) {
    Expect(PostOffer(signaling, "/offer?type=file&name=a.h264", offer) == HttpStatus::kOk, "목록에 있는 파일은 200");
    Expect(PostOffer(signaling, "/offer?type=file&name=%EC%8B%9C%ED%97%98%20%221%22.h264", offer) == HttpStatus::kOk,
           "encodeURIComponent로 인코딩한 한글 이름은 200");
    Expect(PostOffer(signaling, "/offer", offer) == HttpStatus::kOk, "type·name이 없으면 소스 없는 세션으로 200");
    Expect(PostOffer(signaling, "/offer?video=a.h264", offer) == HttpStatus::kOk, "이전 형식의 video는 무시해 200");
    Expect(PostOffer(signaling, "/offer?video=a.h264&video=b.h264", offer) == HttpStatus::kOk,
           "중복된 video도 무시해 200");

    Expect(PostOffer(signaling, "/offer?type=file&name=missing.h264", offer) == HttpStatus::kNotFound,
           "목록에 없는 이름은 404");
    Expect(PostOffer(signaling, "/offer?type=file&name=..%2Fsecret.h264", offer) == HttpStatus::kNotFound,
           "경로처럼 보이는 이름은 404");
    Expect(PostOffer(signaling, "/offer?type=file&name=A.h264", offer) == HttpStatus::kNotFound,
           "대소문자가 다르면 404");
    Expect(PostOffer(signaling, "/offer?type=camera&name=b.h264", offer) == HttpStatus::kNotFound,
           "파일 이름을 type=camera로 보내면 404");
    Expect(PostOffer(signaling, "/offer?type=file&name=front", offer) == HttpStatus::kNotFound,
           "카메라 이름을 type=file로 보내면 404");
    Expect(PostOffer(signaling, "/offer?type=camera&name=front", offer) == HttpStatus::kOk, "목록에 있는 카메라는 200");
    Expect(PostOffer(signaling, "/offer?type=camera&name=a.h264", offer) == HttpStatus::kOk,
           "파일과 같은 이름의 카메라도 종류로 구분해 200");
    Expect(PostOffer(signaling, "/offer?type=camera&name=missing", offer) == HttpStatus::kNotFound,
           "목록에 없는 카메라는 404");

    Expect(PostOffer(signaling, "/offer?type=disk&name=a.h264", offer) == HttpStatus::kBadRequest,
           "알 수 없는 종류는 400");
    Expect(PostOffer(signaling, "/offer?name=a.h264", offer) == HttpStatus::kBadRequest, "name만 있으면 400");
    Expect(PostOffer(signaling, "/offer?type=file", offer) == HttpStatus::kBadRequest, "type만 있으면 400");
    Expect(PostOffer(signaling, "/offer?type=file&name=%zz", offer) == HttpStatus::kBadRequest,
           "잘못된 퍼센트 인코딩은 400");
    Expect(PostOffer(signaling, "/offer?type=file&name=a.h264&name=b.h264", offer) == HttpStatus::kBadRequest,
           "중복된 name은 400");
    Expect(PostOffer(signaling, "/offer?type=file&type=file&name=a.h264", offer) == HttpStatus::kBadRequest,
           "중복된 type은 400");

    // 판정 순서: Content-Type → 소스 → SDP
    Expect(PostOffer(signaling, "/offer?type=file&name=missing.h264", offer, "text/plain") ==
               HttpStatus::kUnsupportedMediaType,
           "Content-Type을 소스보다 먼저 검사");
    Expect(PostOffer(signaling, "/offer?type=file&name=missing.h264", "hello") == HttpStatus::kNotFound,
           "소스를 SDP보다 먼저 검사");
    Expect(PostOffer(signaling, "/offer?type=file&name=a.h264", "hello") == HttpStatus::kBadRequest,
           "소스가 맞아도 SDP가 잘못되면 400");
}


}  // namespace


int main() {
    const std::string offer = ReadFile(fs::path(SIGNALING_TEST_DATA_DIR) / "chrome_offer.sdp");
    const auto certificate = Certificate::Generate();
    const auto dtls_context = certificate ? DtlsContext::Create(*certificate, DtlsTransport::VerifyPeerCertificate)
                                          : std::unexpected(certificate.error());
    auto loop = EventLoop::Create();
    if (offer.empty() || !dtls_context || !loop) {
        Expect(false, "offer 견본, DTLS 컨텍스트, 루프 준비");
        return vms::test::ExitCode();
    }

    std::error_code error;
    std::string root_template = (fs::temp_directory_path(error) / "signaling_test.XXXXXX").string();
    if (error || ::mkdtemp(root_template.data()) == nullptr) {
        Expect(false, "임시 디렉터리 생성");
        return vms::test::ExitCode();
    }
    const fs::path video_dir = root_template;
    std::ofstream(video_dir / "b.h264", std::ios::binary) << kSingleFrameVideo;
    std::ofstream(video_dir / "a.h264", std::ios::binary) << kSingleFrameVideo << kNextFrame;
    std::ofstream(video_dir / kKoreanVideoId, std::ios::binary) << kSingleFrameVideo;
    auto loaded = VideoLibrary::LoadDirectory(video_dir);
    fs::remove_all(video_dir, error);
    if (!loaded) {
        Expect(false, "동영상 목록 준비: " + loaded.error());
        return vms::test::ExitCode();
    }

    // 세션이 카메라 소스를 참조하므로 에이전트보다 먼저 만든다.
    CameraList cameras;
    cameras.Add("front", "rtsp://127.0.0.1:8554/cam");
    cameras.Add("a.h264", "rtsp://127.0.0.1:9554/cam");

    const in_addr loopback = *vms::core::ParseIpv4Address("127.0.0.1");
    auto agent = IceLiteAgent::Start(*loop, IceLiteAgentConfig{.addresses = {loopback}, .port = 0}, *dtls_context);
    if (!agent) {
        Expect(false, "에이전트 시작: " + agent.error());
        return vms::test::ExitCode();
    }

    Signaling signaling(**agent, *certificate, loaded->library, cameras);
    TestOfferSourceParameter(signaling, offer);

    const VideoLibrary empty_library;
    const CameraList empty_cameras;
    Signaling empty_signaling(**agent, *certificate, empty_library, empty_cameras);
    Expect(PostOffer(empty_signaling, "/offer", offer) == HttpStatus::kOk, "빈 목록에서도 type·name 없는 offer는 200");

    // 에이전트는 루프, DTLS 컨텍스트, 동영상 목록보다 먼저 소멸해야 한다.
    agent->reset();
    return vms::test::ExitCode();
}
