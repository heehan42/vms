#pragma once

#include <chrono>
#include <functional>
#include <string>

#include "media/live_video_source.h"
#include "recording/camera_recording_planner.h"
#include "recording/recording_task.h"


namespace vms::recording {


// 현재 시각(UTC)을 돌려주는 함수. 테스트에서 시계를 흉내 내는 데 쓴다.
using RecordingClock = std::function<std::chrono::system_clock::time_point()>;


// 카메라 하나의 프레임을 녹화 계획에 넣고, 계획이 만든 파일 작업을 녹화 스레드의 큐로 넘기는 녹화 상태.
//
// 이벤트 루프 스레드에서 실시간 소스를 구독한다. 프레임마다 NAL을 복사해 받은 시각과 함께 계획기
// (CameraRecordingPlanner)에 넣고, 계획기가 정한 파일 열기·조각·닫기 작업을 작업 대상에 넣는다. 바이트를 만들고
// 디스크에 쓰는 일은 작업을 받은 녹화 스레드가 한다.
//
// 키프레임이 왔을 때 작업 대상에 여유가 없으면 그 GOP를 계획기에 넣지 않고 큐 넘침으로 알린다. 그래서 버리는 단위가
// GOP로 맞고, 계획기는 앞 GOP의 마지막 프레임을 다시 받은 키프레임까지 늘린다.
//
// 소스와 작업 대상을 참조하므로 둘보다 먼저 소멸해야 한다. 소멸할 때 구독을 해지하며, 쓰던 파일을 닫는 작업은 넣지
// 않는다(정상 종료에서는 카메라 클라이언트의 Stop()이 먼저 끊김을 알려 파일이 닫힌다). 구독 핸들러가 this를 잡으므로
// 복사·이동하지 않는다.
class CameraRecorder {
public:
    // 녹화 상태를 만들고 소스를 구독한다.
    //
    // camera_name: 카메라 이름. 작업의 대상 폴더가 된다.
    // source: 구독할 실시간 소스
    // segment_seconds: 파일을 나누는 단위 시간(초). 1 이상이어야 한다.
    // sink: 작업을 넣을 대상
    // clock: 현재 시각 함수. 프레임을 받을 때마다 부른다.
    CameraRecorder(std::string camera_name, media::LiveVideoSource& source, int segment_seconds,
                   RecordingTaskSink& sink, RecordingClock clock);


    // 구독을 해지한다.
    ~CameraRecorder();


    // 구독 핸들러가 this를 잡으므로 복사를 금지한다.
    CameraRecorder(const CameraRecorder&) = delete;


    // 구독 핸들러가 this를 잡으므로 복사 대입을 금지한다.
    CameraRecorder& operator=(const CameraRecorder&) = delete;


private:
    // 프레임을 복사해 계획기에 넣는다. 여유가 없을 때 온 키프레임은 넣지 않고 큐 넘침으로 알린다.
    void HandleFrame(const media::LiveVideoFrame& frame);


    // 녹화에 영향을 주는 상태 변화(손실, 끊김)를 계획기에 넣는다. 연결은 다음 키프레임이 녹화를 시작하므로 넣지 않는다.
    void HandleEvent(media::LiveVideoEvent event);


    // 구독한 소스. 이 객체보다 오래 살아 있다.
    media::LiveVideoSource& source_;


    // 소스 구독 ID
    media::LiveVideoSource::SubscriptionId subscription_id_ = 0;


    // 작업을 넣을 대상. 이 객체보다 오래 살아 있다.
    RecordingTaskSink& sink_;


    // 현재 시각 함수
    RecordingClock clock_;


    // 녹화 계획. 만든 작업을 sink_에 넣는다.
    CameraRecordingPlanner planner_;
};


}  // namespace vms::recording
