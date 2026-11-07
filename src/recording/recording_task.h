#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "recording/fmp4.h"


namespace vms::recording {


// 카메라의 새 녹화 파일을 여는 작업.
struct OpenFileTask {
    // 파일 시작 시각(유닉스 밀리초, UTC). 파일 이름과 색인 헤더가 되며, 파일 안 재생 시각 0이 이 시각이다.
    std::int64_t start_unix_ms = 0;


    // 초기화 구간의 avcC에 넣을 SPS(시작 코드 제외)
    std::vector<std::uint8_t> sps;


    // 초기화 구간의 avcC에 넣을 PPS(시작 코드 제외)
    std::vector<std::uint8_t> pps;
};


// 열린 파일에 GOP 하나를 조각으로 덧붙이는 작업.
struct WriteFragmentTask {
    // 디코딩 순서대로의 프레임. 하나 이상이며 첫 프레임이 키프레임이다. 재생 시각은 파일 시작 기준이다.
    std::vector<Fmp4Frame> frames;
};


// 열린 파일을 동기화하고 닫는 작업.
struct CloseFileTask {};


// 카메라 하나에 대한 파일 작업.
//
// 이벤트 루프 스레드에서 계획기(CameraRecordingPlanner)가 만들어 값으로 녹화 스레드에 넘기고, 녹화 스레드의 파일
// 저장소(RecordingFileStore)가 실행한다. 같은 카메라의 작업은 만든 순서대로 실행된다.
struct RecordingTask {
    // 카메라 이름. 녹화 디렉터리 아래 폴더 이름이다.
    std::string camera_name;


    // 할 일
    std::variant<OpenFileTask, WriteFragmentTask, CloseFileTask> action;
};


// 작업이 큐 상한에서 차지하는 바이트 수를 구한다.
//
// task: 셀 작업
// 반환값: WriteFragmentTask이면 모든 NAL 바이트의 합, 아니면 0
inline std::size_t QueuedBytesOf(const RecordingTask& task) {
    const auto* write = std::get_if<WriteFragmentTask>(&task.action);
    if (write == nullptr) {
        return 0;
    }
    std::size_t bytes = 0;
    for (const Fmp4Frame& frame : write->frames) {
        for (const auto& nal_unit : frame.nal_units) {
            bytes += nal_unit.size();
        }
    }
    return bytes;
}


// 파일 작업을 받는 대상.
//
// 녹화 스레드의 작업 큐(RecordingWriter)가 구현한다. 테스트에서는 작업을 모으는 가짜 대상으로 바꾼다.
class RecordingTaskSink {
public:
    // 대상을 기반 클래스 포인터로 지울 수 있게 한다.
    virtual ~RecordingTaskSink() = default;


    // 작업을 넣는다.
    //
    // task: 넣을 작업. 소유권이 대상으로 넘어간다.
    //
    // 상한과 관계없이 항상 받아들인다. 상한은 넣는 쪽이 HasRoom으로 미리 확인한다. 이벤트 루프 스레드에서 부르며,
    // 디스크 작업을 기다리지 않고 바로 돌아와야 한다.
    virtual void Push(RecordingTask task) = 0;


    // 아직 쓰지 않은 조각의 바이트 합이 상한보다 작은지 확인한다.
    //
    // 반환값: 상한보다 작으면 true
    //
    // 넣는 쪽은 키프레임에서 이 값으로 그 GOP를 받을지 정한다. 받기로 한 GOP는 끝까지 넣으므로 합은 상한을 넘을 수 있다.
    virtual bool HasRoom() const = 0;
};


}  // namespace vms::recording
