#include "webrtc/video_stream_sender.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "rtp_test_support.h"
#include "srtp_test_receiver.h"
#include "test_support.h"

using vms::media::H264File;
using vms::test::Expect;
using vms::test::MakeSyntheticH264File;
using vms::test::ParseRtpPacket;
using vms::test::ReassembleNalUnits;
using vms::test::ReceivedRtpPacket;
using vms::test::SrtpTestReceiver;
using vms::test::TransmittedNalUnits;
using vms::webrtc::VideoFrameTransmission;
using vms::webrtc::VideoStreamSender;
using vms::webrtc::VideoStreamSenderConfig;

using Clock = std::chrono::steady_clock;


namespace {


// 송신 측과 수신 측이 함께 쓰는 마스터 키
constexpr std::array<std::uint8_t, 16> kMasterKey = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                                     0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};


// 송신 측과 수신 측이 함께 쓰는 마스터 솔트
constexpr std::array<std::uint8_t, 14> kMasterSalt = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
                                                      0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D};


// 테스트 송신 설정의 SSRC
constexpr std::uint32_t kSsrc = 0x12345678;


// 테스트 송신 설정의 payload type
constexpr std::uint8_t kPayloadType = 102;


// 송신 상태를 만든다.
//
// video_file: 보낼 동영상
// start_time: 송신 시작 시각
// frames_per_second: 재생 속도
// initial_sequence_number: 순번 초기값
// 반환값: 만든 송신 상태
std::unique_ptr<VideoStreamSender> CreateSender(const H264File& video_file, Clock::time_point start_time,
                                                int frames_per_second = 30,
                                                std::uint16_t initial_sequence_number = 1000) {
    const VideoStreamSenderConfig config{.frames_per_second = frames_per_second,
                                         .ssrc = kSsrc,
                                         .payload_type = kPayloadType,
                                         .initial_sequence_number = initial_sequence_number,
                                         .initial_timestamp = 0xFFFFF000};
    return *VideoStreamSender::Create(video_file, config, kMasterKey, kMasterSalt, start_time);
}


// 프레임 하나의 패킷을 복호화해 헤더와 페이로드를 읽는다.
//
// transmission: SendDueFrame의 결과
// receiver: 순번 순환을 따라가는 테스트 수신 측
// 반환값: 순서대로의 RTP 패킷. 복호화나 해석이 하나라도 실패하면 빈 optional
std::optional<std::vector<ReceivedRtpPacket>> ReceiveFrame(const VideoFrameTransmission& transmission,
                                                           SrtpTestReceiver& receiver) {
    std::vector<ReceivedRtpPacket> packets;
    for (const auto& srtp_packet : transmission.packets) {
        const auto rtp_packet = receiver.Unprotect(srtp_packet);
        if (!rtp_packet) {
            return std::nullopt;
        }
        auto parsed = ParseRtpPacket(*rtp_packet);
        if (!parsed) {
            return std::nullopt;
        }
        packets.push_back(*std::move(parsed));
    }
    return packets;
}


