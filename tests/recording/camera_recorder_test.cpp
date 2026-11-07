#include "recording/camera_recorder.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "media/h264_file.h"
#include "media/live_video_source.h"
#include "recording/recording_task.h"
#include "test_support.h"

using vms::media::H264Frame;
using vms::media::LiveVideoEvent;
using vms::media::LiveVideoFrame;
using vms::media::LiveVideoSource;
using vms::recording::CameraRecorder;
using vms::recording::OpenFileTask;
using vms::recording::RecordingTask;
using vms::recording::RecordingTaskSink;
using vms::recording::WriteFragmentTask;
using vms::test::Expect;


namespace {


// SPS NAL
const std::vector<std::uint8_t> kSps = {0x67, 0x42, 0xC0, 0x1F, 0xDA};


// PPS NAL
const std::vector<std::uint8_t> kPps = {0x68, 0xCE, 0x3C, 0x80};


// IDR 슬라이스 NAL
const std::vector<std::uint8_t> kIdrSlice = {0x65, 0x88, 0x84};


// 일반 슬라이스 NAL
const std::vector<std::uint8_t> kSlice = {0x41, 0x9A};


// 2026-11-04 10:01:10 UTC의 유닉스 밀리초
constexpr std::int64_t kStartUnixMs = 1793786470LL * 1000;


// 받은 작업을 모으고, 지정한 대로 여유가 있다고 답하는 가짜 작업 대상.
class FakeSink : public RecordingTaskSink {
public:
    // 작업을 기록한다.
    void Push(RecordingTask task) override { tasks.push_back(std::move(task)); }


    // has_room 값을 돌려준다.
    bool HasRoom() const override { return has_room; }


    // 받은 작업의 종류를 순서대로 이어 붙인다(O: 열기, W: 조각, C: 닫기).
    std::string Kinds() const {
        std::string kinds;
        for (const RecordingTask& task : tasks) {
            kinds += std::holds_alternative<OpenFileTask>(task.action)        ? 'O'
                     : std::holds_alternative<WriteFragmentTask>(task.action) ? 'W'
                                                                              : 'C';
        }
        return kinds;
    }


    // 받은 작업
    std::vector<RecordingTask> tasks;


    // HasRoom이 돌려줄 값
    bool has_room = true;
};


// 녹화 상태 하나와 그 입력(소스, 가짜 시계, 가짜 대상)을 묶은 시험 환경.
struct Harness {
    // 시계를 kStartUnixMs로 두고 단위 60초 녹화 상태를 만든다.
    Harness() : recorder("front", source, 60, sink, [this] { return now; }) {}


    // 키프레임을 발행한다.
    void Keyframe(std::uint32_t rtp_timestamp) {
        const H264Frame frame{.nal_units = {kSps, kPps, kIdrSlice}};
        source.PublishFrame(LiveVideoFrame{.frame = frame, .rtp_timestamp = rtp_timestamp, .is_keyframe = true});
    }


    // 일반 프레임을 발행한다.
    void Delta(std::uint32_t rtp_timestamp) {
        const H264Frame frame{.nal_units = {kSlice}};
        source.PublishFrame(LiveVideoFrame{.frame = frame, .rtp_timestamp = rtp_timestamp, .is_keyframe = false});
    }


    // 프레임을 내보내는 소스
    LiveVideoSource source;


    // 작업을 모으는 대상
    FakeSink sink;


    // 가짜 시계의 현재 시각
    std::chrono::system_clock::time_point now{std::chrono::milliseconds(kStartUnixMs)};


    // 검사 대상
    CameraRecorder recorder;
};


// 발행이 끝난 프레임의 NAL을 복사해 받은 시각으로 계획하고, 만든 작업을 대상에 넣는지 검사한다.
void TestFramesBecomeTasks() {
    Harness harness;
    harness.now += std::chrono::milliseconds(250);
    harness.Keyframe(0);
    harness.Delta(3000);
    harness.Keyframe(90000);

    Expect(harness.sink.Kinds() == "OW", "첫 키프레임에서 열고 다음 키프레임에서 조각");
    const auto* open = harness.sink.tasks.empty() ? nullptr : std::get_if<OpenFileTask>(&harness.sink.tasks[0].action);
    Expect(open != nullptr && open->start_unix_ms == kStartUnixMs + 250 && open->sps == kSps,
           "받은 시각과 SPS로 파일 열기");
    const auto* write =
        harness.sink.tasks.size() < 2 ? nullptr : std::get_if<WriteFragmentTask>(&harness.sink.tasks[1].action);
    Expect(write != nullptr && write->frames.size() == 2 &&
               write->frames[1].nal_units == std::vector<std::vector<std::uint8_t>>{kSlice},
           "발행이 끝난 뒤에도 남는 NAL 복사본");
}


// 여유가 없을 때 온 키프레임부터 GOP를 버리고, 다시 받은 키프레임까지 앞 프레임을 늘리는지 검사한다.
void TestNoRoomSkipsGop() {
    Harness harness;
    harness.Keyframe(0);
    harness.Delta(3000);

    harness.sink.has_room = false;
    harness.Keyframe(90000);
    harness.Delta(93000);
    Expect(harness.sink.Kinds() == "O", "여유가 없으면 그 GOP를 버리고 파일 유지");

    harness.sink.has_room = true;
    harness.Delta(96000);
    harness.Keyframe(180000);
    Expect(harness.sink.Kinds() == "OW", "다시 받은 키프레임에서 앞 GOP를 씀");
    const auto* write =
        harness.sink.tasks.size() < 2 ? nullptr : std::get_if<WriteFragmentTask>(&harness.sink.tasks[1].action);
    Expect(write != nullptr && write->frames.size() == 2 && write->frames[1].duration == 177000,
           "넘침 직전 프레임이 다시 받은 키프레임까지 늘어남");
}


// 손실과 끊김을 계획기에 넘기고 연결은 무시하는지 검사한다.
void TestEvents() {
    Harness harness;
    harness.source.PublishEvent(LiveVideoEvent::kConnected);
    harness.Keyframe(0);
    harness.source.PublishEvent(LiveVideoEvent::kLoss);
    harness.Delta(3000);
    harness.source.PublishEvent(LiveVideoEvent::kDisconnected);
    Expect(harness.sink.Kinds() == "OWC", "손실 뒤 프레임은 버리고, 끊기면 쓰고 닫음");
    const auto* write =
        harness.sink.tasks.size() < 2 ? nullptr : std::get_if<WriteFragmentTask>(&harness.sink.tasks[1].action);
    Expect(write != nullptr && write->frames.size() == 1, "손실 뒤 프레임은 조각에 없음");
}


}  // namespace


int main() {
    TestFramesBecomeTasks();
    TestNoRoomSkipsGop();
    TestEvents();
    return vms::test::ExitCode();
}
