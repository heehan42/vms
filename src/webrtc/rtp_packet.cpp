#include "webrtc/rtp_packet.h"


namespace vms::webrtc {


std::vector<std::uint8_t> BuildRtpPacket(const RtpHeader& header, std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> packet;
    packet.reserve(kRtpHeaderSize + payload.size());

    // 첫 바이트는 버전 2(상위 2비트)이고 패딩·확장·CSRC 수는 0이다.
    packet.push_back(0x80);
    packet.push_back(static_cast<std::uint8_t>((header.marker ? 0x80 : 0x00) | (header.payload_type & 0x7F)));
    packet.push_back(static_cast<std::uint8_t>(header.sequence_number >> 8));
    packet.push_back(static_cast<std::uint8_t>(header.sequence_number));
    for (const std::uint32_t value : {header.timestamp, header.ssrc}) {
        packet.push_back(static_cast<std::uint8_t>(value >> 24));
        packet.push_back(static_cast<std::uint8_t>(value >> 16));
        packet.push_back(static_cast<std::uint8_t>(value >> 8));
        packet.push_back(static_cast<std::uint8_t>(value));
    }

    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}


}  // namespace vms::webrtc
