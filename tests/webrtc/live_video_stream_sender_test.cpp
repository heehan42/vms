#include "webrtc/live_video_stream_sender.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "rtp_test_support.h"
#include "srtp_test_receiver.h"
#include "test_support.h"

using vms::media::H264File;
using vms::media::LiveVideoEvent;
using vms::media::LiveVideoFrame;
using vms::test::Expect;
using vms::test::MakeSyntheticH264File;
using vms::test::ParseRtpPacket;
using vms::test::ReceivedRtpPacket;
using vms::test::SrtpTestReceiver;
using vms::webrtc::LiveVideoStreamSender;
using vms::webrtc::LiveVideoStreamSenderConfig;
using vms::webrtc::LiveVideoStreamState;

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;


namespace {


// 송신 측과 수신 측이 함께 쓰는 마스터 키
constexpr std::array<std::uint8_t, 16> kMasterKey = {0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
                                                     0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F};


// 송신 측과 수신 측이 함께 쓰는 마스터 솔트
constexpr std::array<std::uint8_t, 14> kMasterSalt = {0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66,
                                                      0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D};


// 처음 보내는 키프레임의 세션 타임스탬프
constexpr std::uint32_t kInitialTimestamp = 0x10000000;


// 송신자와 그 패킷을 받는 테스트 수신 측.
struct Harness {
    // 합성 동영상. 0번은 키프레임, 1·2번은 키프레임이 아니다.
    H264File video = MakeSyntheticH264File();


    // 시험 대상
    std::unique_ptr<LiveVideoStreamSender> sender;


    // 패킷을 복호화하는 수신 측
    SrtpTestReceiver receiver = *SrtpTestReceiver::Create(kMasterKey, kMasterSalt);


    // 테스트 시각의 기준
    Clock::time_point start = Clock::now();


    // 송신자를 만든다.
    //
    // is_camera_connected: 만드는 시점의 카메라 연결 상태
    // initial_sequence_number: 순번 초기값
    explicit Harness(bool is_camera_connected, std::uint16_t initial_sequence_number = 100)
        : sender(*LiveVideoStreamSender::Create(
              LiveVideoStreamSenderConfig{.ssrc = 7,
                                          .payload_type = 102,
                                          .initial_sequence_number = initial_sequence_number,
                                          .initial_timestamp = kInitialTimestamp},
              kMasterKey, kMasterSalt, is_camera_connected)) {}


