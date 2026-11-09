#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>


namespace vms::http {


// 쿼리 문자열을 해석할 수 없는 이유.
enum class QueryParameterError {
    // '%' 뒤에 16진수 두 자리가 없음
    kInvalidEncoding,

    // 찾는 이름의 매개변수가 두 번 이상 나옴
    kDuplicate,
};


// 요청 대상의 쿼리 문자열에서 이름이 같은 매개변수 하나의 값을 찾는다.
//
// target: 요청 대상 원문. 예: "/offer?video=%EC%8B%9C.h264"
// name: 찾을 매개변수 이름(디코딩한 결과와 비교)
// 반환값: 해석 결과
//   - 값이 있는 optional: 퍼센트 디코딩한 값. '='가 없는 쌍이면 빈 문자열
//   - 빈 optional: 쿼리 문자열이 없거나 그 이름의 매개변수가 없음
//   - kInvalidEncoding: 어느 쌍에든 잘못된 퍼센트 인코딩이 있음
//   - kDuplicate: 그 이름의 매개변수가 두 번 이상 나옴
//
// 브라우저의 URLSearchParams와 같이 '?' 뒤를 '&'로 나누고, 이름과 값의 %XX를 바이트로, '+'를 공백으로
// 바꾼다. '#' 뒤의 프래그먼트는 브라우저가 보내지 않으므로 따로 처리하지 않는다. 디코딩한 값이 유효한
// UTF-8인지는 확인하지 않으며, 호출자는 값을 정해진 목록과 비교하는 데만 써야 한다.
std::expected<std::optional<std::string>, QueryParameterError> FindQueryParameter(std::string_view target,
                                                                                   std::string_view name);


// 쿼리 매개변수의 이름이나 값 하나를 퍼센트 인코딩한다.
//
// value: 인코딩할 바이트열. 보통 UTF-8 문자열이다.
// 반환값: 영문자, 숫자, '-', '.', '_', '~'(RFC 3986의 unreserved 문자)는 그대로 두고, 나머지 바이트를 대문자 16진수
//         "%XX"로 바꾼 문자열
//
// 결과에는 '&', '=', '"', 공백, 줄바꿈이 남지 않으므로, 쿼리 문자열이나 HLS 재생 목록의 따옴표 속성 값에 그대로 넣을
// 수 있다. FindQueryParameter로 디코딩하면 원래 값이 나온다.
std::string EncodeQueryComponent(std::string_view value);


}  // namespace vms::http
