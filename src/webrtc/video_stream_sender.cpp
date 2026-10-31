#include "webrtc/video_stream_sender.h"

#include <array>
#include <utility>

#include "crypto/random.h"
#include "webrtc/h264_packetizer.h"
#include "webrtc/rtp_packet.h"


namespace vms::webrtc {


namespace {


// H.264 RTP 타임스탬프의 클록 주파수(Hz)
constexpr std::uint64_t kVideoClockRate = 90000;


// 1초의 나노초 수
constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000;


// 순번과 타임스탬프 초기값으로 쓸 난수를 만든다.
//
// 반환값: 6바이트 난수(앞 2바이트 순번, 뒤 4바이트 타임스탬프). 난수 생성이 실패하면 오류 메시지
std::expected<std::array<std::uint8_t, 6>, std::string> GenerateInitialRtpValues() {
    std::array<std::uint8_t, 6> bytes{};
    if (auto filled = crypto::FillRandomBytes(bytes); !filled) {
        return std::unexpected(filled.error());
    }
    return bytes;
}


}  // namespace


std::expected<std::unique_ptr<VideoStreamSender>, std::string> VideoStreamSender::Create(
    const media::H264File& video_file, const VideoStreamSenderConfig& config,
    std::span<const std::uint8_t, crypto::kSrtpMasterKeySize> master_key,
    std::span<const std::uint8_t, crypto::kSrtpMasterSaltSize> master_salt,
    std::chrono::steady_clock::time_point start_time) {
    auto srtp_sender = crypto::SrtpSender::Create(master_key, master_salt);
    if (!srtp_sender) {
        return std::unexpected(srtp_sender.error());
    }

    // 초기값을 무작위로 정해 RTP 패킷만 보고 송신 시작 시점을 추측하기 어렵게 한다(RFC 3550).
    const auto random_values = GenerateInitialRtpValues();
    if (!random_values) {
        return std::unexpected(random_values.error());
    }
    const std::array<std::uint8_t, 6>& bytes = *random_values;
    const std::uint16_t initial_sequence_number = config.initial_sequence_number.value_or(
        static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]));
    const std::uint32_t initial_timestamp = config.initial_timestamp.value_or(
        (static_cast<std::uint32_t>(bytes[2]) << 24) | (static_cast<std::uint32_t>(bytes[3]) << 16) |
        (static_cast<std::uint32_t>(bytes[4]) << 8) | bytes[5]);

    return std::unique_ptr<VideoStreamSender>(new VideoStreamSender(video_file, config, *std::move(srtp_sender),
                                                                    initial_sequence_number, initial_timestamp,
                                                                    start_time));
}


VideoStreamSender::VideoStreamSender(const media::H264File& video_file, const VideoStreamSenderConfig& config,
                                     std::unique_ptr<crypto::SrtpSender> srtp_sender,
                                     std::uint16_t initial_sequence_number, std::uint32_t initial_timestamp,
                                     std::chrono::steady_clock::time_point start_time)
    : video_file_(video_file),
      frames_per_second_(config.frames_per_second),
      ssrc_(config.ssrc),
      payload_type_(config.payload_type),
      srtp_sender_(std::move(srtp_sender)),
      next_sequence_number_(initial_sequence_number),
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
    const std::vector<std::vector<std::uint8_t>> payloads = PacketizeH264Frame(frame);

    VideoFrameTransmission transmission;
    transmission.packets.reserve(payloads.size());
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
        transmission.packets.push_back(*std::move(srtp_packet));
        ++next_sequence_number_;
    }

    ++sent_frame_count_;
    transmission.next_due_time = DueTimeOf(sent_frame_count_);
    return transmission;
}


}  // namespace vms::webrtc
