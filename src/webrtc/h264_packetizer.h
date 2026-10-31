#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "media/h264_file.h"


namespace vms::webrtc {


// RTP 페이로드 최대 크기(바이트).
//
// 페이로드 1200 + RTP 헤더 12 + SRTP 태그 10 + IPv4/UDP 28 = 1250바이트로 일반 MTU 1500보다 충분히 작고,
// DTLS 처리의 MTU 1200과 맞춘다.
inline constexpr std::size_t kMaxRtpPayloadSize = 1200;


// 프레임 하나를 RFC 6184 packetization-mode 1의 RTP 페이로드 목록으로 나눈다.
//
// frame: 패킷화할 프레임
// max_payload_size: 페이로드 최대 크기. FU-A 조각에 본문을 1바이트 이상 담도록 3 이상이어야 한다.
// 반환값: 프레임의 NAL 순서대로의 페이로드. RTP 헤더는 붙이지 않는다.
//
// max_payload_size 이하인 NAL은 그대로 단일 NAL 패킷 페이로드가 된다. 더 큰 NAL은 NAL 헤더를 FU indicator
// (F·NRI + 유형 28)와 FU header(S/E 비트 + 원래 유형)로 옮기고 본문을 나눈 FU-A 조각이 된다.
// AUD는 RFC 6184에서 선택 사항이고 브라우저가 marker 비트와 타임스탬프로 프레임 경계를 알므로 넣지 않는다.
std::vector<std::vector<std::uint8_t>> PacketizeH264Frame(const media::H264Frame& frame,
                                                         std::size_t max_payload_size = kMaxRtpPayloadSize);


}  // namespace vms::webrtc
