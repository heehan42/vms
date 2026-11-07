#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <system_error>

#include "core/unique_fd.h"
#include "recording/recorded_file.h"
#include "recording/recording_options.h"
#include "recording/recording_task.h"


namespace vms::recording {


// 녹화 디렉터리가 있는 파일 시스템의 여유 공간을 statvfs로 조회한다.
//
// path: 그 파일 시스템 안의 경로
// 반환값: 일반 사용자가 쓸 수 있는 바이트 수(f_bavail × f_frsize). 조회에 실패하면 오류 코드
std::expected<std::uint64_t, std::error_code> QueryFileSystemFreeSpace(const std::filesystem::path& path);


// 카메라 폴더의 녹화 파일 짝(.mp4·.idx)을 파일 작업대로 만들고 쓰고 닫는 저장소.
//
// 파일 열기·조각 쓰기·닫기와 동기화, 보관 분량 삭제, 비상 삭제를 한다. 스레드를 만들지 않으며 한 스레드(녹화
// 스레드)에서만 쓴다. 카메라 폴더(<녹화 디렉터리>/<카메라 이름>)는 만든 쪽(main)이 미리 준비해야 한다.
//
// 녹화 오류로 서버를 멈추지 않는다. 실패하면 로그를 남기고 그 파일을 닫으며, 그 카메라의 조각은 다음 열기 작업까지
// 버린다. 열린 파일 디스크립터를 소유하며, 소멸할 때 열린 파일을 동기화해 닫는다.
class RecordingFileStore {
public:
    // 저장소를 만든다. 파일은 열지 않는다.
    //
    // options: 녹화 설정. 범위 검사를 마친 값이어야 한다.
    // closed_files: 시작할 때 이미 있는 닫힌 파일 목록(복구 결과). 등록되지 않은 카메라 폴더도 포함한다.
    RecordingFileStore(RecordingOptions options, RecordedFileCatalog closed_files);


    // 열린 파일을 모두 동기화해 닫는다.
    ~RecordingFileStore();


    // 파일 디스크립터를 소유하므로 복사를 금지한다.
    RecordingFileStore(const RecordingFileStore&) = delete;


    // 파일 디스크립터를 소유하므로 복사 대입을 금지한다.
    RecordingFileStore& operator=(const RecordingFileStore&) = delete;


    // 파일 작업 하나를 실행한다.
    //
    // task: 실행할 작업. 같은 카메라의 작업은 만든 순서대로 넘겨야 한다.
    void Execute(const RecordingTask& task);


    // 마지막 동기화 뒤 데이터를 쓴 열린 파일을 모두 녹화 파일, 색인 파일 순서로 동기화한다.
    void SyncOpenFiles();


    // 열린 파일을 모두 동기화해 닫는다. 닫은 파일은 보관 분량 검사를 받는다.
    void CloseAllFiles();


private:
    // 카메라 하나의 쓰고 있는 파일.
    //
    // 두 파일 디스크립터를 소유하며, 닫을 때까지 이 저장소만 접근한다.
    struct OpenRecording {
        // 확장자를 뺀 파일 이름
        std::string stem;


        // 파일 시작 시각(유닉스 밀리초)
        std::int64_t start_unix_ms = 0;


        // 녹화 파일(.mp4)
        core::UniqueFd video_fd;


        // 색인 파일(.idx)
        core::UniqueFd index_fd;


        // 녹화 파일에 완전히 쓴 바이트 수. 다음 조각의 위치이며, 쓰기 실패 시 이 길이로 되돌린다.
        std::uint64_t video_size = 0;


        // 색인 파일에 완전히 쓴 바이트 수. 다음 레코드의 위치이며, 쓰기 실패 시 이 길이로 되돌린다.
        std::uint64_t index_size = 0;


        // 다음 조각의 번호(1부터)
        std::uint32_t next_sequence_number = 1;


        // 마지막 조각의 끝 재생 시각(90kHz). 닫을 때 파일 길이가 된다.
        std::uint64_t end_time = 0;


        // 쓴 조각 수
        std::size_t fragment_count = 0;


        // 마지막 동기화 뒤에 쓴 데이터가 있는지 여부
        bool has_unsynced_data = false;
    };


    // 비상 삭제 검사 뒤 새 파일 짝을 만들고 초기화 구간과 색인 헤더를 쓴다. 이미 열린 파일이 있으면 먼저 닫는다.
    void OpenFile(const std::string& camera_name, const OpenFileTask& task);


    // 조각과 그 색인 레코드를 쓴다. 열린 파일이 없으면 버린다.
    void WriteFragment(const std::string& camera_name, const WriteFragmentTask& task);


    // 열린 파일을 닫는다. 열린 파일이 없으면 아무것도 하지 않는다.
    void CloseFile(const std::string& camera_name);


    // 열린 파일을 마지막으로 완전히 쓴 위치까지 되돌리고 동기화해 닫는다.
    //
    // 조각이 하나 이상이면 닫힌 파일 목록에 더하고 보관 분량을 검사한다. 조각이 없으면 파일 짝을 지운다.
    void FinishFile(const std::string& camera_name);


    // 카메라의 닫힌 파일 길이 합이 보관 분량을 넘으면 가장 오래된 파일부터 지운다.
    void ApplyRetention(const std::string& camera_name);


    // 여유 공간이 기준보다 적으면 기준 이상이 되거나 지울 닫힌 파일이 없을 때까지 가장 오래된 단위 구간을 지운다.
    void DeleteWhileLowOnSpace();


    // 모든 카메라 폴더에서 가장 오래된 단위 구간에 속한 닫힌 파일을 함께 지운다.
    //
    // 반환값: 지울 닫힌 파일이 있었으면 true
    bool DeleteOldestSegment();


    // 공간 부족으로 실패하면 비상 삭제를 한 번 한 뒤 다시 쓴다.
    //
    // fd: 쓸 파일
    // bytes: 쓸 바이트
    // offset: 쓸 위치
    // 반환값: 성공하면 빈 오류 코드. 실패하면 마지막 오류
    std::error_code WriteWithSpaceRetry(int fd, std::span<const std::uint8_t> bytes, std::uint64_t offset);


    // 여유 공간을 조회한다. 설정의 조회 함수가 비어 있으면 statvfs를 쓴다.
    std::expected<std::uint64_t, std::error_code> QueryFreeSpace() const;


    // 카메라 폴더의 파일 짝 경로의 앞부분(확장자 제외)을 만든다.
    std::filesystem::path StemPath(const std::string& camera_name, const std::string& stem) const;


    // 녹화 설정
    RecordingOptions options_;


    // 카메라 폴더별 닫힌 파일 목록. 시작 시각 순서다.
    RecordedFileCatalog closed_files_;


    // 카메라별 쓰고 있는 파일
    std::map<std::string, OpenRecording> open_files_;
};


}  // namespace vms::recording
