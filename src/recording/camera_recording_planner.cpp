#include "recording/camera_recording_planner.h"

#include <algorithm>
#include <format>
#include <print>
#include <span>
#include <utility>

#include "media/h264_file.h"
#include "recording/fmp4.h"
#include "recording/recording_file_name.h"
#include "recording/recording_index.h"


namespace vms::recording {


namespace {


// 끊김으로 마지막 프레임 길이를 알 수 없고 GOP에 프레임이 하나뿐일 때 쓰는 길이(90kHz, 30fps 한 프레임)
constexpr std::uint32_t kFallbackFrameDuration = 3000;


// 프레임에서 지정한 유형의 첫 NAL을 복사한다.
//
// frame: 찾을 프레임
// type: NAL 유형
// 반환값: 찾은 NAL 복사본. 없으면 빈 벡터
std::vector<std::uint8_t> CopyFirstNalOfType(const RecordingFrame& frame, media::H264NalUnitType type) {
    for (const std::vector<std::uint8_t>& nal_unit : frame.nal_units) {
        if (!nal_unit.empty() && media::NalUnitTypeOf(nal_unit) == static_cast<std::uint8_t>(type)) {
            return nal_unit;
        }
    }
    return {};
}


}  // namespace


CameraRecordingPlanner::CameraRecordingPlanner(std::string camera_name, int segment_seconds,
                                               RecordingTaskHandler handle_task)
    : camera_name_(std::move(camera_name)), segment_seconds_(segment_seconds), handle_task_(std::move(handle_task)) {}


void CameraRecordingPlanner::HandleFrame(RecordingFrame& frame) {
    switch (state_) {
        case State::kWaitingForKeyframe:
            // 키프레임 전의 프레임은 혼자 디코딩할 수 없으므로 버린다.
            if (frame.is_keyframe) {
                StartFile(frame);
                Log("녹화 시작", "");
            }
            return;

        case State::kRecording:
            if (frame.is_keyframe) {
                AcceptKeyframe(frame);
            } else {
                AppendFrame(frame);
            }
            return;

        case State::kSkipping:
            // 키프레임이 와야 다시 디코딩할 수 있으므로 그 전의 프레임은 버린다.
            if (frame.is_keyframe) {
                AcceptKeyframe(frame);
                Log("녹화 재개", "");
            }
            return;
    }
}


void CameraRecordingPlanner::HandleEvent(RecordingEvent event) {
    if (state_ == State::kWaitingForKeyframe) {
        return;
    }
    switch (event) {
        case RecordingEvent::kLoss:
        case RecordingEvent::kQueueOverflow:
            // 이미 버리는 중이면 붙잡은 GOP를 그대로 두고 계속 버린다.
            if (state_ == State::kRecording) {
                state_ = State::kSkipping;
                Log("녹화 프레임 버림",
                    event == RecordingEvent::kLoss ? "패킷 손실" : "녹화 작업이 밀림");
            }
            return;

        case RecordingEvent::kDisconnected:
            // 다음 프레임이 없으므로 버리는 중이었더라도 마지막 프레임을 늘리지 않고 직전 간격으로 어림한다.
            FlushGop(EstimateLastFrameDuration());
            StopFile();
            Log("녹화 중단", "카메라 연결 끊김");
            return;
    }
}


void CameraRecordingPlanner::AcceptKeyframe(RecordingFrame& keyframe) {
    FlushGop(keyframe.rtp_timestamp - gop_.back().rtp_timestamp);
    if (SegmentNumberOf(keyframe.receive_unix_ms, segment_seconds_) != file_segment_number_) {
        StopFile();
        StartFile(keyframe);
        return;
    }
    state_ = State::kRecording;
    AppendFrame(keyframe);
}


void CameraRecordingPlanner::StartFile(RecordingFrame& keyframe) {
    // 서버 시계가 뒤로 갔더라도 같은 카메라의 녹화 시간이 겹치지 않게 한다.
    file_start_unix_ms_ = std::max(keyframe.receive_unix_ms, last_fragment_end_unix_ms_);
    file_base_rtp_ = keyframe.rtp_timestamp;
    file_segment_number_ = SegmentNumberOf(keyframe.receive_unix_ms, segment_seconds_);
    handle_task_(RecordingTask{
        .camera_name = camera_name_,
        .action = OpenFileTask{.start_unix_ms = file_start_unix_ms_,
                               .sps = CopyFirstNalOfType(keyframe, media::H264NalUnitType::kSps),
                               .pps = CopyFirstNalOfType(keyframe, media::H264NalUnitType::kPps)},
    });

    state_ = State::kRecording;
    gop_.clear();
    AppendFrame(keyframe);
}


void CameraRecordingPlanner::FlushGop(std::uint32_t last_frame_duration) {
    if (gop_.empty()) {
        return;
    }

    // 재생 시각은 uint32 차이라서 RTP 타임스탬프가 순환해도 늘어난다. 파일 하나는 13시간을 넘지 않는다.
    WriteFragmentTask task;
    task.frames.reserve(gop_.size());
    for (std::size_t index = 0; index < gop_.size(); ++index) {
        const std::uint32_t duration = index + 1 < gop_.size()
                                           ? gop_[index + 1].rtp_timestamp - gop_[index].rtp_timestamp
                                           : last_frame_duration;
        task.frames.push_back(Fmp4Frame{
            .nal_units = std::move(gop_[index].nal_units),
            .decode_time = static_cast<std::uint32_t>(gop_[index].rtp_timestamp - file_base_rtp_),
            .duration = duration,
        });
    }
    const std::uint64_t end_time = task.frames.back().decode_time + task.frames.back().duration;
    gop_.clear();

    handle_task_(RecordingTask{.camera_name = camera_name_, .action = std::move(task)});
    last_fragment_end_unix_ms_ =
        file_start_unix_ms_ + static_cast<std::int64_t>(end_time * 1000 / kRecordingTimescale);
}


std::uint32_t CameraRecordingPlanner::EstimateLastFrameDuration() const {
    if (gop_.size() < 2) {
        return kFallbackFrameDuration;
    }
    return gop_[gop_.size() - 1].rtp_timestamp - gop_[gop_.size() - 2].rtp_timestamp;
}


void CameraRecordingPlanner::StopFile() {
    gop_.clear();
    handle_task_(RecordingTask{.camera_name = camera_name_, .action = CloseFileTask{}});
    state_ = State::kWaitingForKeyframe;
}


void CameraRecordingPlanner::AppendFrame(RecordingFrame& frame) {
    PendingFrame pending{.nal_units = {}, .rtp_timestamp = frame.rtp_timestamp};
    for (std::vector<std::uint8_t>& nal_unit : frame.nal_units) {
        if (IsFragmentNalUnit(nal_unit)) {
            pending.nal_units.push_back(std::move(nal_unit));
        }
    }
    gop_.push_back(std::move(pending));
}


void CameraRecordingPlanner::Log(std::string_view what, std::string_view reason) const {
    if (reason.empty()) {
        std::println(stderr, "{}", std::format("{}: {}", what, camera_name_));
    } else {
        std::println(stderr, "{}", std::format("{}: {} ({})", what, camera_name_, reason));
    }
}


}  // namespace vms::recording
