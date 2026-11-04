#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "webrtc/sdp.h"


namespace vms::rtsp {


// 응답 헤더 영역(상태 줄부터 빈 줄까지)의 최대 크기(바이트). 넘으면 형식 오류로 본다.
inline constexpr std::size_t kMaxRtspHeaderSize = 8 * 1024;


// 응답 본문의 최대 크기(바이트). DESCRIBE의 SDP보다 충분히 크다.
inline constexpr std::size_t kMaxRtspBodySize = 64 * 1024;


// RTSP 헤더 한 줄.
struct RtspHeader {
    // 헤더 이름. 받은 응답에서는 원문 그대로이며, 비교할 때 대소문자를 구분하지 않는다.
    std::string name;


    // ':' 뒤의 값. 앞뒤 공백을 뺀다.
    std::string value;
};


// 완성된 RTSP 응답 하나.
struct RtspResponse {
    // 상태 코드(100~999)
    int status_code = 0;


    // 상태 줄의 이유 문구. 예: "Not Found"
    std::string reason_phrase;


    // 헤더. 받은 순서대로다.
    std::vector<RtspHeader> headers;


    // Content-Length만큼의 본문. 없으면 빈 문자열
    std::string body;


    // 수신 버퍼에서 이 응답이 차지한 바이트 수. 호출자는 이만큼 버퍼 앞을 지운다.
    std::size_t consumed_size = 0;


    // 이름이 같은(대소문자 무시) 첫 헤더의 값을 찾는다.
    //
    // name: 찾을 헤더 이름
    // 반환값: 헤더 값. 없으면 빈 optional. 이 객체가 살아 있는 동안 유효하다.
    std::optional<std::string_view> FindHeader(std::string_view name) const;
};


// SETUP 응답의 Session 헤더 값.
struct RtspSession {
    // 이후 요청의 Session 헤더에 넣을 세션 ID
    std::string id;


    // 서버가 요청 없이 세션을 유지하는 시간. 헤더에 없으면 60초
    std::chrono::seconds timeout{60};
};


// SETUP 응답의 Transport 헤더에서 쓰는 값.
struct RtspTransport {
    // 카메라가 RTP를 보내는 포트
    std::uint16_t server_rtp_port = 0;


    // 카메라가 RTCP를 보내는 포트
    std::uint16_t server_rtcp_port = 0;
};


// DESCRIBE 응답 SDP에서 고른 H.264 비디오 트랙.
struct RtspVideoTrack {
    // rtpmap의 payload type(0~127)
    std::uint8_t payload_type = 0;


    // a=control 값. 없으면 "*"(기준 URL 자체)로 본다.
    std::string control;


    // a=fmtp의 sprop-parameter-sets 값(Base64 SPS·PPS를 ','로 구분). 없으면 빈 문자열
    std::string sprop_parameter_sets;
};


// RTSP/1.0 요청 문자열을 만든다.
//
// method: 메서드. 예: "DESCRIBE"
// url: 요청 URL
// cseq: CSeq 값
// headers: CSeq와 User-Agent 뒤에 넣을 헤더
// 반환값: 요청 줄, CSeq, User-Agent: vms, headers, 빈 줄로 이루어진 문자열. 줄 끝은 "\r\n"이다.
std::string BuildRtspRequest(std::string_view method, std::string_view url, std::uint32_t cseq,
                             std::span<const RtspHeader> headers = {});


// 수신 버퍼의 앞에서 RTSP 응답 하나를 해석한다.
//
// bytes: 지금까지 받은 바이트. 응답 여러 개가 이어져 있으면 첫 응답만 해석한다.
// 반환값: 해석 결과
//   - 응답: 헤더 끝("\r\n\r\n")과 Content-Length만큼의 본문이 모두 도착함
//   - 빈 optional: 아직 응답이 끝나지 않음. 더 받은 뒤 다시 호출한다.
//   - 오류 메시지: 상태 줄이 "RTSP/1.0 <3자리 코드> <문구>"가 아님, "이름: 값" 형식이 아닌 헤더 줄,
//     잘못된 Content-Length, 헤더 영역이 kMaxRtspHeaderSize나 본문이 kMaxRtspBodySize를 넘음
std::expected<std::optional<RtspResponse>, std::string> ParseRtspResponse(std::string_view bytes);


// Session 헤더 값을 해석한다.
//
// value: 예: "1234ABCD;timeout=60"
// 반환값: 세션 ID와 timeout. ID가 비었거나 timeout이 양의 정수가 아니면 원인 메시지
std::expected<RtspSession, std::string> ParseSessionHeader(std::string_view value);


// SETUP 응답의 Transport 헤더 값을 확인하고 서버 포트를 꺼낸다.
//
// value: 예: "RTP/AVP;unicast;client_port=50000-50001;server_port=6970-6971"
// 반환값: 서버 포트 쌍. 다음 경우 원인 메시지
//   - 전송 규격이 RTP/AVP 또는 RTP/AVP/UDP가 아님(TCP interleaved 등)
//   - unicast가 없음
//   - server_port가 없거나 "<포트>-<포트>" 형식이 아님
std::expected<RtspTransport, std::string> ParseTransportHeader(std::string_view value);


// SDP의 control 속성을 SETUP할 절대 URL로 바꾼다.
//
// base: 기준 URL. ContentBaseOf의 결과
// control: a=control 값
// 반환값: control이 "*"이면 base, "rtsp://"로 시작하면(대소문자 무시) control, 아니면 base 끝에 '/'를
//   보장한 뒤 control을 이어 붙인 URL
std::string ResolveControlUrl(std::string_view base, std::string_view control);


// DESCRIBE 응답에서 control URL의 기준 URL을 정한다.
//
// response: DESCRIBE 응답
// request_url: DESCRIBE 요청 URL
// 반환값: Content-Base, 없으면 Content-Location, 둘 다 없으면 request_url. response가 살아 있는 동안 유효하다.
std::string_view ContentBaseOf(const RtspResponse& response, std::string_view request_url);


// DESCRIBE 응답 SDP에서 H.264 비디오 트랙을 고른다.
//
// description: webrtc::ParseSdp로 나눈 SDP
// 반환값: rtpmap이 H264/90000(대소문자 무시)인 첫 video 섹션의 첫 포맷. 없으면 원인 메시지
std::expected<RtspVideoTrack, std::string> SelectH264VideoTrack(const webrtc::SdpSessionDescription& description);


// SDP 세션 수준의 control 속성을 돌려준다.
//
// description: webrtc::ParseSdp로 나눈 SDP
// 반환값: 첫 m= 줄 앞의 a=control 값. 없으면 "*"(기준 URL 자체)
//
// 세션 전체를 대상으로 하는 요청(PLAY, TEARDOWN)의 URL을 정하는 데 쓴다. gst-rtsp-server처럼 세션 수준 control이
// 있는 서버는 트랙 URL로 보낸 PLAY를 거부한다(RFC 2326 C.1.1).
std::string SessionControlOf(const webrtc::SdpSessionDescription& description);


// sprop-parameter-sets 값을 NAL 단위 목록으로 되돌린다.
//
// value: Base64로 부호화한 NAL을 ','로 구분한 문자열(RFC 6184 8.1). 예: "Z0LAHtoCgPbA...,aM48gA=="
// 반환값: 나타난 순서대로의 NAL. Base64 형식이 틀리거나 빈 항목은 건너뛴다.
//
// 연결 성공 로그에 해상도를 남기려고 SPS를 꺼내는 데 쓴다.
std::vector<std::vector<std::uint8_t>> DecodeSpropParameterSets(std::string_view value);


}  // namespace vms::rtsp
