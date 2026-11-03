#include "webrtc/signaling.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <print>
#include <string>

#include "core/ascii.h"
#include "core/json.h"
#include "crypto/random.h"
#include "http/query_string.h"
#include "webrtc/sdp.h"


namespace vms::webrtc {


namespace {


// Content-Type의 미디어 타입이 application/sdp인지 확인한다.
//
// content_type: 요청의 Content-Type 원문. 예: "application/sdp; charset=utf-8"
// 반환값: ';' 앞의 미디어 타입이 대소문자를 무시하고 application/sdp이면 true
bool IsSdpContentType(std::string_view content_type) {
    const std::string_view media_type =
        core::TrimAsciiWhitespace(content_type.substr(0, content_type.find(';')));
    return core::EqualsIgnoringAsciiCase(media_type, "application/sdp");
}


// answer의 o= 줄에 쓸 세션 ID를 만든다.
//
// 반환값: 63비트 범위의 난수(양수). 난수 생성에 실패하면 오류 메시지
std::expected<std::uint64_t, std::string> GenerateSessionId() {
    std::array<std::uint8_t, 8> bytes{};
    if (auto filled = crypto::FillRandomBytes(bytes); !filled) {
        return std::unexpected(filled.error());
    }
    std::uint64_t session_id = 0;
    for (const std::uint8_t value : bytes) {
        session_id = (session_id << 8) | value;
    }
    // SDP 세션 ID는 부호 있는 64비트 정수로 다뤄지는 경우가 있어 최상위 비트를 뺀다.
    return session_id & 0x7FFFFFFFFFFFFFFFu;
}


// answer의 cname 길이. RFC 7022가 권고하는 96비트 이상의 무작위 값이 되도록 64가지 문자 16개를 쓴다.
constexpr std::size_t kCnameLength = 16;


// answer의 a=msid 스트림 ID. 세션마다 RTCPeerConnection이 따로 있으므로 모든 세션이 같은 값을 쓴다.
constexpr std::string_view kMediaStreamId = "vms";


// answer의 a=msid 트랙 ID
constexpr std::string_view kVideoTrackId = "video";


// offer의 payload type 문자열을 RTP 헤더에 넣을 정수로 바꾼다.
//
// payload_type: SelectH264Codec이 offer의 포맷 목록에서 고른 값
// 반환값: 0~127 정수. 숫자가 아니거나 범위를 벗어나면 원인 문구
std::expected<std::uint8_t, std::string> ParsePayloadType(std::string_view payload_type) {
    int value = -1;
    const auto [end, error] = std::from_chars(payload_type.data(), payload_type.data() + payload_type.size(), value);
    if (error != std::errc() || end != payload_type.data() + payload_type.size() || value < 0 || value > 127) {
        return std::unexpected(std::format("H.264 payload type이 0~127 정수가 아님: {}", payload_type));
    }
    return static_cast<std::uint8_t>(value);
}


// API의 소스 종류. 목록 원소와 offer 요청의 type 문자열에 대응한다.
enum class SourceType {
    // 동영상 파일. type 문자열은 "file"
    kFile,
    // 카메라. type 문자열은 "camera"
    kCamera,
};


// offer 요청의 type 매개변수 값을 소스 종류로 바꾼다.
//
// type: 디코딩한 type 매개변수 값
// 반환값: 대소문자까지 같은 종류. "file"·"camera"가 아니면 std::nullopt
std::optional<SourceType> ParseSourceType(std::string_view type) {
    if (type == "file") {
        return SourceType::kFile;
    }
    if (type == "camera") {
        return SourceType::kCamera;
    }
    return std::nullopt;
}


// 쿼리 매개변수 해석 오류를 로그 문구로 바꾼다.
//
// error: FindQueryParameter가 돌려준 오류
// 반환값: 오류 원인 문구
std::string_view DescribeQueryParameterError(http::QueryParameterError error) {
    return error == http::QueryParameterError::kDuplicate ? "두 번 이상 옴" : "잘못된 퍼센트 인코딩";
}


// offer 요청의 type·id 쿼리 매개변수로 세션에 보낼 소스를 찾는다.
//
// target: 요청 대상 원문
// video_library: 파일 소스를 찾을 동영상 목록
// 반환값: 찾은 파일 항목. type·id가 모두 없으면 nullptr. 해석할 수 없으면 400, 목록에 없으면 404
//   (원인을 로그에 남김)
//
// 해석 오류(인코딩·중복, 한쪽만 옴, 알 수 없는 종류)를 존재 여부보다 먼저 판정한다. 카메라 목록은 아직 없으므로
// type=camera는 항상 404다. 디코딩한 id는 목록 ID와 비교만 하고 파일 경로로 쓰지 않으므로, "../" 같은 값도
// 목록에 없어 404가 된다.
std::expected<const media::VideoEntry*, http::HttpStatus> FindRequestedSource(
    std::string_view target, const media::VideoLibrary& video_library) {
    const auto type = http::FindQueryParameter(target, "type");
    const auto id = http::FindQueryParameter(target, "id");
    if (!type || !id) {
        std::println(stderr, "offer 거부: {} 매개변수 해석 실패 ({}): {}", !type ? "type" : "id", target,
                     DescribeQueryParameterError(!type ? type.error() : id.error()));
        return std::unexpected(http::HttpStatus::kBadRequest);
    }
    if (!*type && !*id) {
        return nullptr;
    }
    if (!*type || !*id) {
        std::println(stderr, "offer 거부: type·id 중 하나만 옴 ({})", target);
        return std::unexpected(http::HttpStatus::kBadRequest);
    }

    const std::optional<SourceType> source_type = ParseSourceType(**type);
    if (!source_type) {
        std::println(stderr, "offer 거부: 알 수 없는 소스 종류 ({})", target);
        return std::unexpected(http::HttpStatus::kBadRequest);
    }

    const media::VideoEntry* const entry =
        *source_type == SourceType::kFile ? video_library.Find(**id) : nullptr;
    if (entry == nullptr) {
        std::println(stderr, "offer 거부: 목록에 없는 소스 ({})", target);
        return std::unexpected(http::HttpStatus::kNotFound);
    }
    return entry;
}


}  // namespace


Signaling::Signaling(IceLiteAgent& agent, const crypto::Certificate& certificate,
                     const media::VideoLibrary& video_library)
    : agent_(agent), certificate_(certificate), video_library_(video_library) {}


http::HttpEndpointResponse Signaling::HandleOffer(const http::HttpRequest& request,
                                                  std::string_view body) {
    if (!IsSdpContentType(request.content_type)) {
        std::println(stderr, "offer 거부: Content-Type이 application/sdp가 아님 ({})",
                     request.content_type);
        return http::HttpEndpointResponse::Error(http::HttpStatus::kUnsupportedMediaType);
    }

    // 소스 확인은 SDP 해석보다 가벼우므로 먼저 한다.
    const auto video = FindRequestedSource(request.target, video_library_);
    if (!video) {
        return http::HttpEndpointResponse::Error(video.error());
    }

    const auto description = ParseSdp(body);
    auto offer = description ? InterpretOffer(*description)
                             : std::expected<SdpOffer, std::string>(std::unexpected(description.error()));
    if (!offer) {
        std::println(stderr, "offer 거부: {}", offer.error());
        return http::HttpEndpointResponse::Error(http::HttpStatus::kBadRequest);
    }

    const auto payload_type = ParsePayloadType(offer->h264.payload_type);
    if (!payload_type) {
        std::println(stderr, "offer 거부: {}", payload_type.error());
        return http::HttpEndpointResponse::Error(http::HttpStatus::kBadRequest);
    }

    // 세션을 만든 뒤 난수 생성이 실패하면 쓰지 않을 세션이 만료될 때까지 남으므로, 난수 값을 먼저 만든다.
    const auto session_id = GenerateSessionId();
    auto cname = session_id ? crypto::GenerateIceCharString(kCnameLength)
                            : std::expected<std::string, std::string>(std::unexpected(session_id.error()));
    auto session = cname ? agent_.CreateSession(IceSessionRequest{.remote_ufrag = offer->ice_ufrag,
                                                                  .remote_fingerprint = offer->remote_fingerprint,
                                                                  .h264_payload_type = *payload_type,
                                                                  .video = *video ? &(*video)->file : nullptr,
                                                                  .video_id = *video ? (*video)->id : ""})
                         : std::expected<IceSessionParameters, std::string>(std::unexpected(cname.error()));
    if (!session) {
        std::println(stderr, "ICE 세션 생성 실패: {}", session.error());
        return http::HttpEndpointResponse::Error(http::HttpStatus::kInternalServerError);
    }

    SdpAnswerParameters parameters;
    parameters.session_id = *session_id;
    parameters.ice_ufrag = session->ice_ufrag;
    parameters.ice_pwd = session->ice_pwd;
    parameters.sha256_fingerprint = certificate_.Sha256Fingerprint();
    parameters.candidates = agent_.Candidates();
    parameters.ssrc = session->ssrc;
    parameters.cname = *std::move(cname);
    parameters.media_stream_id = kMediaStreamId;
    parameters.track_id = kVideoTrackId;

    std::println(stderr, "ICE 세션 생성: 세션 {}, 브라우저 ufrag {}, H.264 payload {}, SSRC {}, 동영상 {}",
                 session->ice_ufrag, offer->ice_ufrag, *payload_type, session->ssrc,
                 *video ? std::string_view((*video)->id) : "없음");
    return http::HttpEndpointResponse{.status = http::HttpStatus::kOk,
                                      .content_type = "application/sdp",
                                      .body = BuildSdpAnswer(*offer, parameters)};
}


http::HttpEndpointResponse Signaling::HandleSourceList() const {
    std::string body = R"({"sources":[)";
    for (const media::VideoEntry& entry : video_library_.Entries()) {
        if (&entry != &video_library_.Entries().front()) {
            body.push_back(',');
        }
        body.append(R"({"type":"file","id":)");
        core::AppendJsonString(body, entry.id);
        std::format_to(std::back_inserter(body), R"(,"frames":{}}})", entry.file.Frames().size());
    }
    body.append("]}");
    return http::HttpEndpointResponse{
        .status = http::HttpStatus::kOk, .content_type = "application/json", .body = std::move(body)};
}


}  // namespace vms::webrtc
