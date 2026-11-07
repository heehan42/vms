#include "recording/camera_recorder.h"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>


namespace vms::recording {


CameraRecorder::CameraRecorder(std::string camera_name, media::LiveVideoSource& source, int segment_seconds,
                               RecordingTaskSink& sink, RecordingClock clock)
    : source_(source),
      sink_(sink),
      clock_(std::move(clock)),
      planner_(std::move(camera_name), segment_seconds, [this](RecordingTask task) { sink_.Push(std::move(task)); }) {
    subscription_id_ = source_.Subscribe([this](const media::LiveVideoFrame& frame) { HandleFrame(frame); },
                                         [this](media::LiveVideoEvent event) { HandleEvent(event); });
}


CameraRecorder::~CameraRecorder() { source_.Unsubscribe(subscription_id_); }


void CameraRecorder::HandleFrame(const media::LiveVideoFrame& frame) {
    // 버림 판단은 키프레임에서만 해서 GOP 중간이 잘리지 않게 한다. 넘친 뒤 오는 일반 프레임은 계획기가 버린다.
    if (frame.is_keyframe && !sink_.HasRoom()) {
        planner_.HandleEvent(RecordingEvent::kQueueOverflow);
        return;
    }

    RecordingFrame copy{
        .nal_units = {},
        .rtp_timestamp = frame.rtp_timestamp,
        .is_keyframe = frame.is_keyframe,
        .receive_unix_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock_().time_since_epoch()).count(),
    };
    copy.nal_units.reserve(frame.frame.nal_units.size());
    for (const std::span<const std::uint8_t> nal_unit : frame.frame.nal_units) {
        copy.nal_units.emplace_back(nal_unit.begin(), nal_unit.end());
    }
    planner_.HandleFrame(copy);
}


void CameraRecorder::HandleEvent(media::LiveVideoEvent event) {
    switch (event) {
        case media::LiveVideoEvent::kConnected:
            return;

        case media::LiveVideoEvent::kLoss:
            planner_.HandleEvent(RecordingEvent::kLoss);
            return;

        case media::LiveVideoEvent::kDisconnected:
            planner_.HandleEvent(RecordingEvent::kDisconnected);
            return;
    }
}


}  // namespace vms::recording
