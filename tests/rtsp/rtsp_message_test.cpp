#include "rtsp/rtsp_message.h"

#include <string>
#include <string_view>

#include "media/h264_sps.h"
#include "test_support.h"
#include "webrtc/sdp.h"

using vms::rtsp::BuildRtspRequest;
using vms::rtsp::ContentBaseOf;
using vms::rtsp::ParseRtspResponse;
using vms::rtsp::ParseSessionHeader;
using vms::rtsp::ParseTransportHeader;
using vms::rtsp::ResolveControlUrl;
using vms::rtsp::RtspHeader;
using vms::rtsp::SelectH264VideoTrack;
using vms::test::Expect;


namespace {


// vms-cam(gst-rtsp-server)이 DESCRIBE에 돌려주는 SDP의 주요 줄
constexpr std::string_view kCameraSdp =
    "v=0\r\n"
    "o=- 1 1 IN IP4 127.0.0.1\r\n"
    "s=Session streamed with GStreamer\r\n"
    "t=0 0\r\n"
    "a=control:*\r\n"
    "m=audio 0 RTP/AVP 0\r\n"
    "a=control:stream=9\r\n"
    "m=video 0 RTP/AVP 96\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=Z0LAHtoCgPbAWoMDA1KAAAADAIAAAB5HixdQ,aM48gA==;"
    "profile-level-id=42c01e\r\n"
    "a=control:stream=0\r\n";


// 요청 줄, CSeq, User-Agent, 추가 헤더, 빈 줄 순서로 요청을 만드는지 검사한다.
void TestBuildsRequest() {
    const RtspHeader headers[] = {{.name = "Accept", .value = "application/sdp"}};
    Expect(BuildRtspRequest("DESCRIBE", "rtsp://127.0.0.1:8554/cam", 2, headers) ==
               "DESCRIBE rtsp://127.0.0.1:8554/cam RTSP/1.0\r\nCSeq: 2\r\nUser-Agent: vms\r\n"
               "Accept: application/sdp\r\n\r\n",
           "DESCRIBE 요청 문자열");
    Expect(BuildRtspRequest("OPTIONS", "rtsp://h/cam", 1) == "OPTIONS rtsp://h/cam RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: vms\r\n\r\n",
           "헤더 없는 요청");
}


// 나뉘어 도착한 응답, 본문, 헤더 대소문자, 이어진 응답을 해석하는지 검사한다.
void TestParsesResponses() {
    const std::string response =
        "RTSP/1.0 200 OK\r\nCSeq: 2\r\ncontent-length: 5\r\nContent-Base: rtsp://h/cam/\r\n\r\nhello"
        "RTSP/1.0 200 OK\r\nCSeq: 3\r\n\r\n";
    const std::size_t first_size = response.find("RTSP/1.0", 1);

    for (std::size_t size = 0; size < first_size; ++size) {
        const auto partial = ParseRtspResponse(std::string_view(response).substr(0, size));
        if (!partial || *partial) {
            Expect(false, "끝나지 않은 응답은 미완성이어야 한다: " + std::to_string(size));
            return;
        }
    }

    const auto parsed = ParseRtspResponse(response);
    if (!parsed || !*parsed) {
        Expect(false, "완성된 응답 해석");
        return;
    }
    const auto& first = **parsed;
    Expect(first.status_code == 200 && first.reason_phrase == "OK", "상태 코드와 문구");
    Expect(first.FindHeader("cseq") == "2" && first.FindHeader("CONTENT-BASE") == "rtsp://h/cam/",
           "헤더 이름은 대소문자를 구분하지 않음");
    Expect(first.body == "hello" && first.consumed_size == first_size, "본문과 소비한 바이트 수");

    const auto second = ParseRtspResponse(std::string_view(response).substr(first.consumed_size));
    Expect(second && *second && (*second)->FindHeader("CSeq") == "3" && (*second)->body.empty(), "이어진 두 번째 응답");

    const auto not_found = ParseRtspResponse("RTSP/1.0 404 Not Found\r\nCSeq: 2\r\n\r\n");
    Expect(not_found && *not_found && (*not_found)->status_code == 404 && (*not_found)->reason_phrase == "Not Found",
           "404 응답");
}


// 크기 초과와 형식 오류를 거부하는지 검사한다.
void TestRejectsMalformedResponses() {
    Expect(!ParseRtspResponse(std::string(9000, 'a')), "헤더 끝 없이 8KiB를 넘으면 오류");
    Expect(!ParseRtspResponse("RTSP/1.0 200 OK\r\nX: " + std::string(9000, 'a') + "\r\n\r\n"), "헤더 영역이 8KiB를 넘으면 오류");
    Expect(!ParseRtspResponse("RTSP/1.0 200 OK\r\nContent-Length: 70000\r\n\r\n"), "본문이 64KiB를 넘으면 오류");
    Expect(!ParseRtspResponse("HTTP/1.1 200 OK\r\n\r\n"), "RTSP가 아닌 상태 줄");
    Expect(!ParseRtspResponse("RTSP/1.0 2x0 OK\r\n\r\n"), "숫자가 아닌 상태 코드");
    Expect(!ParseRtspResponse("RTSP/1.0 200 OK\r\nno colon\r\n\r\n"), "':' 없는 헤더 줄");
    Expect(!ParseRtspResponse("RTSP/1.0 200 OK\r\nContent-Length: -1\r\n\r\n"), "잘못된 Content-Length");
}


// Session 헤더의 timeout 있음·없음을 해석하는지 검사한다.
void TestParsesSessionHeader() {
    const auto with_timeout = ParseSessionHeader("1234ABCD;timeout=30");
    Expect(with_timeout && with_timeout->id == "1234ABCD" && with_timeout->timeout == std::chrono::seconds(30),
           "timeout 있음");
    const auto without_timeout = ParseSessionHeader("jhVjqVYqnc_K1y6a");
    Expect(without_timeout && without_timeout->id == "jhVjqVYqnc_K1y6a" &&
               without_timeout->timeout == std::chrono::seconds(60),
           "timeout 없으면 60초");
    Expect(!ParseSessionHeader(";timeout=60"), "빈 ID");
    Expect(!ParseSessionHeader("A;timeout=0"), "timeout 0");
}


// UDP 유니캐스트 Transport를 받아들이고 TCP와 멀티캐스트를 거부하는지 검사한다.
void TestParsesTransportHeader() {
    const auto udp = ParseTransportHeader(
        "RTP/AVP;unicast;client_port=40000-40001;server_port=56642-56643;ssrc=FAEC15D2;mode=\"PLAY\"");
    Expect(udp && udp->server_rtp_port == 56642 && udp->server_rtcp_port == 56643, "UDP 유니캐스트와 서버 포트");
    Expect(ParseTransportHeader("RTP/AVP/UDP;unicast;server_port=6970-6971").has_value(), "RTP/AVP/UDP");
    Expect(!ParseTransportHeader("RTP/AVP/TCP;unicast;interleaved=0-1"), "TCP interleaved 거부");
    Expect(!ParseTransportHeader("RTP/AVP;multicast;server_port=6970-6971"), "멀티캐스트 거부");
    Expect(!ParseTransportHeader("RTP/AVP;unicast;client_port=40000-40001"), "server_port 없음 거부");
}


// control URL의 *, 절대, 상대 해석과 기준 URL 선택을 검사한다.
void TestResolvesControlUrl() {
    Expect(ResolveControlUrl("rtsp://h/cam/", "*") == "rtsp://h/cam/", "*는 기준 URL");
    Expect(ResolveControlUrl("rtsp://h/cam/", "RTSP://other/x") == "RTSP://other/x", "절대 URL은 그대로");
    Expect(ResolveControlUrl("rtsp://h/cam/", "stream=0") == "rtsp://h/cam/stream=0", "끝이 /인 기준");
    Expect(ResolveControlUrl("rtsp://h/cam", "stream=0") == "rtsp://h/cam/stream=0", "끝에 /가 없는 기준");

    const auto with_base = ParseRtspResponse("RTSP/1.0 200 OK\r\nContent-Location: rtsp://l/\r\nContent-Base: rtsp://b/\r\n\r\n");
    const auto with_location = ParseRtspResponse("RTSP/1.0 200 OK\r\nContent-Location: rtsp://l/\r\n\r\n");
    const auto without = ParseRtspResponse("RTSP/1.0 200 OK\r\n\r\n");
    Expect(with_base && *with_base && ContentBaseOf(**with_base, "rtsp://r/") == "rtsp://b/", "Content-Base 우선");
    Expect(with_location && *with_location && ContentBaseOf(**with_location, "rtsp://r/") == "rtsp://l/",
           "다음은 Content-Location");
    Expect(without && *without && ContentBaseOf(**without, "rtsp://r/") == "rtsp://r/", "없으면 요청 URL");
}


// vms-cam SDP에서 H.264 트랙을 고르고, H.264가 없으면 실패하는지 검사한다.
void TestSelectsVideoTrack() {
    const auto description = vms::webrtc::ParseSdp(kCameraSdp);
    const auto track = description ? SelectH264VideoTrack(*description)
                                   : std::expected<vms::rtsp::RtspVideoTrack, std::string>(std::unexpected("SDP"));
    Expect(track && track->payload_type == 96 && track->control == "stream=0" &&
               track->sprop_parameter_sets == "Z0LAHtoCgPbAWoMDA1KAAAADAIAAAB5HixdQ,aM48gA==",
           "H.264 트랙의 payload type, control, sprop-parameter-sets");

    Expect(description && vms::rtsp::SessionControlOf(*description) == "*", "세션 수준 control");
    const auto no_session_control = vms::webrtc::ParseSdp("v=0\r\nm=video 0 RTP/AVP 96\r\na=control:track1\r\n");
    Expect(no_session_control && vms::rtsp::SessionControlOf(*no_session_control) == "*",
           "세션 수준 control이 없으면 *");

    const auto audio_only = vms::webrtc::ParseSdp("v=0\r\nm=audio 0 RTP/AVP 0\r\nm=video 0 RTP/AVP 97\r\na=rtpmap:97 VP8/90000\r\n");
    Expect(audio_only && !SelectH264VideoTrack(*audio_only), "H.264가 없으면 실패");
}


// vms-cam의 sprop-parameter-sets에서 SPS·PPS를 꺼내고, SPS에서 640x480을 읽는지 검사한다.
void TestDecodesSpropParameterSets() {
    const auto nal_units = vms::rtsp::DecodeSpropParameterSets("Z0LAHtoCgPbAWoMDA1KAAAADAIAAAB5HixdQ,aM48gA==");
    if (nal_units.size() != 2) {
        Expect(false, "SPS와 PPS 두 개");
        return;
    }
    Expect(nal_units[0][0] == 0x67 && nal_units[1] == std::vector<std::uint8_t>{0x68, 0xCE, 0x3C, 0x80},
           "SPS 헤더와 PPS 바이트");
    const auto resolution = vms::media::ParseH264SpsResolution(nal_units[0]);
    Expect(resolution && resolution->width == 640 && resolution->height == 480, "SPS의 해상도 640x480");
    Expect(!vms::media::ParseH264SpsResolution(nal_units[1]), "PPS는 SPS가 아님");

    Expect(vms::rtsp::DecodeSpropParameterSets("!!!!,aM48gA==").size() == 1, "형식이 틀린 항목은 건너뜀");
}


}  // namespace


int main() {
    TestBuildsRequest();
    TestParsesResponses();
    TestRejectsMalformedResponses();
    TestParsesSessionHeader();
    TestParsesTransportHeader();
    TestResolvesControlUrl();
    TestSelectsVideoTrack();
    TestDecodesSpropParameterSets();
    return vms::test::ExitCode();
}
