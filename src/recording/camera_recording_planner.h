#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "recording/recording_task.h"


namespace vms::recording {


// 녹화 계획에 넣는 카메라 프레임 하나.
//
// 실시간 소스의 프레임 뷰는 발행하는 동안만 유효하므로 NAL 바이트를 복사해 담는다.
struct RecordingFrame {
    // 프레임의 NAL 복사본(시작 코드 제외). AUD·SPS·PPS도 그대로 담는다.
    std::vector<std::vector<std::uint8_t>> nal_units;


    // 카메라 RTP 타임스탬프(90kHz)
    std::uint32_t rtp_timestamp = 0;


    // SPS, PPS, IDR 슬라이스를 모두 포함하면 true
    bool is_keyframe = false;


    // 이벤트 루프가 프레임을 받은 서버 시각(유닉스 밀리초, UTC). 파일 시작 시각과 단위 구간 분할의 기준이다.
    std::int64_t receive_unix_ms = 0;
};


// 녹화에 영향을 주는 카메라 상태 변화.
enum class RecordingEvent {
    // 패킷이 빠져 만들던 프레임을 버림. 다음 키프레임까지 프레임을 쓸 수 없다.
    kLoss,

    // 기록을 기다리는 데이터가 상한 이상이라 이번 키프레임부터 다음 키프레임까지 프레임을 버림
    kQueueOverflow,

    // 카메라 연결이 끊김
    kDisconnected,
};


// 계획기가 만든 파일 작업을 받는 함수.
using RecordingTaskHandler = std::function<void(RecordingTask)>;


// 카메라 하나의 녹화 입력을 GOP로 모아 파일 작업으로 바꾸는 녹화 계획.
//
// 이벤트 루프 스레드의 CameraRecorder 안에서 프레임과 상태 변화를 받아, 언제 파일을 열고 닫을지와 각 프레임의 재생
// 시각을 정한다. 시계를 읽지 않고 프레임의 수신 시각만 쓰므로, 입력이 같으면 만드는 작업도 같다.
//
// 상태는 키프레임 대기, 녹화 중, 버리는 중 세 가지다. 손실이나 큐 넘침이 오면 모으던 GOP를 붙잡아 둔 채 다음
// 키프레임까지 프레임을 버리고, 그 키프레임에서 붙잡은 GOP의 마지막 프레임을 그 키프레임까지 늘려 쓴다. 그래서 버린
// 구간은 마지막 화면이 멈춘 채로 재생되고 파일은 닫히지 않는다. 끊김일 때만 파일을 닫는다.
//
// 한 스레드에서만 쓴다. 소멸할 때 붙잡거나 모으던 GOP는 쓰지 않고 버린다.
class CameraRecordingPlanner {
public:
    // 키프레임을 기다리는 상태로 만든다.
    //
    // camera_name: 카메라 이름. 만드는 작업의 대상 폴더가 된다.
    // segment_seconds: 파일을 나누는 단위 시간(초). 1 이상이어야 한다.
    // handle_task: 만든 파일 작업을 받는 함수
    CameraRecordingPlanner(std::string camera_name, int segment_seconds, RecordingTaskHandler handle_task);


    // 프레임 하나를 처리한다.
    //
    // frame: 처리할 프레임. NAL 바이트는 GOP로 옮겨 가므로 호출 뒤 비어 있을 수 있다.
    void HandleFrame(RecordingFrame& frame);


    // 상태 변화를 처리한다.
    void HandleEvent(RecordingEvent event);


private:
    // 녹화 상태
    enum class State {
        // 열린 파일이 없고, 다음 키프레임을 기다림
        kWaitingForKeyframe,

        // 파일이 열려 있고 GOP를 모으는 중
        kRecording,

        // 파일이 열려 있고, 모으던 GOP를 붙잡아 둔 채 다음 키프레임까지 프레임을 버리는 중
        kSkipping,
    };


    // GOP로 모으는 중인 프레임 하나.
    struct PendingFrame {
        // 조각에 넣을 NAL. AUD·SPS·PPS는 뺐다.
        std::vector<std::vector<std::uint8_t>> nal_units;


        // 카메라 RTP 타임스탬프(90kHz)
        std::uint32_t rtp_timestamp = 0;
    };


    // 녹화 중이거나 버리는 중에 온 키프레임으로 앞 GOP를 쓰고 새 GOP를 시작한다.
    //
    // 앞 GOP의 마지막 프레임 길이는 이 키프레임까지의 간격이다. 버리는 중이었다면 버린 구간이 이 길이에 들어간다.
    // 키프레임이 새 단위 구간에 속하면 파일을 바꾼다.
    void AcceptKeyframe(RecordingFrame& keyframe);


    // 키프레임으로 새 파일을 열고 녹화 중 상태가 된다.
    void StartFile(RecordingFrame& keyframe);


    // 모은 GOP를 조각 작업으로 넘기고 GOP를 비운다.
    //
    // last_frame_duration: 마지막 프레임의 길이(90kHz)
    //
    // GOP가 비어 있으면 아무것도 하지 않는다. 넘긴 뒤 파일 끝 시각을 갱신한다.
    void FlushGop(std::uint32_t last_frame_duration);


    // 끊김으로 다음 프레임이 없을 때 마지막 프레임의 길이를 어림한다.
    //
    // 반환값: GOP 안 직전 프레임 간격. 프레임이 하나뿐이면 30fps 한 프레임 길이
    std::uint32_t EstimateLastFrameDuration() const;


    // 파일을 닫는 작업을 넘기고 키프레임 대기 상태가 된다. 모으던 GOP는 미리 써 두어야 한다.
    void StopFile();


    // 프레임의 NAL 중 조각에 넣을 것을 옮겨 GOP 끝에 붙인다.
    void AppendFrame(RecordingFrame& frame);


    // "<무엇>: <카메라 이름> (<원인>)" 형식의 로그 한 줄을 남긴다.
    //
    // what: 일어난 일. 예: 녹화 시작
    // reason: 원인. 비어 있으면 괄호를 붙이지 않는다.
    void Log(std::string_view what, std::string_view reason) const;


    // 카메라 이름
    std::string camera_name_;


    // 파일을 나누는 단위 시간(초)
    int segment_seconds_;


    // 만든 파일 작업을 받는 함수
    RecordingTaskHandler handle_task_;


    // 녹화 상태
    State state_ = State::kWaitingForKeyframe;


    // 모으는 중이거나 붙잡아 둔 GOP. 녹화 중이거나 버리는 중일 때 비어 있지 않으며, 첫 프레임은 키프레임이다.
    std::vector<PendingFrame> gop_;


    // 열린 파일의 시작 시각(유닉스 밀리초). 녹화 중이거나 버리는 중일 때만 의미가 있다.
    std::int64_t file_start_unix_ms_ = 0;


    // 열린 파일 첫 키프레임의 RTP 타임스탬프. 프레임 재생 시각의 기준이다.
    std::uint32_t file_base_rtp_ = 0;


    // 열린 파일 첫 키프레임의 수신 시각이 속한 단위 구간 번호
    std::int64_t file_segment_number_ = 0;


    // 지금까지 이 카메라가 쓴 마지막 조각의 끝 시각(유닉스 밀리초). 새 파일 시작 시각의 하한이다. 처음에는 최솟값이다.
    std::int64_t last_fragment_end_unix_ms_ = std::numeric_limits<std::int64_t>::min();
};


}  // namespace vms::recording
