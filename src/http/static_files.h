#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "http/http_response.h"


namespace vms::http {


// 요청 대상을 웹 루트 안의 파일 경로로 바꾼다.
//
// web_root: 정적 파일을 제공하는 디렉터리
// request_target: 요청 줄의 대상 원문. 예: "/app.js?v=1"
// 반환값: 제공할 일반 파일의 경로. 실패하면 응답할 상태 코드
//   - kBadRequest: 대상이 '/'로 시작하지 않음
//   - kForbidden: 경로 구성 요소에 ".."이 있음
//   - kNotFound: 파일이 없거나 일반 파일이 아님
//
// 쿼리 문자열은 무시하고, "/"는 "/index.html"로 취급한다. 퍼센트 인코딩은 해석하지 않는다.
// 웹 루트 안에 심볼릭 링크가 없다고 가정하므로, 링크를 따라간 실제 위치는 검사하지 않는다.
std::expected<std::filesystem::path, HttpStatus> ResolveStaticFilePath(
    const std::filesystem::path& web_root, std::string_view request_target);


// 파일 전체를 메모리로 읽는다.
//
// path: 읽을 파일 경로
// 반환값: 파일 내용. 열기나 읽기에 실패하면 시스템 오류 코드
//
// 웹 페이지와 스크립트 같은 작은 파일을 위한 함수이며, 큰 파일에는 적합하지 않다.
std::expected<std::string, std::error_code> ReadFileContent(const std::filesystem::path& path);


}  // namespace vms::http
