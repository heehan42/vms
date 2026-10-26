#include "http/static_files.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <ranges>

#include "core/system_error.h"
#include "core/unique_fd.h"


namespace vms::http {


std::expected<std::filesystem::path, HttpStatus> ResolveStaticFilePath(
    const std::filesystem::path& web_root, std::string_view request_target) {
    const std::string_view target_path = request_target.substr(0, request_target.find('?'));
    if (!target_path.starts_with('/')) {
        return std::unexpected(HttpStatus::kBadRequest);
    }
    if (target_path == "/") {
        return ResolveStaticFilePath(web_root, "/index.html");
    }

    // 구성 요소를 하나씩 붙여야 "//etc/passwd"처럼 빈 구성 요소 뒤의 절대 경로가
    // 웹 루트를 대체하지 않는다.
    std::filesystem::path file_path = web_root;
    for (const auto component_range : std::views::split(target_path, '/')) {
        const std::string_view component(component_range.begin(), component_range.end());
        if (component == "..") {
            return std::unexpected(HttpStatus::kForbidden);
        }
        if (!component.empty()) {
            file_path /= component;
        }
    }

    std::error_code status_error;
    const std::filesystem::file_status file_status =
        std::filesystem::status(file_path, status_error);
    if (status_error || !std::filesystem::is_regular_file(file_status)) {
        return std::unexpected(HttpStatus::kNotFound);
    }
    return file_path;
}


std::expected<std::string, std::error_code> ReadFileContent(const std::filesystem::path& path) {
    const core::UniqueFd file(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!file.IsValid()) {
        return std::unexpected(core::LastSystemError());
    }

    std::string content;
    std::array<char, 16 * 1024> chunk;
    while (true) {
        const ssize_t read_size = ::read(file.Get(), chunk.data(), chunk.size());
        if (read_size < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(core::LastSystemError());
        }
        if (read_size == 0) {
            return content;
        }
        content.append(chunk.data(), static_cast<std::size_t>(read_size));
    }
}


}  // namespace vms::http
