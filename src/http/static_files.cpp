#include "http/static_files.h"

#include <ranges>


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

    return file_path;
}


}  // namespace vms::http
