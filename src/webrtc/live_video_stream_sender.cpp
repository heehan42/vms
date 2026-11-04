#include "webrtc/live_video_stream_sender.h"

#include <utility>


namespace vms::webrtc {


namespace {


// H.264 RTP 타임스탬프의 클록 주파수(Hz)
constexpr std::uint64_t kVideoClockRate = 90000;


}  // namespace


std::expected<std::unique_ptr<LiveVideoStreamSender>, std::string> LiveVideoStreamSender::Create(
    const LiveVideoStreamSenderConfig& config, std::span<const std::uint8_t, crypto::kSrtpMasterKeySize> master_key,
    std::span<const std::uint8_t, crypto::kSrtpMasterSaltSize> master_salt, bool is_camera_connected) {
    auto packet_writer = SrtpVideoPacketWriter::Create(
        SrtpVideoPacketWriterConfig{.ssrc = config.ssrc,
                                    .payload_type = config.payload_type,
                                    .initial_sequence_number = config.initial_sequence_number},
        master_key, master_salt);
    if (!packet_writer) {
        return std::unexpected(packet_writer.error());
    }

    std::uint32_t initial_timestamp = 0;
    if (config.initial_timestamp) {
        initial_timestamp = *config.initial_timestamp;
    } else {
        const auto random_timestamp = GenerateInitialRtpTimestamp();
        if (!random_timestamp) {
            return std::unexpected(random_timestamp.error());
        }
        initial_timestamp = *random_timestamp;
    }

    return std::unique_ptr<LiveVideoStreamSender>(
        new LiveVideoStreamSender(*std::move(packet_writer), initial_timestamp, is_camera_connected));
}


LiveVideoStreamSender::LiveVideoStreamSender(SrtpVideoPacketWriter packet_writer, std::uint32_t initial_timestamp,
                                             bool is_camera_connected)
    : packet_writer_(std::move(packet_writer)),
      initial_timestamp_(initial_timestamp),
      state_(is_camera_connected ? LiveVideoStreamState::kWaitingForKeyframe
                                 : LiveVideoStreamState::kWaitingForCamera) {}


std::expected<std::vector<std::vector<std::uint8_t>>, std::string> LiveVideoStreamSender::HandleFrame(
    const media::LiveVideoFrame& frame, std::chrono::steady_clock::time_point now) {
    if (state_ == LiveVideoStreamState::kWaitingForCamera) {
        return std::vector<std::vector<std::uint8_t>>{};
    }
    if (state_ == LiveVideoStreamState::kWaitingForKeyframe) {
        // 키프레임 전의 프레임은 앞 프레임 없이 디코딩할 수 없으므로 보내지 않는다.
        if (!frame.is_keyframe) {
            return std::vector<std::vector<std::uint8_t>>{};
        }
        RebaseTimestampIfNeeded(frame.rtp_timestamp, now);
        state_ = LiveVideoStreamState::kSending;
    }

    const auto session_timestamp =
        static_cast<std::uint32_t>(session_base_timestamp_ + (frame.rtp_timestamp - camera_base_timestamp_));
    auto packets = packet_writer_.WriteFrame(frame.frame, session_timestamp);
    if (!packets) {
        return std::unexpected(packets.error());
    }
    last_sent_frame_ = SentFrameRecord{.session_timestamp = session_timestamp, .sent_time = now};
    return packets;
}


void LiveVideoStreamSender::HandleEvent(media::LiveVideoEvent event) {
    switch (event) {
        case media::LiveVideoEvent::kLoss:
            if (state_ == LiveVideoStreamState::kSending) {
                state_ = LiveVideoStreamState::kWaitingForKeyframe;
            }
            break;
        case media::LiveVideoEvent::kDisconnected:
            state_ = LiveVideoStreamState::kWaitingForCamera;
            is_timestamp_base_valid_ = false;
            break;
        case media::LiveVideoEvent::kConnected:
            if (state_ == LiveVideoStreamState::kWaitingForCamera) {
                state_ = LiveVideoStreamState::kWaitingForKeyframe;
            }
            break;
    }
}


void LiveVideoStreamSender::RebaseTimestampIfNeeded(std::uint32_t camera_timestamp,
                                                    std::chrono::steady_clock::time_point now) {
    if (is_timestamp_base_valid_) {
        return;
    }

    if (!last_sent_frame_) {
        session_base_timestamp_ = initial_timestamp_;
    } else {
        // 브라우저가 끊긴 시간을 재생 시간으로 보게 해, 다시 이어진 프레임이 늦게 온 것으로 취급되지 않게 한다.
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_sent_frame_->sent_time).count();
        const std::uint64_t elapsed_ticks =
            elapsed > 0 ? static_cast<std::uint64_t>(elapsed) * kVideoClockRate / 1'000'000'000 : 0;
        session_base_timestamp_ = static_cast<std::uint32_t>(last_sent_frame_->session_timestamp + elapsed_ticks);
    }
    camera_base_timestamp_ = camera_timestamp;
    is_timestamp_base_valid_ = true;
}


}  // namespace vms::webrtc
