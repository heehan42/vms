#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <system_error>


namespace vms::recording {


// 파일 시스템의 여유 공간을 조회하는 함수.
//
// 인자는 그 파일 시스템 안의 경로이고, 반환값은 일반 사용자가 쓸 수 있는 바이트 수 또는 조회 오류다. 녹화 스레드에서
// 호출한다.
using FreeSpaceQuery = std::function<std::expected<std::uint64_t, std::error_code>(const std::filesystem::path&)>;


// 서버 전체의 녹화 설정.
//
// 명령행에서 받아 범위를 검사한 값을 담는다. 모든 카메라에 같은 값을 쓴다.
struct RecordingOptions {
    // 녹화 디렉터리. 카메라마다 이 아래에 이름이 같은 폴더를 둔다.
    std::filesystem::path directory = "recordings";


    // 파일을 나누는 단위 시간(초). 10~3600
    int segment_seconds = 60;


    // 카메라마다 남길 녹화 분량(초). segment_seconds 이상 604800 이하
    int retention_seconds = 300;


    // 열린 파일을 디스크와 동기화하는 주기(초). 1 이상 segment_seconds 이하
    int sync_interval_seconds = 10;


    // 새 파일을 열기 전에 확보할 최소 여유 공간(바이트). 이보다 적으면 비상 삭제를 한다.
    std::uint64_t min_free_bytes = 1024ULL * 1024 * 1024;


    // 녹화 스레드가 아직 쓰지 않은 조각의 NAL 바이트 합 상한. 합이 이 값 이상이면 키프레임에서 새 GOP를 받지 않는다.
    std::size_t queue_byte_limit = 64 * 1024 * 1024;


    // 여유 공간 조회 함수. 비어 있으면 녹화 디렉터리에 statvfs를 한다. 테스트에서 디스크 부족을 흉내 내는 데 쓴다.
    FreeSpaceQuery query_free_space;
};


}  // namespace vms::recording
