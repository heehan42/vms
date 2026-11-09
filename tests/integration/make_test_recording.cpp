// 통합 시험에서 서버에 넘길 녹화 디렉터리를 만드는 도구.
//
// 사용법: make_test_recording <녹화 디렉터리> <카메라 폴더 이름> <파일 시작 유닉스 밀리초> <조각 수>
//
// RecordingFileStore로 합성 프레임(1초 GOP마다 IDR 하나, 일반 슬라이스 하나)을 써서 닫힌 녹화 파일 짝(.mp4·.idx)
// 하나를 만든다. 카메라 폴더가 없으면 만든다. 서버가 시작할 때 녹화 복구가 이 파일을 그대로 남긴다.

#include <charconv>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <print>
#include <string_view>
#include <system_error>

#include "recording/recording_file_store.h"
#include "recording/recording_test_support.h"

namespace fs = std::filesystem;


namespace {


// 조각 하나의 길이(1초, 1/90000초 단위)
constexpr std::uint32_t kFragmentDuration = 90000;


// 프레임 하나의 슬라이스 NAL 크기(바이트)
constexpr std::size_t kPayloadSize = 1000;


// 10진수 정수 인자 하나를 읽는다.
//
// text: 인자 문자열
// value: 읽은 값을 받을 변수
// 반환값: 문자열 전체가 정수이면 true
template <typename Integer>
bool ParseInteger(std::string_view text, Integer& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc() && end == text.data() + text.size();
}


}  // namespace


int main(int argc, char* argv[]) {
    std::int64_t start_unix_ms = 0;
    std::uint32_t fragment_count = 0;
    if (argc != 5 || !ParseInteger(argv[3], start_unix_ms) || !ParseInteger(argv[4], fragment_count) ||
        fragment_count == 0) {
        std::println(stderr, "사용법: make_test_recording <녹화 디렉터리> <카메라 폴더 이름> <파일 시작 유닉스 밀리초> "
                             "<조각 수(1 이상)>");
        return 2;
    }
    const fs::path directory = argv[1];
    const std::string camera_folder = argv[2];

    std::error_code error;
    fs::create_directories(directory / camera_folder, error);
    if (error) {
        std::println(stderr, "카메라 폴더를 만들 수 없음 ({}): {}", (directory / camera_folder).string(), error.message());
        return 1;
    }

    // 보관 분량 삭제와 비상 삭제가 일어나지 않게 보관 분량을 최대로, 여유 공간을 무한으로 둔다.
    vms::recording::RecordingOptions options;
    options.directory = directory;
    options.retention_seconds = 604800;
    options.min_free_bytes = 0;
    options.query_free_space = [](const fs::path&) -> std::expected<std::uint64_t, std::error_code> {
        return std::numeric_limits<std::uint64_t>::max();
    };
    vms::recording::RecordingFileStore store(std::move(options), {}, {});

    using namespace vms::recording::test;
    store.Execute(MakeOpenTask(camera_folder, start_unix_ms));
    for (std::uint32_t index = 0; index < fragment_count; ++index) {
        store.Execute(MakeWriteTask(camera_folder, index * kFragmentDuration, kFragmentDuration, kPayloadSize));
    }
    store.Execute(MakeCloseTask(camera_folder));
    return 0;
}
