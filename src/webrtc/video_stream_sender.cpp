#include "webrtc/video_stream_sender.h"

#include <utility>


namespace vms::webrtc {


namespace {


// H.264 RTP 타임스탬프의 클록 주파수(Hz)
constexpr std::uint64_t kVideoClockRate = 90000;


// 1초의 나노초 수
constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000;


}  // namespace


std::expected<std::unique_ptr<VideoStreamSender>, std::string> VideoStreamSender::Create(
    const media::H264File& video_file, const VideoStreamSenderConfig& config,
    std::span<const std::uint8_t, crypto::kSrtpMasterKeySize> master_key,
    std::span<const std::uint8_t, crypto::kSrtpMasterSaltSize> master_salt,
    std::chrono::steady_clock::time_point start_time) {
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

    return std::unique_ptr<VideoStreamSender>(new VideoStreamSender(
        video_file, config.frames_per_second, *std::move(packet_writer), initial_timestamp, start_time));
}


VideoStreamSender::VideoStreamSender(const media::H264File& video_file, int frames_per_second,
                                     SrtpVideoPacketWriter packet_writer, std::uint32_t initial_timestamp,
                                     std::chrono::steady_clock::time_point start_time)
    : video_file_(video_file),
      frames_per_second_(frames_per_second),
      packet_writer_(std::move(packet_writer)),
      initial_timestamp_(initial_timestamp),
      schedule_origin_(start_time) {}


std::chrono::steady_clock::time_point VideoStreamSender::DueTimeOf(std::uint64_t frame_number) const {
    // 누적 덧셈 대신 기준점에서 바로 곱해 계산하므로, 1/fps가 나노초로 나누어떨어지지 않아도 오차가 쌓이지 않는다.
    const std::uint64_t frames_since_origin = frame_number - schedule_origin_frame_;
    const std::uint64_t nanoseconds = frames_since_origin * kNanosecondsPerSecond / frames_per_second_;
    return schedule_origin_ +
           std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(nanoseconds));
}


std::expected<VideoFrameTransmission, std::string> VideoStreamSender::SendDueFrame(
    std::chrono::steady_clock::time_point now) {
    const std::chrono::steady_clock::time_point due_time = DueTimeOf(sent_frame_count_);
    if (now < due_time) {
        return VideoFrameTransmission{.packets = {}, .next_due_time = due_time};
    }

    // 루프가 한 프레임 간격 넘게 멈췄다 돌아왔으면 밀린 프레임을 몰아 보내지 않고, 지금 이 프레임을 보낸 뒤
    // 지금부터 다시 일정대로 보낸다. 몰아 보내면 브라우저 지터 버퍼가 넘치거나 송신 버퍼가 가득 찬다.
    if (now > DueTimeOf(sent_frame_count_ + 1)) {
        schedule_origin_ = now;
        schedule_origin_frame_ = sent_frame_count_;
    }

    const std::vector<media::H264Frame>& frames = video_file_.Frames();
    const media::H264Frame& frame = frames[sent_frame_count_ % frames.size()];
    const auto timestamp =
        static_cast<std::uint32_t>(initial_timestamp_ + sent_frame_count_ * kVideoClockRate / frames_per_second_);
    auto packets = packet_writer_.WriteFrame(frame, timestamp);
    if (!packets) {
        return std::unexpected(packets.error());
    }

    ++sent_frame_count_;
    return VideoFrameTransmission{.packets = *std::move(packets), .next_due_time = DueTimeOf(sent_frame_count_)};
}


}  // namespace vms::webrtc
