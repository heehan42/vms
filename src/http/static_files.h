#pragma once

#include <expected>
#include <filesystem>
#include <string_view>

#include "http/http_response.h"


namespace vms::http {


// 요청 대상을 웹 루트 안의 파일 경로로 바꾼다.
//
// web_root: 정적 파일을 제공하는 디렉터리
// request_target: 요청 줄의 대상 원문. 예: "/app.js?v=1"
// 반환값: 웹 루트 안의 파일 경로. 실패하면 응답할 상태 코드
//   - kBadRequest: 대상이 '/'로 시작하지 않음
//   - kForbidden: 경로 구성 요소에 ".."이 있음
//
// 문자열 검사만 하고 파일 시스템은 조회하지 않으므로 이벤트 루프 스레드에서 호출할 수 있다.
// 파일이 있는지와 일반 파일인지는 파일 읽기 스레드가 파일을 열 때 확인한다.
// 쿼리 문자열은 무시하고, "/"는 "/index.html"로 취급한다. 퍼센트 인코딩은 해석하지 않는다.
// 웹 루트 안에 심볼릭 링크가 없다고 가정하므로, 링크를 따라간 실제 위치는 검사하지 않는다.
std::expected<std::filesystem::path, HttpStatus> ResolveStaticFilePath(
    const std::filesystem::path& web_root, std::string_view request_target);


}  // namespace vms::http
