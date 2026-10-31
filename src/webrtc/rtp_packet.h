#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>


namespace vms::webrtc {


// 패딩·확장·CSRC가 없는 RTP 고정 헤더의 크기(바이트)
inline constexpr std::size_t kRtpHeaderSize = 12;


// 서버가 보내는 RTP 패킷의 고정 헤더 값. 버전은 항상 2이고 패딩·확장·CSRC는 쓰지 않는다.
struct RtpHeader {
    // 프레임의 마지막 패킷이면 true
    bool marker = false;


    // answer에서 정한 payload type(0~127)
    std::uint8_t payload_type = 0;


    // 패킷마다 1씩 늘어나는 순번
    std::uint16_t sequence_number = 0;


    // 프레임의 RTP 타임스탬프(H.264는 90kHz)
    std::uint32_t timestamp = 0;


    // 송신 스트림 식별자
    std::uint32_t ssrc = 0;
};


// RTP 고정 헤더 뒤에 페이로드를 붙인 패킷을 만든다.
//
// header: 헤더 값. payload_type은 7비트 범위여야 한다.
// payload: 헤더 뒤에 복사할 페이로드
// 반환값: 12바이트 헤더(필드는 네트워크 바이트 순서)와 페이로드를 이어 붙인 패킷
std::vector<std::uint8_t> BuildRtpPacket(const RtpHeader& header, std::span<const std::uint8_t> payload);


}  // namespace vms::webrtc
