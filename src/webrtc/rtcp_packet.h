#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>


namespace vms::webrtc {


// SR(Sender Report)의 RTCP 패킷 종류 번호
inline constexpr std::uint8_t kRtcpSenderReportType = 200;


// RR(Receiver Report)의 RTCP 패킷 종류 번호
inline constexpr std::uint8_t kRtcpReceiverReportType = 201;


// SDES(Source Description)의 RTCP 패킷 종류 번호
inline constexpr std::uint8_t kRtcpSourceDescriptionType = 202;


// SR·RR에 담긴, 송신자 하나에 대한 수신 품질 보고(RFC 3550 6.4.1).
struct RtcpReportBlock {
    // 이 보고가 가리키는 송신 스트림의 SSRC. 서버가 보낸 영상이면 answer의 a=ssrc 값이다.
    std::uint32_t source_ssrc = 0;


    // 직전 보고 이후 잃은 패킷의 비율(256분율)
    std::uint8_t fraction_lost = 0;


    // 지금까지 잃은 패킷 수. 중복 수신 때문에 음수가 될 수 있는 부호 있는 24비트 값이다.
    std::int32_t cumulative_lost = 0;


    // 받은 가장 큰 순번. 상위 16비트는 순번이 넘어간 횟수, 하위 16비트는 RTP 순번이다.
    std::uint32_t extended_highest_sequence_number = 0;


    // 도착 간격 변동(RTP 타임스탬프 단위)
    std::uint32_t jitter = 0;


    // 마지막으로 받은 SR의 NTP 타임스탬프 가운데 32비트(LSR). 받은 SR이 없으면 0
    std::uint32_t last_sender_report = 0;


    // 마지막 SR을 받은 뒤 이 보고를 보낼 때까지 지난 시간(DLSR, 1/65536초 단위)
    std::uint32_t delay_since_last_sender_report = 0;
};


// RTCP 복합 패킷 안의 패킷 하나를 해석한 결과.
//
// SR·RR은 보고 블록, SDES는 첫 청크의 CNAME까지 읽고, 그 밖의 종류는 종류 번호와 길이만 담는다.
struct RtcpPacket {
    // 패킷 종류 번호(PT). 200 SR, 201 RR, 202 SDES, 205·206 피드백 등이다.
    std::uint8_t packet_type = 0;


    // 헤더의 5비트 count 필드. SR·RR은 보고 블록 수, SDES는 청크 수, 피드백은 세부 종류(FMT)다.
    std::uint8_t count = 0;


    // 헤더와 padding을 포함한 패킷 전체 길이(바이트)
    std::size_t size = 0;


    // 헤더 바로 뒤의 SSRC. SR·RR·피드백은 보낸 쪽 SSRC, SDES는 첫 청크의 SSRC다. 본문이 4바이트보다
    // 짧으면 비어 있다.
    std::optional<std::uint32_t> sender_ssrc;


    // SR·RR의 보고 블록. 그 밖의 종류에서는 비어 있다.
    std::vector<RtcpReportBlock> report_blocks;


    // SDES 첫 청크의 CNAME 원본 바이트. 상대가 정한 값이므로 로그에 쓰기 전에 걸러야 한다.
    // CNAME 항목이 없거나 SDES가 아니면 비어 있다.
    std::optional<std::string> cname;
};


// RTCP 복합 패킷을 개별 패킷으로 나누어 해석한다(RFC 3550 6.1).
//
// compound: SRTCP 검증·복호화를 마친 RTCP 복합 패킷
// 반환값: 앞에서부터 순서대로의 패킷. 형식이 틀리면 원인 메시지
//
// 패킷마다 버전이 2이고 length가 남은 데이터를 넘지 않아야 하며, 마지막 패킷이 데이터 끝에서 정확히
// 끝나야 한다. padding이 있으면 마지막 바이트를 padding 길이로 보고 본문에서 뺀다. 하나라도 틀리면
// 복합 패킷 전체를 형식 오류로 본다. reduced-size RTCP를 허용하기 위해 첫 패킷이 SR·RR인지는 보지 않는다.
std::expected<std::vector<RtcpPacket>, std::string> ParseRtcpCompoundPacket(std::span<const std::uint8_t> compound);


}  // namespace vms::webrtc
