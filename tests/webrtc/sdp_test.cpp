#include "webrtc/sdp.h"

#include <fstream>
#include <sstream>
#include <string>

#include "test_support.h"

using vms::test::Expect;
using vms::webrtc::BuildSdpAnswer;
using vms::webrtc::IceCandidate;
using vms::webrtc::InterpretOffer;
using vms::webrtc::ParseSdp;
using vms::webrtc::SdpAnswerParameters;
using vms::webrtc::SdpOffer;


namespace {


// 실제 Windows Chrome이 수신 전용 비디오 트랜시버 하나로 만든 offer 견본을 읽는다.
//
// 반환값: 견본 파일 내용. 읽지 못하면 빈 문자열
std::string ReadChromeOffer() {
    std::ifstream file(SDP_TEST_DATA_DIR "/chrome_offer.sdp", std::ios::binary);
    std::stringstream content;
    content << file.rdbuf();
    return content.str();
}


// SDP 원문을 offer로 해석한다.
//
// text: SDP 원문
// 반환값: 해석 결과 또는 실패 원인
std::expected<SdpOffer, std::string> ParseOffer(std::string_view text) {
    const auto description = ParseSdp(text);
    if (!description) {
        return std::unexpected(description.error());
    }
    return InterpretOffer(*description);
}


// answer 생성에 쓸 고정 서버 값을 만든다.
//
// 반환값: 세션 ID, 자격 증명, fingerprint, candidate 두 개
SdpAnswerParameters MakeAnswerParameters() {
    SdpAnswerParameters parameters;
    parameters.session_id = 1234;
    parameters.ice_ufrag = "srvufrag";
    parameters.ice_pwd = "serverpasswordserverpas1";
    parameters.sha256_fingerprint = "AB:CD";
    parameters.candidates = {
        IceCandidate{.foundation = 1, .priority = 2130706431, .address = "172.27.209.81", .port = 50000},
        IceCandidate{.foundation = 2, .priority = 2130706175, .address = "127.0.0.1", .port = 50000},
    };
    return parameters;
}


// Chrome offer 견본에서 mid, ICE 자격 증명, 42e01f H.264(payload 108)를 고르는지 검사한다.
void TestInterpretsChromeOffer() {
    const std::string text = ReadChromeOffer();
    if (text.empty()) {
        Expect(false, "Chrome offer 견본을 읽어야 한다");
        return;
    }
    const auto offer = ParseOffer(text);
    if (!offer) {
        Expect(false, "Chrome offer 해석: " + offer.error());
        return;
    }
    Expect(offer->video_mid == "0", "비디오 mid");
    Expect(offer->ice_ufrag == "S68V", "브라우저 ice-ufrag");
    Expect(offer->ice_pwd == "ny4p7Qr0G/Upi0lBFPiRpASE", "브라우저 ice-pwd");
    Expect(offer->h264.payload_type == "108", "42e01f, packetization-mode=1인 H.264");
    Expect(offer->h264.format_parameters ==
               "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f",
           "fmtp 원문");
    Expect(offer->media_sections.size() == 1 && offer->media_sections[0].is_selected,
           "미디어 섹션 하나가 선택됨");
}


// 42e01f가 없으면 packetization-mode=1인 첫 H.264를, 세션 수준 ICE 자격 증명도 쓰는지 검사한다.
void TestFallsBackToFirstH264AndSessionCredentials() {
    const auto offer = ParseOffer(
        "v=0\r\na=ice-ufrag:sess\r\na=ice-pwd:sessionpassword\r\n"
        "a=fingerprint:sha-256 0E:6A:A6:4D:F1:A2:6F:95:9F:50:48:0E:6D:31:F5:BD:E5:79:D4:25:87:8B:53:45:5E:E1:DA:A8:FB:4E:50:71\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96 97 98\r\na=mid:v\r\n"
        "a=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=0;profile-level-id=42e01f\r\n"
        "a=rtpmap:97 h264/90000\r\na=fmtp:97 profile-level-id=4d001f; packetization-mode=1\r\n"
        "a=rtpmap:98 H264/90000\r\na=fmtp:98 packetization-mode=1;profile-level-id=640c1f\r\n");
    if (!offer) {
        Expect(false, "대체 H.264 선택: " + offer.error());
        return;
    }
    Expect(offer->h264.payload_type == "97", "packetization-mode=1인 첫 H.264");
    Expect(offer->ice_ufrag == "sess" && offer->ice_pwd == "sessionpassword",
           "세션 수준 ICE 자격 증명");
}


// 비디오 없음, H.264 없음, ICE 자격 증명 없음, 형식 오류를 원인과 함께 거부하는지 검사한다.
void TestRejectsUnsupportedOffers() {
    // 검사하려는 원인 외의 조건은 모두 갖추도록 fingerprint도 넣는다.
    const std::string credentials =
        "a=ice-ufrag:u\r\na=ice-pwd:p\r\n"
        "a=fingerprint:sha-256 0E:6A:A6:4D:F1:A2:6F:95:9F:50:48:0E:6D:31:F5:BD:E5:79:D4:25:87:8B:53:45:5E:E1:DA:A8:FB:4E:50:71\r\n";
    Expect(!ParseOffer("v=0\r\n" + credentials + "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:0\r\n"),
           "비디오 섹션이 없는 offer");
    Expect(!ParseOffer("v=0\r\n" + credentials +
                       "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:0\r\na=rtpmap:96 VP8/90000\r\n"),
           "H.264가 없는 offer");
    Expect(!ParseOffer("v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:0\r\n"
                       "a=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1\r\n"),
           "ICE 자격 증명이 없는 offer");
    Expect(!ParseSdp("hello"), "SDP가 아닌 본문");
    Expect(!ParseSdp("o=- 1 1 IN IP4 0.0.0.0\r\n"), "v=0으로 시작하지 않는 SDP");
    Expect(!ParseSdp("v=0\r\nm=video 9\r\n"), "항목이 부족한 m= 줄");
    Expect(!ParseSdp(""), "빈 본문");
}


// BUNDLE 그룹이 있으면 그룹에 속한 비디오 섹션만 고르는지 검사한다.
void TestSelectsVideoInBundle() {
    const auto offer = ParseOffer(
        "v=0\r\na=group:BUNDLE 1\r\na=ice-ufrag:u\r\na=ice-pwd:p\r\n"
        "a=fingerprint:sha-256 0E:6A:A6:4D:F1:A2:6F:95:9F:50:48:0E:6D:31:F5:BD:E5:79:D4:25:87:8B:53:45:5E:E1:DA:A8:FB:4E:50:71\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:0\r\n"
        "a=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 97\r\na=mid:1\r\n"
        "a=rtpmap:97 H264/90000\r\na=fmtp:97 packetization-mode=1\r\n");
    Expect(offer && offer->video_mid == "1" && offer->h264.payload_type == "97",
           "BUNDLE 그룹에 속한 비디오 섹션");
}


// Chrome offer로 만든 answer에 필수 줄이 모두 있고 줄 끝이 CRLF인지 검사한다.
void TestBuildsAnswerWithRequiredLines() {
    const auto offer = ParseOffer(ReadChromeOffer());
    if (!offer) {
        Expect(false, "answer용 offer 해석");
        return;
    }
    const std::string answer = BuildSdpAnswer(*offer, MakeAnswerParameters());

    Expect(answer.starts_with("v=0\r\no=- 1234 2 IN IP4 127.0.0.1\r\n"), "세션 ID");
    Expect(answer.contains("a=group:BUNDLE 0\r\n"), "BUNDLE 그룹");
    Expect(answer.contains("a=ice-lite\r\n"), "ice-lite");
    Expect(answer.contains("m=video 9 UDP/TLS/RTP/SAVPF 108\r\n"), "선택한 payload만 담은 m= 줄");
    Expect(answer.contains("a=mid:0\r\n"), "mid");
    Expect(answer.contains("a=sendonly\r\n"), "sendonly");
    Expect(answer.contains("a=rtcp-mux\r\n"), "rtcp-mux");
    Expect(answer.contains("a=rtpmap:108 H264/90000\r\n"), "rtpmap");
    Expect(answer.contains(
               "a=fmtp:108 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"),
           "fmtp");
    Expect(answer.contains("a=ice-ufrag:srvufrag\r\n"), "서버 ice-ufrag");
    Expect(answer.contains("a=ice-pwd:serverpasswordserverpas1\r\n"), "서버 ice-pwd");
    Expect(answer.contains("a=fingerprint:sha-256 AB:CD\r\n"), "fingerprint");
    Expect(answer.contains("a=setup:passive\r\n"), "setup:passive");
    Expect(answer.contains("a=candidate:1 1 udp 2130706431 172.27.209.81 50000 typ host\r\n"),
           "첫 번째 candidate");
    Expect(answer.contains("a=candidate:2 1 udp 2130706175 127.0.0.1 50000 typ host\r\n"),
           "두 번째 candidate");
    Expect(answer.ends_with("a=end-of-candidates\r\n"), "end-of-candidates");

    // 모든 줄이 CRLF로 끝나야 하므로, CR 없이 쓰인 LF가 없어야 한다.
    bool has_bare_line_feed = false;
    for (std::size_t index = 0; index < answer.size(); ++index) {
        if (answer[index] == '\n' && (index == 0 || answer[index - 1] != '\r')) {
            has_bare_line_feed = true;
        }
    }
    Expect(!has_bare_line_feed, "줄 끝 CRLF");
}


// 비디오 뒤의 오디오 섹션을 포트 0으로 거절하고 BUNDLE에서 빼며, 순서를 유지하는지 검사한다.
void TestRejectsExtraSectionsInAnswer() {
    const auto offer = ParseOffer(
        "v=0\r\na=group:BUNDLE 0 1\r\na=ice-ufrag:u\r\na=ice-pwd:p\r\n"
        "a=fingerprint:sha-256 0E:6A:A6:4D:F1:A2:6F:95:9F:50:48:0E:6D:31:F5:BD:E5:79:D4:25:87:8B:53:45:5E:E1:DA:A8:FB:4E:50:71\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:0\r\n"
        "a=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111 0\r\na=mid:1\r\na=rtpmap:111 opus/48000/2\r\n");
    if (!offer) {
        Expect(false, "오디오가 함께 있는 offer 해석: " + offer.error());
        return;
    }
    const std::string answer = BuildSdpAnswer(*offer, MakeAnswerParameters());
    Expect(answer.contains("a=group:BUNDLE 0\r\n"), "BUNDLE 그룹에서 오디오 제외");

    const std::size_t video = answer.find("m=video 9 ");
    const std::size_t audio = answer.find("m=audio 0 UDP/TLS/RTP/SAVPF 111 0\r\nc=IN IP4 0.0.0.0\r\na=mid:1\r\n");
    Expect(video != std::string::npos && audio != std::string::npos && video < audio,
           "오디오를 포트 0으로 거절하고 offer 순서를 유지");
}


// offer에 BUNDLE 그룹이 없으면 answer에도 넣지 않고, 비디오 섹션에 mid가 없으면 거부하는지 검사한다.
void TestFollowsOfferBundleAndRequiresMid() {
    const std::string without_bundle =
        "v=0\r\na=ice-ufrag:u\r\na=ice-pwd:p\r\n"
        "a=fingerprint:sha-256 0E:6A:A6:4D:F1:A2:6F:95:9F:50:48:0E:6D:31:F5:BD:E5:79:D4:25:87:8B:53:45:5E:E1:DA:A8:FB:4E:50:71\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:v\r\n"
        "a=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1\r\n";
    const auto offer = ParseOffer(without_bundle);
    if (!offer) {
        Expect(false, "BUNDLE 없는 offer 해석: " + offer.error());
        return;
    }
    Expect(!offer->has_bundle_group, "BUNDLE 그룹 없음으로 기록");
    const std::string answer = BuildSdpAnswer(*offer, MakeAnswerParameters());
    Expect(!answer.contains("a=group:"), "offer에 없던 BUNDLE 그룹을 answer에 넣지 않음");
    Expect(answer.contains("a=mid:v\r\n"), "offer의 mid 유지");

    Expect(!ParseOffer("v=0\r\na=ice-ufrag:u\r\na=ice-pwd:p\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
                       "a=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1\r\n"),
           "mid가 없는 비디오 섹션은 거부");
}


// fingerprint와 setup 시나리오에 쓸 offer를 만든다.
//
// session_lines: 세션 수준에 넣을 줄(ICE 자격 증명 뒤)
// media_lines: 비디오 섹션의 a=mid 뒤에 넣을 줄
// 반환값: 비디오 섹션 하나와 H.264를 갖춘 offer 원문
std::string MakeOffer(std::string_view session_lines, std::string_view media_lines) {
    return std::string("v=0\r\na=ice-ufrag:u\r\na=ice-pwd:p\r\n") + std::string(session_lines) +
           "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:0\r\n" + std::string(media_lines) +
           "a=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1\r\n";
}


// Chrome offer 견본의 fingerprint를 읽고, 소문자 값을 대문자로 정규화하며, 미디어 수준을 우선하는지 검사한다.
void TestInterpretsFingerprint() {
    const auto chrome = ParseOffer(ReadChromeOffer());
    Expect(chrome && chrome->remote_fingerprint ==
                         "0E:6A:A6:4D:F1:A2:6F:95:9F:50:48:0E:6D:31:F5:BD:E5:79:D4:25:87:8B:53:45:"
                         "5E:E1:DA:A8:FB:4E:50:71",
           "견본 Chrome offer의 fingerprint");

    const std::string lower_case =
        "0e:6a:a6:4d:f1:a2:6f:95:9f:50:48:0e:6d:31:f5:bd:e5:79:d4:25:87:8b:53:45:5e:e1:da:a8:fb:4e:50:71";
    const auto normalized = ParseOffer(MakeOffer("", "a=fingerprint:SHA-256 " + lower_case + "\r\n"));
    Expect(normalized && normalized->remote_fingerprint ==
                             "0E:6A:A6:4D:F1:A2:6F:95:9F:50:48:0E:6D:31:F5:BD:E5:79:D4:25:87:8B:53:"
                             "45:5E:E1:DA:A8:FB:4E:50:71",
           "소문자 값과 해시 이름을 대소문자 무시로 해석하고 대문자로 정규화");

    const std::string session_value =
        "11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11";
    const std::string media_value =
        "22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22";
    const auto media_first =
        ParseOffer(MakeOffer("a=fingerprint:sha-256 " + session_value + "\r\n",
                             "a=fingerprint:sha-1 AA:BB\r\na=fingerprint:sha-256 " + media_value +
                                 "\r\n"));
    Expect(media_first && media_first->remote_fingerprint == media_value,
           "미디어 수준의 첫 sha-256 줄을 우선");
    const auto session_only =
        ParseOffer(MakeOffer("a=fingerprint:sha-256 " + session_value + "\r\n", ""));
    Expect(session_only && session_only->remote_fingerprint == session_value,
           "미디어 수준에 없으면 세션 수준 값");
}


// fingerprint 없음, sha-1만 있음, 형식이 틀린 값, setup:passive를 거부하고 setup 기본값을 받아들이는지 검사한다.
void TestRejectsInvalidFingerprintAndSetup() {
    const std::string valid_fingerprint =
        "a=fingerprint:sha-256 "
        "0E:6A:A6:4D:F1:A2:6F:95:9F:50:48:0E:6D:31:F5:BD:E5:79:D4:25:87:8B:53:45:5E:E1:DA:A8:FB:4E:50:71"
        "\r\n";

    Expect(!ParseOffer(MakeOffer("", "")), "fingerprint가 없는 offer");
    Expect(!ParseOffer(MakeOffer("", "a=fingerprint:sha-1 0E:6A:A6:4D:F1:A2:6F:95:9F:50:48:0E:6D:31:"
                                     "F5:BD:E5:79:D4:25\r\n")),
           "sha-1 fingerprint만 있는 offer");
    Expect(!ParseOffer(MakeOffer("", "a=fingerprint:sha-256 0E:6A:A6\r\n")), "짧은 fingerprint 값");
    Expect(!ParseOffer(MakeOffer(
               "", "a=fingerprint:sha-256 "
                   "0E-6A:A6:4D:F1:A2:6F:95:9F:50:48:0E:6D:31:F5:BD:E5:79:D4:25:87:8B:53:45:5E:E1:DA:"
                   "A8:FB:4E:50:71\r\n")),
           "구분자가 틀린 fingerprint 값");
    Expect(!ParseOffer(MakeOffer(
               "", "a=fingerprint:sha-256 "
                   "ZZ:6A:A6:4D:F1:A2:6F:95:9F:50:48:0E:6D:31:F5:BD:E5:79:D4:25:87:8B:53:45:5E:E1:DA:"
                   "A8:FB:4E:50:71\r\n")),
           "16진수가 아닌 fingerprint 값");

    Expect(!ParseOffer(MakeOffer("", valid_fingerprint + "a=setup:passive\r\n")),
           "setup:passive offer");
    Expect(!ParseOffer(MakeOffer("a=setup:passive\r\n", valid_fingerprint)),
           "세션 수준 setup:passive offer");
    Expect(ParseOffer(MakeOffer("", valid_fingerprint + "a=setup:actpass\r\n")).has_value(),
           "setup:actpass offer");
    Expect(ParseOffer(MakeOffer("", valid_fingerprint + "a=setup:active\r\n")).has_value(),
           "setup:active offer");
    Expect(ParseOffer(MakeOffer("", valid_fingerprint)).has_value(), "setup이 없으면 active로 봄");
}


}  // namespace


int main() {
    TestInterpretsChromeOffer();
    TestFallsBackToFirstH264AndSessionCredentials();
    TestRejectsUnsupportedOffers();
    TestSelectsVideoInBundle();
    TestBuildsAnswerWithRequiredLines();
    TestRejectsExtraSectionsInAnswer();
    TestFollowsOfferBundleAndRequiresMid();
    TestInterpretsFingerprint();
    TestRejectsInvalidFingerprintAndSetup();
    return vms::test::ExitCode();
}
