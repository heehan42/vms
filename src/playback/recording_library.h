#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "recording/recorded_file.h"
#include "recording/recording_catalog_event.h"


namespace vms::playback {


// 카메라 폴더별 재생할 수 있는 녹화 파일 목록. 키는 폴더 이름이고, 값은 시작 시각 오름차순의 닫힌 파일 목록이다.
//
// 비교 함수가 std::less<>라 std::string_view로 바로 찾을 수 있다. 값의 파일 목록은 비어 있지 않다.
using RecordingFolderMap = std::map<std::string, std::vector<recording::RecordedFile>, std::less<>>;


// 이벤트 루프 스레드가 재생 목록과 소스 목록을 만드는 데 쓰는 닫힌 녹화 파일과 조각 목록.
//
// 서버가 시작할 때 녹화 복구 결과로 만들고, 녹화 스레드의 알림(RecordedFileClosed, RecordedFileDeleted)을 Apply()로
// 반영한다. 이벤트 루프 스레드에서만 읽고 고치므로 잠금이 없다. 녹화 스레드는 이 객체를 직접 부르지 않고,
// EventLoop::Post()로 넘긴 작업이 Apply()를 부른다.
//
// 이 객체는 디스크를 읽지 않는다. 목록에 있는 파일이 디스크에서 이미 지워졌을 수 있으며(삭제 알림이 아직 처리되기 전),
// 그 경우 파일 응답이 파일을 열지 못해 404로 응답한다.
class RecordingLibrary {
public:
    // 재생할 녹화가 없는 빈 목록을 만든다.
    RecordingLibrary() = default;


    // 녹화 복구 결과로 목록을 만든다.
    //
    // catalog: 카메라 폴더별 닫힌 파일 목록. 파일마다 조각 목록을 채운 값이어야 한다.
    //
    // 이름이 UTF-8이 아닌 폴더는 소스 목록 JSON에 넣을 수 없으므로 빼고 표준 오류에 로그를 남긴다. 파일이 없는 폴더도
    // 뺀다.
    explicit RecordingLibrary(recording::RecordedFileCatalog catalog);


    // 녹화 스레드의 알림 하나를 목록에 반영한다.
    //
    // event: 녹화 스레드가 보낸 알림
    //
    // 닫기 알림은 그 파일을 폴더 목록의 시작 시각 순서 자리에 더한다(폴더가 없으면 만든다). 삭제 알림은 그 파일을
    // 빼고, 폴더에 파일이 남지 않으면 폴더를 지운다. 목록에 없는 파일의 삭제 알림과 이름이 UTF-8이 아닌 폴더의 알림은
    // 무시한다. 이 호출 전에 Find·Folders로 얻은 포인터와 참조는 무효가 될 수 있다.
    void Apply(recording::RecordingCatalogEvent event);


    // 폴더 하나의 닫힌 파일 목록을 찾는다.
    //
    // camera_folder: 폴더 이름. 바이트 단위로 비교한다.
    // 반환값: 시작 시각 순서의 파일 목록(비어 있지 않음). 재생할 녹화가 없는 폴더면 nullptr. 다음 Apply() 호출 전까지
    //         유효하다.
    const std::vector<recording::RecordedFile>* FindFolder(std::string_view camera_folder) const;


    // 폴더 안의 닫힌 파일 하나를 찾는다.
    //
    // camera_folder: 폴더 이름. 바이트 단위로 비교한다.
    // stem: 확장자를 뺀 파일 이름. 바이트 단위로 비교한다.
    // 반환값: 찾은 파일. 없으면 nullptr. 다음 Apply() 호출 전까지 유효하다.
    const recording::RecordedFile* FindFile(std::string_view camera_folder, std::string_view stem) const;


    // 재생할 녹화가 있는 모든 폴더를 돌려준다.
    //
    // 반환값: 폴더 이름의 바이트 순서로 정렬된 폴더 목록. 이 객체가 살아 있는 동안 유효하지만, Apply()가 내용을 바꾼다.
    const RecordingFolderMap& Folders() const { return folders_; }


private:
    // 폴더 이름별 닫힌 파일 목록. 이름은 모두 유효한 UTF-8이고, 파일 목록은 비어 있지 않다.
    RecordingFolderMap folders_;
};


}  // namespace vms::playback
