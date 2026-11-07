#include "recording/camera_recording_planner.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "recording/recording_task.h"
#include "test_support.h"

using vms::recording::CameraRecordingPlanner;
using vms::recording::CloseFileTask;
using vms::recording::OpenFileTask;
using vms::recording::RecordingEvent;
using vms::recording::RecordingFrame;
using vms::recording::RecordingTask;
using vms::recording::WriteFragmentTask;
using vms::test::Expect;


namespace {


// AUD NAL
const std::vector<std::uint8_t> kAud = {0x09, 0xF0};


// SPS NAL. 계획기는 내용을 해석하지 않으므로 유형만 맞으면 된다.
const std::vector<std::uint8_t> kSps = {0x67, 0x42, 0xC0, 0x1F, 0xDA};


// PPS NAL
const std::vector<std::uint8_t> kPps = {0x68, 0xCE, 0x3C, 0x80};


// IDR 슬라이스 NAL
const std::vector<std::uint8_t> kIdrSlice = {0x65, 0x88, 0x84};


// 일반 슬라이스 NAL
const std::vector<std::uint8_t> kSlice = {0x41, 0x9A};


// 2026-11-04 10:01:10 UTC(단위 60초 구간의 10초 지점)의 유닉스 밀리초
constexpr std::int64_t kStartUnixMs = 1793786470LL * 1000;


// 계획기 하나와 그 입력의 수신 시각, 만든 파일 작업을 묶은 시험 환경.
struct Harness {
    // 수신 시각을 kStartUnixMs로 두고 단위 60초 계획기를 만든다.
    Harness() : planner("front", 60, [this](RecordingTask task) { tasks.push_back(std::move(task)); }) {}


    // 키프레임을 넣는다.
    void Keyframe(std::uint32_t rtp_timestamp) {
        RecordingFrame frame{.nal_units = {kAud, kSps, kPps, kIdrSlice}, .rtp_timestamp = rtp_timestamp,
                             .is_keyframe = true, .receive_unix_ms = now_unix_ms};
        planner.HandleFrame(frame);
    }


    // 일반 프레임을 넣는다.
    void Delta(std::uint32_t rtp_timestamp) {
        RecordingFrame frame{.nal_units = {kAud, kSlice}, .rtp_timestamp = rtp_timestamp, .is_keyframe = false,
                             .receive_unix_ms = now_unix_ms};
        planner.HandleFrame(frame);
    }


    // 수신 시각을 앞으로(음수면 뒤로) 옮긴다.
    void Advance(std::int64_t milliseconds) { now_unix_ms += milliseconds; }


    // 만든 작업의 종류를 순서대로 이어 붙인다(O: 열기, W: 조각, C: 닫기).
    std::string Kinds() const {
        std::string kinds;
        for (const RecordingTask& task : tasks) {
            kinds += std::holds_alternative<OpenFileTask>(task.action)        ? 'O'
                     : std::holds_alternative<WriteFragmentTask>(task.action) ? 'W'
                                                                              : 'C';
        }
        return kinds;
    }


    // index번째 작업의 열기 작업. 다른 종류이면 nullptr
    const OpenFileTask* OpenAt(std::size_t index) const {
        return index < tasks.size() ? std::get_if<OpenFileTask>(&tasks[index].action) : nullptr;
    }


    // index번째 작업의 조각 작업. 다른 종류이면 nullptr
    const WriteFragmentTask* WriteAt(std::size_t index) const {
        return index < tasks.size() ? std::get_if<WriteFragmentTask>(&tasks[index].action) : nullptr;
    }


    // 다음에 넣을 입력의 수신 시각(유닉스 밀리초)
    std::int64_t now_unix_ms = kStartUnixMs;


    // 계획기가 만든 작업
    std::vector<RecordingTask> tasks;


