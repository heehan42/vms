#include "webrtc/srtp_video_packet_writer.h"

#include <array>
#include <utility>

#include "crypto/random.h"
#include "webrtc/h264_packetizer.h"
#include "webrtc/rtp_packet.h"


namespace vms::webrtc {


std::expected<SrtpVideoPacketWriter, std::string> SrtpVideoPacketWriter::Create(
    const SrtpVideoPacketWriterConfig& config, std::span<const std::uint8_t, crypto::kSrtpMasterKeySize> master_key,
    std::span<const std::uint8_t, crypto::kSrtpMasterSaltSize> master_salt) {
    auto srtp_sender = crypto::SrtpSender::Create(master_key, master_salt);
    if (!srtp_sender) {
        return std::unexpected(srtp_sender.error());
    }

    std::uint16_t initial_sequence_number = 0;
    if (config.initial_sequence_number) {
        initial_sequence_number = *config.initial_sequence_number;
    } else {
        std::array<std::uint8_t, 2> bytes{};
        if (auto filled = crypto::FillRandomBytes(bytes); !filled) {
            return std::unexpected(filled.error());
        }
        initial_sequence_number = static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]);
    }
    return SrtpVideoPacketWriter(config, *std::move(srtp_sender), initial_sequence_number);
}


SrtpVideoPacketWriter::SrtpVideoPacketWriter(const SrtpVideoPacketWriterConfig& config,
                                             std::unique_ptr<crypto::SrtpSender> srtp_sender,
                                             std::uint16_t initial_sequence_number)
    : ssrc_(config.ssrc),
      payload_type_(config.payload_type),
      srtp_sender_(std::move(srtp_sender)),
      next_sequence_number_(initial_sequence_number) {}


std::expected<std::vector<std::vector<std::uint8_t>>, std::string> SrtpVideoPacketWriter::WriteFrame(
    const media::H264Frame& frame, std::uint32_t timestamp) {
    const std::vector<std::vector<std::uint8_t>> payloads = PacketizeH264Frame(frame);

    std::vector<std::vector<std::uint8_t>> packets;
    packets.reserve(payloads.size());
    for (std::size_t index = 0; index < payloads.size(); ++index) {
        const std::vector<std::uint8_t> rtp_packet = BuildRtpPacket(RtpHeader{.marker = index + 1 == payloads.size(),
                                                                              .payload_type = payload_type_,
                                                                              .sequence_number = next_sequence_number_,
                                                                              .timestamp = timestamp,
                                                                              .ssrc = ssrc_},
                                                                    payloads[index]);
        auto srtp_packet = srtp_sender_->Protect(rtp_packet);
        if (!srtp_packet) {
            return std::unexpected(srtp_packet.error());
        }
        packets.push_back(*std::move(srtp_packet));
        ++next_sequence_number_;
    }
    return packets;
}


std::expected<std::uint32_t, std::string> GenerateInitialRtpTimestamp() {
    std::array<std::uint8_t, 4> bytes{};
    if (auto filled = crypto::FillRandomBytes(bytes); !filled) {
        return std::unexpected(filled.error());
    }
    return (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
}


}  // namespace vms::webrtc
