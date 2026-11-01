#include "media/video_library.h"

#include <algorithm>
#include <format>
#include <system_error>
#include <utility>

#include "core/ascii.h"
#include "core/json.h"


namespace vms::media {


namespace {


// 동영상 목록에 넣을 확장자인지 확인한다.
//
// name: 디렉터리 항목의 파일 이름
// 반환값: 확장자가 대소문자 구분 없이 .h264 또는 .264이면 true
bool HasVideoExtension(const std::filesystem::path& name) {
    const std::string extension = name.extension().string();
    return core::EqualsIgnoringAsciiCase(extension, ".h264") || core::EqualsIgnoringAsciiCase(extension, ".264");
}


// 디렉터리 바로 아래에서 확장자가 맞는 항목의 이름을 모은다.
//
// directory: 동영상 디렉터리
// 반환값: 파일 이름 목록(순서 없음). 디렉터리가 아니거나 열거나 순회할 수 없으면 원인 메시지
std::expected<std::vector<std::string>, std::string> ListVideoFileNames(const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        return std::unexpected(std::format("동영상 디렉터리가 아님 ({}){}", directory.string(),
                                           error ? ": " + error.message() : std::string()));
    }

    std::filesystem::directory_iterator iterator(directory, error);
    if (error) {
        return std::unexpected(std::format("동영상 디렉터리를 열 수 없음 ({}): {}", directory.string(), error.message()));
    }
    std::vector<std::string> names;
    for (; iterator != std::filesystem::directory_iterator(); iterator.increment(error)) {
        // increment가 실패하면 반복자가 끝 위치가 된다고 기대하지만, 구현에 따라 그렇지 않을 수 있어
        // 실패한 반복자를 역참조하지 않도록 먼저 멈춘다.
        if (error) {
            break;
        }
        const std::filesystem::path name = iterator->path().filename();
        if (HasVideoExtension(name)) {
            names.push_back(name.string());
        }
    }
    if (error) {
        return std::unexpected(std::format("동영상 디렉터리를 읽을 수 없음 ({}): {}", directory.string(), error.message()));
    }
    return names;
}


}  // namespace


std::expected<VideoLibraryLoadResult, std::string> VideoLibrary::LoadDirectory(const std::filesystem::path& directory,
                                                                               const VideoLibraryLimits& limits) {
    auto names = ListVideoFileNames(directory);
    if (!names) {
        return std::unexpected(names.error());
    }
    // 순회 순서는 파일 시스템마다 다르므로, 정렬해야 목록 순서와 합계 상한으로 빠지는 파일이 일정하다.
    // std::string 비교는 바이트를 부호 없는 값으로 비교한다.
    std::ranges::sort(*names);

    std::vector<VideoEntry> entries;
    std::vector<std::string> warnings;
    std::uintmax_t total_bytes = 0;
    for (std::string& name : *names) {
        const std::filesystem::path path = directory / name;
        std::error_code error;
        if (std::filesystem::is_directory(path, error)) {
            continue;
        }
        if (!core::IsValidUtf8(name)) {
            warnings.push_back(std::format("동영상 파일 건너뜀 ({}): 이름이 UTF-8이 아님", path.string()));
            continue;
        }

        // 크기를 먼저 확인해, 합계 상한을 넘는 파일은 읽지 않고 건너뛴다.
        // 파일당 상한을 넘는 파일은 H264File::Load가 읽기 전에 거부한다.
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (error) {
            warnings.push_back(std::format("동영상 파일을 열 수 없음 ({}): {}", path.string(), error.message()));
            continue;
        }
        if (size <= kMaxH264FileSize && total_bytes + size > limits.max_total_bytes) {
            warnings.push_back(std::format("동영상 파일 건너뜀 ({}): 읽으면 목록 합계가 {}바이트를 넘음",
                                           path.string(), limits.max_total_bytes));
            continue;
        }

        auto file = H264File::Load(path);
        if (!file) {
            warnings.push_back(file.error());
            continue;
        }
        total_bytes += size;
        entries.push_back(VideoEntry{.id = std::move(name), .file = *std::move(file)});
    }

    if (entries.empty()) {
        return std::unexpected(std::format("쓸 수 있는 동영상이 없음 ({})", directory.string()));
    }
    return VideoLibraryLoadResult{.library = VideoLibrary(std::move(entries)), .warnings = std::move(warnings)};
}


const VideoEntry* VideoLibrary::Find(std::string_view id) const {
    const auto found = std::ranges::find(entries_, id, &VideoEntry::id);
    return found != entries_.end() ? &*found : nullptr;
}


}  // namespace vms::media