    // 검사 대상
    CameraRecordingPlanner planner;
};


// 첫 키프레임 전 프레임 무시, 첫 키프레임의 열기 작업, 다음 키프레임의 조각 작업을 검사한다.
void TestFirstKeyframeAndFragment() {
    Harness harness;
    harness.Delta(100);
    Expect(harness.tasks.empty(), "첫 키프레임 전 프레임은 무시");

    harness.Keyframe(1000);
    const OpenFileTask* open = harness.OpenAt(0);
    Expect(open != nullptr && open->start_unix_ms == kStartUnixMs, "첫 키프레임의 수신 시각으로 파일 열기");
    Expect(open != nullptr && open->sps == kSps && open->pps == kPps, "열기 작업의 SPS·PPS");
    Expect(harness.tasks.size() == 1 && harness.tasks[0].camera_name == "front", "작업의 카메라 이름");

    harness.Delta(4000);
    harness.Delta(7003);
    harness.Advance(100);
    harness.Keyframe(10000);
    Expect(harness.Kinds() == "OW", "다음 키프레임에서 앞 GOP를 조각으로");
    const WriteFragmentTask* write = harness.WriteAt(1);
    if (write == nullptr || write->frames.size() != 3) {
        Expect(false, "조각의 프레임 수 3");
        return;
    }
    Expect(write->frames[0].decode_time == 0 && write->frames[1].decode_time == 3000 &&
               write->frames[2].decode_time == 6003,
           "재생 시각은 첫 키프레임과의 RTP 차이");
    Expect(write->frames[0].duration == 3000 && write->frames[1].duration == 3003 &&
               write->frames[2].duration == 2997,
           "프레임 길이는 다음 프레임과의 RTP 차이");
    Expect(write->frames[0].nal_units == std::vector<std::vector<std::uint8_t>>{kIdrSlice},
           "키프레임에서 AUD·SPS·PPS 제외");
    Expect(write->frames[1].nal_units == std::vector<std::vector<std::uint8_t>>{kSlice}, "일반 프레임에서 AUD 제외");
}


// 단위 구간이 바뀐 뒤 키프레임에서 파일을 바꾸는지 검사한다.
void TestSegmentSplit() {
    Harness harness;
    harness.Keyframe(0);
    harness.Advance(49'000);  // 10:01:59
    harness.Keyframe(90000 * 49);
    Expect(harness.Kinds() == "OW", "같은 구간에서는 파일 유지");

    harness.Advance(1'000);  // 10:02:00
    harness.Keyframe(90000 * 50);
    Expect(harness.Kinds() == "OWWCO", "새 구간의 키프레임에서 앞 GOP를 쓰고 닫은 뒤 새 파일");
    const OpenFileTask* open = harness.OpenAt(4);
    Expect(open != nullptr && open->start_unix_ms == kStartUnixMs + 50'000, "새 파일 시작 시각");

    harness.Delta(90000 * 50 + 3000);
    harness.Keyframe(90000 * 51);
    const WriteFragmentTask* write = harness.WriteAt(5);
    Expect(write != nullptr && write->frames.size() == 2 && write->frames[0].decode_time == 0 &&
               write->frames[1].decode_time == 3000,
           "새 파일의 재생 시각은 0부터");
}


// 손실이 나면 파일을 닫지 않고 다음 키프레임까지 버리며, 그 키프레임까지 앞 프레임을 늘리는지 검사한다.
void TestLossFreezesLastFrame() {
    Harness harness;
    harness.Keyframe(0);
    harness.Delta(3000);
    harness.planner.HandleEvent(RecordingEvent::kLoss);
    Expect(harness.Kinds() == "O", "손실 시 파일을 닫지 않고 GOP를 붙잡아 둠");

    harness.Delta(6000);
    harness.planner.HandleEvent(RecordingEvent::kLoss);
    harness.Delta(9000);
    Expect(harness.Kinds() == "O", "다음 키프레임 전 프레임은 버림");

    harness.Advance(2'000);
    harness.Keyframe(180000);
    Expect(harness.Kinds() == "OW", "다음 키프레임에서 붙잡은 GOP를 같은 파일에 씀");
    const WriteFragmentTask* write = harness.WriteAt(1);
    Expect(write != nullptr && write->frames.size() == 2 && write->frames[1].decode_time == 3000 &&
               write->frames[1].duration == 177000,
           "손실 직전 프레임이 다음 키프레임까지 늘어남");

    harness.Delta(183000);
    harness.Keyframe(270000);
    const WriteFragmentTask* next = harness.WriteAt(2);
    Expect(next != nullptr && next->frames.size() == 2 && next->frames[0].decode_time == 180000,
           "재개한 GOP의 재생 시각이 이어짐");
}


// 큐 넘침을 손실과 같이 처리하는지 검사한다.
void TestQueueOverflowFreezesLastFrame() {
    Harness harness;
    harness.Keyframe(0);
    harness.Delta(3000);
    harness.planner.HandleEvent(RecordingEvent::kQueueOverflow);
    harness.Keyframe(270000);
    const WriteFragmentTask* write = harness.WriteAt(1);
    Expect(harness.Kinds() == "OW", "큐 넘침 뒤에도 파일 유지");
    Expect(write != nullptr && write->frames.size() == 2 && write->frames[1].duration == 267000,
           "넘침 직전 프레임이 다시 받은 키프레임까지 늘어남");
}


// 끊기면 모으던 GOP를 쓰고 닫으며, 마지막 프레임 길이는 직전 간격인지 검사한다.
void TestDisconnect() {
    Harness harness;
    harness.Keyframe(0);
    harness.Delta(3000);
    harness.Delta(6600);
    harness.planner.HandleEvent(RecordingEvent::kDisconnected);
    Expect(harness.Kinds() == "OWC", "끊기면 모으던 GOP를 쓰고 닫기");
    const WriteFragmentTask* write = harness.WriteAt(1);
    Expect(write != nullptr && write->frames.size() == 3 && write->frames[2].duration == 3600,
           "마지막 프레임 길이는 직전 간격");

    // 프레임이 하나뿐인 GOP는 30fps 한 프레임 길이를 쓴다.
    harness.Keyframe(500);
    harness.planner.HandleEvent(RecordingEvent::kDisconnected);
    const WriteFragmentTask* single = harness.WriteAt(4);
    Expect(harness.Kinds() == "OWCOWC", "다시 연결된 뒤 새 파일");
    Expect(single != nullptr && single->frames.size() == 1 && single->frames[0].duration == 3000,
           "프레임 하나뿐이면 길이 3000");
}


// 버리는 중에 끊기면 붙잡은 GOP를 늘리지 않고 직전 간격으로 쓰고 닫는지 검사한다.
void TestDisconnectWhileSkipping() {
    Harness harness;
    harness.Keyframe(0);
    harness.Delta(3000);
    harness.planner.HandleEvent(RecordingEvent::kLoss);
    harness.Delta(6000);
    harness.planner.HandleEvent(RecordingEvent::kDisconnected);
    Expect(harness.Kinds() == "OWC", "붙잡은 GOP를 쓰고 닫기");
    const WriteFragmentTask* write = harness.WriteAt(1);
    Expect(write != nullptr && write->frames.size() == 2 && write->frames[1].duration == 3000,
           "붙잡은 GOP의 마지막 프레임 길이는 직전 간격");

    harness.Delta(9000);
    Expect(harness.Kinds() == "OWC", "끊긴 뒤에는 키프레임을 기다림");
}


// 수신 시각이 뒤로 가도 새 파일의 시작 시각이 직전 파일 끝 시각보다 이르지 않은지 검사한다.
void TestClockGoesBackward() {
    Harness harness;
    harness.Keyframe(0);
    harness.Delta(3000);
    harness.Advance(1'000);
    harness.Keyframe(90000);  // 직전 파일 끝: 시작 + 1초
    harness.planner.HandleEvent(RecordingEvent::kDisconnected);

    harness.Advance(-10'000);
    harness.Keyframe(0);
    const OpenFileTask* open = harness.OpenAt(4);
    Expect(harness.Kinds() == "OWWCO", "다시 연 파일");
    // 마지막 GOP는 키프레임 하나(길이 3000)이므로 끝 시각은 시작 + 1초 + 1/30초다.
    Expect(open != nullptr && open->start_unix_ms == kStartUnixMs + 1'033, "직전 파일 끝 시각으로 올림");
}


// RTP 타임스탬프가 순환해도 재생 시각과 길이가 늘어나는지 검사한다.
void TestRtpWraparound() {
    Harness harness;
    harness.Keyframe(0xFFFFF000U);
    harness.Delta(0xFFFFF000U + 3000);  // 0x00000BB8 - 0x1000 = 순환
    harness.Keyframe(0xFFFFF000U + 6000);
    const WriteFragmentTask* write = harness.WriteAt(1);
    Expect(write != nullptr && write->frames.size() == 2 && write->frames[1].decode_time == 3000 &&
               write->frames[1].duration == 3000,
           "순환 구간에서도 재생 시각과 길이가 늘어남");
}


}  // namespace


int main() {
    TestFirstKeyframeAndFragment();
    TestSegmentSplit();
    TestLossFreezesLastFrame();
    TestQueueOverflowFreezesLastFrame();
    TestDisconnect();
    TestDisconnectWhileSkipping();
    TestClockGoesBackward();
    TestRtpWraparound();
    return vms::test::ExitCode();
}
