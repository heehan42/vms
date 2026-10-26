#pragma once

#include <filesystem>
#include <string_view>


namespace vms::http {


// 파일 확장자로 응답의 Content-Type 값을 정한다.
//
// path: 응답할 파일 경로. 확장자만 사용한다.
// 반환값: Content-Type 헤더 값. 알 수 없는 확장자이면 "application/octet-stream".
//         정적 문자열을 가리키므로 수명 제한이 없다.
//
// 확장자는 대소문자를 구분한다.
std::string_view ContentTypeForPath(const std::filesystem::path& path);


}  // namespace vms::http