    // 프레임 하나를 넣고 보낸 패킷을 복호화해 읽는다.
    //
    // keyframe: true이면 키프레임(0번), false이면 1번 프레임을 넣는다.
    // camera_timestamp: 카메라 타임스탬프
    // offset: start부터의 현재 시각
    // 반환값: 보낸 패킷. 보호·복호화가 실패하면 빈 optional
    std::optional<std::vector<ReceivedRtpPacket>> Send(bool keyframe, std::uint32_t camera_timestamp,
                                                       Clock::duration offset = {}) {
        const auto packets = sender->HandleFrame(
            LiveVideoFrame{.frame = video.Frames()[keyframe ? 0 : 1], .rtp_timestamp = camera_timestamp,
                           .is_keyframe = keyframe},
            start + offset);
        if (!packets) {
            return std::nullopt;
        }
        std::vector<ReceivedRtpPacket> received;
        for (const auto& packet : *packets) {
            const auto rtp_packet = receiver.Unprotect(packet);
            auto parsed = rtp_packet ? ParseRtpPacket(*rtp_packet) : std::nullopt;
            if (!parsed) {
                return std::nullopt;
            }
            received.push_back(*std::move(parsed));
        }
        return received;
    }
};


// 키프레임 전의 프레임은 패킷이 없고, 첫 키프레임의 타임스탬프가 초기값이며, 같은 연결 안의 간격이 32비트 순환을
// 포함해 카메라 간격과 같은지 검사한다.
void TestKeyframeStartAndInterval() {
    Harness harness(true);
    const auto before_keyframe = harness.Send(false, 0xFFFFF000);
    Expect(before_keyframe && before_keyframe->empty(), "키프레임 전 프레임은 패킷 없음");
    Expect(harness.sender->State() == LiveVideoStreamState::kWaitingForKeyframe, "키프레임 대기");

    const auto keyframe = harness.Send(true, 0xFFFFF800);
    if (!keyframe || keyframe->empty()) {
        Expect(false, "키프레임은 보냄");
        return;
    }
    Expect(keyframe->front().timestamp == kInitialTimestamp, "첫 키프레임의 타임스탬프는 초기값");
    Expect(harness.sender->State() == LiveVideoStreamState::kSending, "송신 중");

    // 카메라 타임스탬프가 0xFFFFF800에서 0x00000B88(+5000)로 순환한다.
    const auto next = harness.Send(false, 0x00000B88);
    Expect(next && !next->empty() && next->front().timestamp == kInitialTimestamp + 5000,
           "순환해도 카메라 간격을 그대로 유지");
}


// 손실 뒤 다음 키프레임까지 보내지 않고, 다시 보낼 때 순번이 이어지며 타임스탬프 기준이 바뀌지 않는지 검사한다.
void TestLossRecovery() {
    Harness harness(true);
    const auto first = harness.Send(true, 90000);
    if (!first || first->empty()) {
        Expect(false, "첫 키프레임");
        return;
    }
    const std::uint16_t last_sequence_number = first->back().sequence_number;

    harness.sender->HandleEvent(LiveVideoEvent::kLoss);
    Expect(harness.sender->State() == LiveVideoStreamState::kWaitingForKeyframe, "손실 뒤 키프레임 대기");
    const auto skipped = harness.Send(false, 93000);
    Expect(skipped && skipped->empty(), "손실 뒤 키프레임 전 프레임은 보내지 않음");

    const auto resumed = harness.Send(true, 96000);
    Expect(resumed && !resumed->empty() &&
               resumed->front().sequence_number == static_cast<std::uint16_t>(last_sequence_number + 1),
           "순번은 끊김 없이 이어짐");
    Expect(resumed && !resumed->empty() && resumed->front().timestamp == kInitialTimestamp + 6000,
           "손실 뒤에도 카메라 간격 유지");
}


// 끊겼다가 다시 연결되면 타임스탬프가 마지막 송신 뒤 경과 시간만큼 건너뛰는지 검사한다.
void TestReconnectSkipsElapsedTime() {
    Harness harness(true, 65535);
    const auto first = harness.Send(true, 1000, 0ms);
    if (!first || first->empty()) {
        Expect(false, "첫 키프레임");
        return;
    }

    harness.sender->HandleEvent(LiveVideoEvent::kDisconnected);
    Expect(harness.sender->State() == LiveVideoStreamState::kWaitingForCamera, "끊기면 카메라 대기");
    const auto while_disconnected = harness.Send(true, 4000, 100ms);
    Expect(while_disconnected && while_disconnected->empty(), "끊긴 동안 프레임 무시");

    harness.sender->HandleEvent(LiveVideoEvent::kConnected);
    Expect(harness.sender->State() == LiveVideoStreamState::kWaitingForKeyframe, "연결되면 키프레임 대기");
    // 재시작한 카메라의 타임스탬프 원점은 이전과 무관하다.
    const auto after_reconnect = harness.Send(true, 777, 5s);
    Expect(after_reconnect && !after_reconnect->empty() &&
               after_reconnect->front().timestamp == kInitialTimestamp + 5 * 90000,
           "재연결 뒤 타임스탬프는 경과 시간 5초(450000)만큼 건너뜀");
    Expect(after_reconnect && !after_reconnect->empty() &&
               after_reconnect->front().sequence_number == static_cast<std::uint16_t>(first->back().sequence_number + 1),
           "재연결 뒤에도 순번은 이어짐");

    const auto following = harness.Send(false, 777 + 3000, 5s + 33ms);
    Expect(following && !following->empty() && following->front().timestamp == kInitialTimestamp + 5 * 90000 + 3000,
           "새 연결 안에서는 카메라 간격 유지");
}


// 끊긴 상태로 만들면 연결 이벤트 전까지 프레임을 무시하고, 연결 뒤 첫 키프레임은 초기값으로 보내는지 검사한다.
void TestCreatedWhileDisconnected() {
    Harness harness(false);
    Expect(harness.sender->State() == LiveVideoStreamState::kWaitingForCamera, "끊긴 상태로 시작");
    const auto ignored = harness.Send(true, 1000);
    Expect(ignored && ignored->empty(), "연결 이벤트 전에는 키프레임도 무시");

    harness.sender->HandleEvent(LiveVideoEvent::kConnected);
    const auto first = harness.Send(true, 2000, 3s);
    Expect(first && !first->empty() && first->front().timestamp == kInitialTimestamp,
           "한 번도 보내지 않았으면 경과 시간 없이 초기값");
}


}  // namespace


int main() {
    TestKeyframeStartAndInterval();
    TestLossRecovery();
    TestReconnectSkipsElapsedTime();
    TestCreatedWhileDisconnected();
    return vms::test::ExitCode();
}
