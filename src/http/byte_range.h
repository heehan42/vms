#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

#include "http/http_request.h"


namespace vms::http {


// 시작 위치로 정한 범위. "bytes=a-b" 또는 "bytes=a-"를 해석한 값이다.
//
// 위치는 파일 시작으로부터의 바이트 단위이며 0부터 센다. 끝 위치는 범위에 포함된다.
struct FirstLastByteRangeSpec {
    // 범위의 시작 위치(a)
    std::uint64_t first_position = 0;


    // 범위의 끝 위치(b). "bytes=a-"처럼 생략했으면 빈 optional이며, 파일 끝까지를 뜻한다.
    // 값이 있으면 first_position 이상이다.
    std::optional<std::uint64_t> last_position;
};


// 파일 마지막 n바이트를 요청한 범위. "bytes=-n"을 해석한 값이다.
struct SuffixByteRangeSpec {
    // 요청한 바이트 수(n). 0이면 만족할 수 없는 범위다.
    std::uint64_t length = 0;
};


// 파일 크기 없이 Range 헤더 하나를 해석한 결과.
//
// 파일 크기는 파일을 연 뒤에야 알 수 있으므로, 실제로 응답할 바이트는 ResolveByteRange로 정한다.
using ByteRangeSpec = std::variant<FirstLastByteRangeSpec, SuffixByteRangeSpec>;


// 파일 크기에 맞춰 정한, 실제로 응답할 바이트 범위.
struct ByteRange {
    // 응답할 첫 바이트의 위치. 파일 시작으로부터의 바이트 단위다.
    std::uint64_t first_position = 0;


    // 응답할 바이트 수. 1 이상이다.
    std::uint64_t length = 0;
};


// Range 헤더 값을 범위 하나로 해석한다.
//
// value: 앞뒤 공백을 뺀 Range 헤더 값. 예: "bytes=0-9"
// 반환값: 해석한 범위. 다음 경우에는 범위를 무시해야 하므로 빈 optional
//   - 단위가 bytes가 아님(대소문자는 구분하지 않음)
//   - 범위가 둘 이상(쉼표 포함)
//   - 위치가 10진수 숫자가 아니거나 uint64 범위를 넘음
//   - 끝 위치가 시작 위치보다 작음
std::optional<ByteRangeSpec> ParseRangeHeader(std::string_view value);


// 요청에서 파일 응답에 적용할 범위를 정한다.
//
// request: 파싱한 요청
// 반환값: 적용할 범위. Range가 없거나, 여러 번 왔거나, If-Range가 함께 왔거나, ParseRangeHeader가
//         범위를 무시하라고 하면 빈 optional이며 파일 전체를 200으로 보내야 한다.
//
// 서버는 ETag와 Last-Modified를 보내지 않아 If-Range 값이 현재 파일과 같은지 판단할 수 없다.
// 그래서 If-Range가 오면 RFC 9110이 허용하는 대로 범위를 무시한다.
std::optional<ByteRangeSpec> SelectRequestedRange(const HttpRequest& request);


// 해석한 범위를 파일 크기에 맞춰 실제로 응답할 바이트 범위로 바꾼다.
//
// spec: ParseRangeHeader가 해석한 범위
// file_size: 파일 크기(바이트)
// 반환값: 응답할 바이트 범위. 범위가 파일 안에 한 바이트도 없으면 빈 optional이며 416으로 응답해야 한다.
//
// 끝 위치가 파일 끝을 넘으면 파일 끝까지로 줄이고, 마지막 n바이트가 파일보다 크면 파일 전체로 줄인다.
std::optional<ByteRange> ResolveByteRange(const ByteRangeSpec& spec, std::uint64_t file_size);


}  // namespace vms::http
