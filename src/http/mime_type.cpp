#include "http/mime_type.h"

#include <array>


namespace vms::http {


namespace {


// 확장자와 Content-Type 값의 대응 한 쌍
struct ContentTypeMapping {
    // 점을 포함한 확장자. 예: ".html"
    std::string_view extension;


    // Content-Type 헤더 값
    std::string_view content_type;
};


// 서버가 구분하는 확장자 목록
constexpr std::array kContentTypeMappings = {
    ContentTypeMapping{".html", "text/html; charset=utf-8"},
    ContentTypeMapping{".js", "text/javascript; charset=utf-8"},
    ContentTypeMapping{".css", "text/css; charset=utf-8"},
    ContentTypeMapping{".json", "application/json"},
    ContentTypeMapping{".svg", "image/svg+xml"},
    ContentTypeMapping{".png", "image/png"},
    ContentTypeMapping{".ico", "image/x-icon"},
};


}  // namespace


std::string_view ContentTypeForPath(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    for (const ContentTypeMapping& mapping : kContentTypeMappings) {
        if (mapping.extension == extension) {
            return mapping.content_type;
        }
    }
    return "application/octet-stream";
}


}  // namespace vms::http
