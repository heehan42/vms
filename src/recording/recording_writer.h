#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>

#include "recording/recorded_file.h"
#include "recording/recording_catalog_event.h"
#include "recording/recording_file_store.h"
#include "recording/recording_options.h"
#include "recording/recording_task.h"


namespace vms::recording {


// 파일 작업 큐와 그 작업을 실행하는 녹화 스레드.
//
// 이벤트 루프 스레드는 Push로 작업을 넣고 HasRoom으로 여유를 확인하기만 한다. 파일 열기·쓰기·동기화·삭제와 여유 공간
// 조회는 모두 이 객체가 만든 녹화 스레드가 파일 저장소(RecordingFileStore)로 한다. 두 스레드가 공유하는 것은 잠금으로
// 보호한 작업 큐와 그 바이트 합뿐이다.
//
// 소멸자는 정지를 요청한 뒤 큐에 남은 작업을 모두 처리하고, 열린 파일을 동기화해 닫은 다음 스레드를 join한다.
// 작업을 넣는 쪽(CameraRecorder)은 이 객체보다 먼저 소멸해야 한다.
class RecordingWriter final : public RecordingTaskSink {
public:
    // 녹화 스레드를 시작한다.
    //
    // options: 녹화 설정. 범위 검사를 마친 값이어야 한다.
    // closed_files: 시작할 때 이미 있는 닫힌 파일 목록(복구 결과). 등록되지 않은 카메라 폴더도 포함한다. 조각 목록은
    //               쓰지 않으므로 비운 사본을 넘겨도 된다.
    // listener: 녹화 스레드가 파일을 닫거나 지울 때 부르는 함수. 녹화 스레드에서 불리며, 소멸자에서 마지막 파일을 닫을
    //           때도 불린다. 그래서 이 함수가 참조하는 객체는 이 객체보다 오래 살아 있어야 한다. 비어 있으면 알리지 않는다.
    RecordingWriter(RecordingOptions options, RecordedFileCatalog closed_files, RecordingCatalogListener listener);


    // 남은 작업을 모두 처리하고 열린 파일을 닫은 뒤 녹화 스레드를 끝낸다.
    ~RecordingWriter() override;


    // 스레드가 this를 참조하므로 복사를 금지한다.
    RecordingWriter(const RecordingWriter&) = delete;


    // 스레드가 this를 참조하므로 복사 대입을 금지한다.
    RecordingWriter& operator=(const RecordingWriter&) = delete;


    // 작업을 큐 끝에 넣는다.
    //
    // task: 넣을 작업
    //
    // 상한과 관계없이 넣는다. 조각 작업이면 그 NAL 바이트를 아직 쓰지 않은 바이트 합에 더한다.
    void Push(RecordingTask task) override;


    // 아직 쓰지 않은 조각의 바이트 합이 큐 상한보다 작은지 확인한다.
    //
    // 반환값: 상한보다 작으면 true
    //
    // 바이트 합은 녹화 스레드가 그 조각을 다 쓴 뒤에 줄어든다. 그래서 디스크가 막힌 동안 처리 중인 조각도 상한에 든다.
    bool HasRoom() const override;


    // 큐가 비고 처리 중인 작업이 없을 때까지 기다린다.
    //
    // 테스트에서 녹화 스레드의 결과를 확인하기 전에 쓴다. 다른 스레드가 계속 작업을 넣으면 끝나지 않을 수 있다.
    void WaitUntilIdle();


private:
    // 녹화 스레드의 본체. 정지 요청 뒤 큐가 빌 때까지 작업을 실행하고, 동기화 주기마다 열린 파일을 동기화한다.
    void Run();


    // 녹화 설정. 생성 뒤 바뀌지 않는다.
    const RecordingOptions options_;


    // 파일 작업을 실행하는 저장소. 녹화 스레드만 접근한다.
    RecordingFileStore store_;


    // 아래 공유 상태를 보호하는 잠금. HasRoom이 const이므로 mutable이다.
    mutable std::mutex mutex_;


    // 큐에 작업이 들어왔거나 정지가 요청되었음을 녹화 스레드에 알림
    std::condition_variable work_available_;


    // 큐가 비고 처리 중인 작업이 없어졌음을 WaitUntilIdle에 알림
    std::condition_variable idle_;


    // 실행을 기다리는 작업. mutex_로 보호한다.
    std::deque<RecordingTask> queue_;


    // 큐에 있거나 실행 중인 조각 작업의 NAL 바이트 합. mutex_로 보호한다.
    std::size_t queued_bytes_ = 0;


    // 녹화 스레드가 작업을 실행하는 중인지 여부. mutex_로 보호한다.
    bool is_task_running_ = false;


    // 소멸자가 정지를 요청했는지 여부. mutex_로 보호한다.
    bool is_stop_requested_ = false;


    // 녹화 스레드. 다른 멤버를 모두 초기화한 뒤 생성자 본문에서 시작하므로 마지막에 둔다.
    std::thread thread_;
};


}  // namespace vms::recording
