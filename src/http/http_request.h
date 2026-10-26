#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "http/http_response.h"


namespace vms::http {


// 요청 줄과 헤더를 합한 요청 헤더 영역의 최대 크기(바이트).
//
// 빈 줄(\r\n\r\n)을 포함한 크기가 이 값을 넘으면 400으로 응답한다.
inline constexpr std::size_t kMaxRequestHeaderSize = 8 * 1024;


// 파싱한 HTTP 요청 중 서버가 사용하는 부분.
//
// 헤더는 문법만 검사하고 보관하지 않는다. 사용하는 헤더가 생기면 필드를 추가한다.
struct HttpRequest {
    // 요청 메서드. 예: "GET"
    std::string method;


    // 요청 대상. 쿼리 문자열을 포함한 원문 그대로다. 예: "/app.js?v=1"
    std::string target;
};


// 지금까지 받은 데이터에서 요청 헤더 영역의 끝을 찾는다.
//
// received: 연결에서 지금까지 받은 바이트
// 반환값: 탐지 결과
//   - 값이 있는 optional: 빈 줄(\r\n\r\n)을 포함한 요청 헤더 영역의 크기
//   - 빈 optional: 아직 헤더 끝이 오지 않았으므로 더 받아야 함
//   - HttpStatus::kBadRequest: 헤더 영역이 kMaxRequestHeaderSize를 넘음
std::expected<std::optional<std::size_t>, HttpStatus> FindRequestHeaderEnd(
    std::string_view received);


// 요청 헤더 영역의 문법을 검사하고 요청 줄을 파싱한다.
//
// header: FindRequestHeaderEnd가 찾은 크기만큼의 요청 헤더 영역(마지막 빈 줄 포함)
// 반환값: 파싱한 요청. 요청 줄이나 헤더 줄의 형식이 잘못되었으면 HttpStatus::kBadRequest
//
// 요청 줄은 "메서드 SP 대상 SP HTTP/1.x" 형식만, 헤더 줄은 "이름: 값" 형식만 허용한다.
// 메서드를 지원하는지는 판단하지 않으며, 라우팅 단계에서 결정한다.
std::expected<HttpRequest, HttpStatus> ParseRequestHeader(std::string_view header);


}  // namespace vms::http