// n번째 프레임의 예정 시각까지의 간격을 계산한다.
//
// frame_number: 프레임 번호
// frames_per_second: 재생 속도
// 반환값: n × 10^9 / fps 나노초
std::chrono::nanoseconds ExpectedOffset(std::uint64_t frame_number, int frames_per_second) {
    return std::chrono::nanoseconds(frame_number * 1'000'000'000 / frames_per_second);
}


// 30fps에서 예정 시각이 시작 + n × 33.33ms이고 이웃 프레임 타임스탬프 차이가 3000인지 검사한다.
void TestScheduleAt30Fps() {
    const H264File video = MakeSyntheticH264File();
    const Clock::time_point start = Clock::now();
    auto sender = CreateSender(video, start);
    auto receiver = *SrtpTestReceiver::Create(kMasterKey, kMasterSalt);

    Clock::time_point now = start;
    std::optional<std::uint32_t> previous_timestamp;
    for (std::uint64_t frame_number = 1; frame_number <= 10; ++frame_number) {
        const auto transmission = sender->SendDueFrame(now);
        const auto packets = transmission ? ReceiveFrame(*transmission, receiver) : std::nullopt;
        if (!packets || packets->empty()) {
            Expect(false, "예정 시각에 프레임을 보내야 한다");
            return;
        }
        Expect(transmission->next_due_time - start == ExpectedOffset(frame_number, 30),
               "n번째 프레임의 예정 시각은 시작 + n/30초여야 한다");
        const std::uint32_t timestamp = packets->front().timestamp;
        if (previous_timestamp) {
            Expect(static_cast<std::uint32_t>(timestamp - *previous_timestamp) == 3000,
                   "30fps에서 이웃 프레임 타임스탬프 차이는 3000이어야 한다");
        }
        previous_timestamp = timestamp;
        now = transmission->next_due_time;
    }
}


// fps 7에서 타임스탬프가 ⌊n × 90000 / 7⌋로 계산되어 오차가 쌓이지 않는지 검사한다.
void TestTimestampWithoutAccumulatedError() {
    const H264File video = MakeSyntheticH264File();
    const Clock::time_point start = Clock::now();
    auto sender = CreateSender(video, start, 7);
    auto receiver = *SrtpTestReceiver::Create(kMasterKey, kMasterSalt);

    Clock::time_point now = start;
    std::optional<std::uint32_t> first_timestamp;
    for (std::uint64_t frame_number = 0; frame_number < 50; ++frame_number) {
        const auto transmission = sender->SendDueFrame(now);
        const auto packets = transmission ? ReceiveFrame(*transmission, receiver) : std::nullopt;
        if (!packets || packets->empty()) {
            Expect(false, "fps 7에서도 프레임을 보내야 한다");
            return;
        }
        const std::uint32_t timestamp = packets->front().timestamp;
        if (!first_timestamp) {
            first_timestamp = timestamp;
        }
        Expect(static_cast<std::uint32_t>(timestamp - *first_timestamp) == frame_number * 90000 / 7,
               "타임스탬프는 ⌊n × 90000 / 7⌋만큼 늘어야 한다");
        now = transmission->next_due_time;
    }
}


// 프레임 3개인 파일에서 5프레임을 보내면 소스 위치가 0,1,2,0,1이고, 순번·타임스탬프가 끊기지 않으며,
// 프레임마다 마지막 패킷에만 marker가 있고 payload type과 SSRC가 설정 값과 같은지 검사한다.
void TestLoopingAndHeaders() {
    const H264File video = MakeSyntheticH264File();
    const Clock::time_point start = Clock::now();
    auto sender = CreateSender(video, start);
    auto receiver = *SrtpTestReceiver::Create(kMasterKey, kMasterSalt);

    Clock::time_point now = start;
    std::optional<std::uint16_t> previous_sequence_number;
    std::optional<std::uint32_t> previous_timestamp;
    for (const std::size_t source_index : {0, 1, 2, 0, 1}) {
        const auto transmission = sender->SendDueFrame(now);
        const auto packets = transmission ? ReceiveFrame(*transmission, receiver) : std::nullopt;
        if (!packets || packets->empty()) {
            Expect(false, "반복 재생 중에도 프레임을 보내야 한다");
            return;
        }

        std::vector<std::vector<std::uint8_t>> payloads;
        for (std::size_t index = 0; index < packets->size(); ++index) {
            const ReceivedRtpPacket& packet = (*packets)[index];
            Expect(packet.marker == (index + 1 == packets->size()), "프레임의 마지막 패킷에만 marker가 있어야 한다");
            Expect(packet.payload_type == kPayloadType && packet.ssrc == kSsrc, "payload type과 SSRC는 설정 값");
            Expect(packet.timestamp == packets->front().timestamp, "한 프레임의 패킷은 같은 타임스탬프");
            if (previous_sequence_number) {
                Expect(packet.sequence_number == static_cast<std::uint16_t>(*previous_sequence_number + 1),
                       "순번은 빠짐없이 1씩 늘어야 한다");
            }
            previous_sequence_number = packet.sequence_number;
            payloads.push_back(packet.payload);
        }
        if (previous_timestamp) {
            Expect(static_cast<std::uint32_t>(packets->front().timestamp - *previous_timestamp) == 3000,
                   "반복할 때도 타임스탬프가 이어져야 한다");
        }
        previous_timestamp = packets->front().timestamp;

        const auto nal_units = ReassembleNalUnits(payloads);
        Expect(nal_units && *nal_units == TransmittedNalUnits(video.Frames()[source_index]),
               "재조립한 프레임은 n mod 3 위치의 소스 프레임(AUD 제외)과 같아야 한다");
        now = transmission->next_due_time;
    }
}


// 순번 초기값이 65534이면 첫 프레임 도중 순번이 0으로 넘어가도 수신 측 복호화가 통과하는지 검사한다.
void TestSequenceNumberWrap() {
    const H264File video = MakeSyntheticH264File();
    const Clock::time_point start = Clock::now();
    auto sender = CreateSender(video, start, 30, 65534);
    auto receiver = *SrtpTestReceiver::Create(kMasterKey, kMasterSalt);

    Clock::time_point now = start;
    std::vector<std::uint16_t> sequence_numbers;
    for (int frame = 0; frame < 3; ++frame) {
        const auto transmission = sender->SendDueFrame(now);
        const auto packets = transmission ? ReceiveFrame(*transmission, receiver) : std::nullopt;
        if (!packets) {
            Expect(false, "순번 순환 뒤에도 수신 측 복호화가 통과해야 한다");
            return;
        }
        for (const ReceivedRtpPacket& packet : *packets) {
            sequence_numbers.push_back(packet.sequence_number);
        }
        now = transmission->next_due_time;
    }
    Expect(sequence_numbers.size() > 3 && sequence_numbers[0] == 65534 && sequence_numbers[1] == 65535 &&
               sequence_numbers[2] == 0,
           "순번은 65534, 65535, 0으로 이어져야 한다");
}


// 예정 시각보다 5프레임 늦게 호출하면 프레임 하나만 보내고 다음 예정 시각이 현재 + 한 프레임 간격인지,
// 예정 시각 전에는 보내지 않는지 검사한다.
void TestRescheduleWhenLate() {
    const H264File video = MakeSyntheticH264File();
    const Clock::time_point start = Clock::now();
    auto sender = CreateSender(video, start);

    const auto first = sender->SendDueFrame(start);
    if (!first) {
        Expect(false, "첫 프레임을 보내야 한다");
        return;
    }
    const Clock::time_point late_now = first->next_due_time + ExpectedOffset(5, 30);
    const auto late = sender->SendDueFrame(late_now);
    if (!late) {
        Expect(false, "늦은 호출에도 프레임을 보내야 한다");
        return;
    }
    Expect(!late->packets.empty(), "늦은 호출은 프레임 하나를 보내야 한다");
    Expect(late->next_due_time == late_now + ExpectedOffset(1, 30), "다음 예정 시각은 현재 + 한 프레임 간격");

    const auto early = sender->SendDueFrame(late->next_due_time - std::chrono::milliseconds(1));
    Expect(early && early->packets.empty() && early->next_due_time == late->next_due_time,
           "예정 시각 전에는 패킷 없이 같은 예정 시각을 돌려줘야 한다");
}


}  // namespace


int main() {
    TestScheduleAt30Fps();
    TestTimestampWithoutAccumulatedError();
    TestLoopingAndHeaders();
    TestSequenceNumberWrap();
    TestRescheduleWhenLate();
    return vms::test::ExitCode();
}
