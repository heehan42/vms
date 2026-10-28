#include "webrtc/signaling.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <print>
#include <string>

#include "core/ascii.h"
#include "crypto/random.h"
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


}  // namespace


Signaling::Signaling(IceLiteAgent& agent, const crypto::Certificate& certificate)
    : agent_(agent), certificate_(certificate) {}


http::HttpEndpointResponse Signaling::HandleOffer(const http::HttpRequest& request,
                                                  std::string_view body) {
    if (!IsSdpContentType(request.content_type)) {
        std::println(stderr, "offer 거부: Content-Type이 application/sdp가 아님 ({})",
                     request.content_type);
        return http::HttpEndpointResponse::Error(http::HttpStatus::kUnsupportedMediaType);
    }

    const auto description = ParseSdp(body);
    auto offer = description ? InterpretOffer(*description)
                             : std::expected<SdpOffer, std::string>(std::unexpected(description.error()));
    if (!offer) {
        std::println(stderr, "offer 거부: {}", offer.error());
        return http::HttpEndpointResponse::Error(http::HttpStatus::kBadRequest);
    }

    const auto session_id = GenerateSessionId();
    auto credentials = session_id ? agent_.CreateSession(offer->ice_ufrag, offer->remote_fingerprint)
                                  : std::expected<IceCredentials, std::string>(
                                        std::unexpected(session_id.error()));
    if (!credentials) {
        std::println(stderr, "ICE 세션 생성 실패: {}", credentials.error());
        return http::HttpEndpointResponse::Error(http::HttpStatus::kInternalServerError);
    }

    SdpAnswerParameters parameters;
    parameters.session_id = *session_id;
    parameters.ice_ufrag = credentials->ice_ufrag;
    parameters.ice_pwd = credentials->ice_pwd;
    parameters.sha256_fingerprint = certificate_.Sha256Fingerprint();
    parameters.candidates = agent_.Candidates();

    std::println(stderr, "ICE 세션 생성: 세션 {}, 브라우저 ufrag {}, H.264 payload {}",
                 credentials->ice_ufrag, offer->ice_ufrag, offer->h264.payload_type);
    return http::HttpEndpointResponse{.status = http::HttpStatus::kOk,
                                      .content_type = "application/sdp",
                                      .body = BuildSdpAnswer(*offer, parameters)};
}


}  // namespace vms::webrtc
