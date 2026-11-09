#pragma once

#include <functional>
#include <string>
#include <variant>

#include "recording/recorded_file.h"


namespace vms::recording {


// 녹화 스레드가 녹화 파일 하나를 동기화해 닫고 닫힌 파일 목록에 더했다는 알림.
struct RecordedFileClosed {
    // 카메라 폴더 이름
    std::string camera_folder;


    // 닫은 파일. 조각 목록을 채운 값이며, 디스크에 남은 색인 레코드와 같다.
    RecordedFile file;
};


// 녹화 스레드가 보관 분량 삭제나 비상 삭제로 닫힌 녹화 파일 짝(.mp4·.idx)을 지웠다는 알림.
struct RecordedFileDeleted {
    // 카메라 폴더 이름
    std::string camera_folder;


    // 지운 파일의 확장자를 뺀 이름
    std::string stem;
};


// 닫힌 녹화 파일 목록의 변경 알림 하나.
using RecordingCatalogEvent = std::variant<RecordedFileClosed, RecordedFileDeleted>;


// 닫힌 녹화 파일 목록의 변경을 받는 함수.
//
// 녹화 스레드에서 변경이 일어난 순서대로 불린다. 녹화 쪽 코드는 받는 쪽이 무엇인지 알지 않으며, 다른 스레드의 상태를
// 고쳐야 하면 받는 함수가 그 스레드로 작업을 넘겨야 한다(예: EventLoop::Post). 빈 함수면 알리지 않는다.
using RecordingCatalogListener = std::move_only_function<void(RecordingCatalogEvent)>;


}  // namespace vms::recording
