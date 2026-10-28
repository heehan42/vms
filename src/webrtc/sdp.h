#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "webrtc/ice_candidate.h"


namespace vms::webrtc {


// SDP의 a= 속성 한 줄.
struct SdpAttribute {
    // 속성 이름. 예: "ice-ufrag"
    std::string name;


    // ':' 뒤의 값. 값이 없는 속성(예: "a=rtcp-mux")이면 빈 문자열이다.
    std::string value;
};


// m= 줄로 시작하는 미디어 섹션 하나.
struct SdpMediaSection {
    // 미디어 종류. 예: "video", "audio", "application"
    std::string media;


    // m= 줄의 포트. 0이면 offer 쪽이 이미 거절한 섹션이다.
    std::string port;


    // 전송 프로토콜. 예: "UDP/TLS/RTP/SAVPF"
    std::string protocol;


    // 포맷 목록. RTP 미디어에서는 payload type 번호다.
    std::vector<std::string> formats;


    // 이 섹션의 a= 속성. 나타난 순서대로다.
    std::vector<SdpAttribute> attributes;
};


// SDP 문서를 줄 구조대로 나눈 결과(문법 층).
//
// 해석하지 않는 줄과 속성도 그대로 보관하며, 의미 해석은 InterpretOffer가 맡는다.
struct SdpSessionDescription {
    // 첫 m= 줄 앞(세션 영역)의 a= 속성
    std::vector<SdpAttribute> session_attributes;


    // 미디어 섹션. offer에 나타난 순서대로다.
    std::vector<SdpMediaSection> media_sections;
};


// answer에서 받아들일 H.264 코덱.
struct H264Codec {
    // offer의 payload type 번호
    std::string payload_type;


    // offer의 a=fmtp 값 중 payload type 뒤의 매개변수 원문. answer에 그대로 넣는다.
    std::string format_parameters;
};


// offer의 미디어 섹션 하나를 answer에서 어떻게 다룰지 나타낸다.
struct OfferedMediaSection {
    // 미디어 종류
    std::string media;


    // 전송 프로토콜
    std::string protocol;


    // 포맷 목록
    std::vector<std::string> formats;


    // a=mid 값. 없으면 빈 optional
    std::optional<std::string> mid;


    // 협상 대상으로 고른 섹션인지 여부. 정확히 하나만 true이며, 나머지는 포트 0으로 거절한다.
    bool is_selected = false;
};


// offer에서 answer를 만드는 데 필요한 값만 꺼낸 결과(의미 층).
struct SdpOffer {
    // 협상 대상 비디오 섹션의 mid. 항상 비어 있지 않다.
    std::string video_mid;


    // offer에 a=group:BUNDLE이 있었는지 여부. answer는 offer가 제안한 BUNDLE 그룹만 받아들일 수 있다.
    bool has_bundle_group = false;


    // 브라우저의 ICE ufrag. 서버는 STUN USERNAME 확인에 쓴다.
    std::string ice_ufrag;


    // 브라우저의 ICE pwd. ICE-lite 서버는 쓰지 않지만 offer의 필수 값이므로 검사한다.
    std::string ice_pwd;


    // 브라우저 인증서의 SHA-256 fingerprint. 대문자 16진수 32쌍을 ':'로 구분한 형식으로 정규화했다.
    //
    // DTLS 핸드셰이크에서 받은 브라우저 인증서와 비교한다.
    std::string remote_fingerprint;


    // 협상할 H.264 코덱
    H264Codec h264;


    // offer의 모든 미디어 섹션. offer 순서를 지켜 answer를 만들어야 한다.
    std::vector<OfferedMediaSection> media_sections;
};


// answer를 만들 때 서버 쪽에서 채우는 값.
struct SdpAnswerParameters {
    // o= 줄의 세션 ID. 세션마다 다른 양수다.
    std::uint64_t session_id = 0;


    // 이 세션에 새로 만든 서버 ICE ufrag
    std::string ice_ufrag;


    // 이 세션에 새로 만든 서버 ICE pwd
    std::string ice_pwd;


    // 서버 인증서의 SHA-256 fingerprint("AB:CD:..." 형식)
    std::string sha256_fingerprint;


    // 알릴 host candidate. 우선순위가 높은 것부터 나열한다.
    std::vector<IceCandidate> candidates;
};


// SDP 문서를 줄 구조대로 나눈다.
//
// text: SDP 원문. 줄 끝은 "\r\n"과 "\n"을 모두 허용한다.
// 반환값: 나눈 결과. 첫 줄이 "v=0"이 아니거나, "<한 글자>=<값>" 형식이 아닌 줄이 있거나,
//   m= 줄의 항목이 4개보다 적으면 원인 문구
std::expected<SdpSessionDescription, std::string> ParseSdp(std::string_view text);


// 나눈 SDP를 offer로 해석해 answer에 필요한 값을 꺼낸다.
//
// description: ParseSdp의 결과
// 반환값: 해석 결과. 다음 경우에는 원인 문구
//   - 포트가 0이 아닌 비디오 섹션이 없음(BUNDLE 그룹이 있으면 그 그룹에 속한 섹션만 고른다)
//   - 고른 섹션의 프로토콜이 UDP/TLS/RTP/SAVPF가 아님
//   - 고른 섹션에 a=mid가 없음
//   - ICE ufrag 또는 pwd가 없음(미디어 수준을 우선하고, 없으면 세션 수준을 본다)
//   - sha-256 해시의 a=fingerprint가 없거나 값 형식이 틀림(미디어 수준을 우선한다)
//   - a=setup이 actpass나 active가 아님. 서버가 DTLS 서버이므로 브라우저는 DTLS 클라이언트여야 한다.
//     줄이 없으면 RFC 5763의 기본값 active로 본다.
//   - packetization-mode=1인 H.264가 없음
//
// H.264가 여럿이면 profile-level-id=42e01f(Constrained Baseline)를 우선하고, 없으면 첫 번째를 고른다.
std::expected<SdpOffer, std::string> InterpretOffer(const SdpSessionDescription& description);


// offer와 서버 값으로 SDP answer를 만든다.
//
// offer: InterpretOffer의 결과
// parameters: 서버 ICE 자격 증명, fingerprint, candidate
// 반환값: 줄 끝이 "\r\n"인 answer 원문
//
// 미디어 섹션 순서는 offer와 같다. 고른 섹션은 sendonly, rtcp-mux, setup:passive, ice-lite로
// 받아들이고, 나머지 섹션은 포트 0으로 거절한다. offer에 BUNDLE 그룹이 있었을 때만 고른 섹션 하나로
// 이루어진 BUNDLE 그룹을 넣는다.
std::string BuildSdpAnswer(const SdpOffer& offer, const SdpAnswerParameters& parameters);


}  // namespace vms::webrtc
