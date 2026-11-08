#include "http/byte_range.h"

#include <optional>
#include <string_view>

#include "http/http_request.h"
#include "test_support.h"

using vms::http::ByteRange;
using vms::http::HttpRequest;
using vms::http::ParseRangeHeader;
using vms::http::ResolveByteRange;
using vms::http::SelectRequestedRange;
using vms::test::Expect;


namespace {


// Range 헤더 값을 해석하고 파일 크기에 맞춰 응답할 바이트 범위를 정한다.
//
// value: Range 헤더 값
// file_size: 파일 크기(바이트)
// 반환값: 응답할 바이트 범위. 범위를 무시하거나 만족할 수 없으면 빈 optional
std::optional<ByteRange> ParseAndResolve(std::string_view value, std::uint64_t file_size) {
    const auto spec = ParseRangeHeader(value);
    if (!spec) {
        return std::nullopt;
    }
    return ResolveByteRange(*spec, file_size);
}


// 응답할 바이트 범위가 기대한 시작 위치와 끝 위치(포함)인지 확인한다.
//
// range: 검사할 범위
// first_position: 기대하는 시작 위치
// last_position: 기대하는 끝 위치
// 반환값: 범위가 있고 시작·끝이 같으면 true
bool IsRange(const std::optional<ByteRange>& range, std::uint64_t first_position,
             std::uint64_t last_position) {
    return range && range->first_position == first_position &&
           range->first_position + range->length - 1 == last_position;
}


// 명세 "범위 요청"의 시나리오 값을 크기 100인 파일에서 계산하는지 검사한다.
void TestResolvesSatisfiableRanges() {
    Expect(IsRange(ParseAndResolve("bytes=0-9", 100), 0, 9), "bytes=0-9 → 0~9");
    Expect(IsRange(ParseAndResolve("bytes=90-", 100), 90, 99), "bytes=90- → 90~99");
    Expect(IsRange(ParseAndResolve("bytes=95-200", 100), 95, 99), "bytes=95-200 → 95~99");
    Expect(IsRange(ParseAndResolve("bytes=-20", 100), 80, 99), "bytes=-20 → 80~99");
    Expect(IsRange(ParseAndResolve("bytes=-200", 100), 0, 99), "bytes=-200 → 0~99");
    Expect(IsRange(ParseAndResolve("bytes=0-", 100), 0, 99), "bytes=0- → 0~99");
    Expect(IsRange(ParseAndResolve("Bytes=0-0", 100), 0, 0), "단위의 대소문자는 구분하지 않는다");
}


// 명세 "만족할 수 없는 범위"의 시나리오가 해석은 되지만 계산 결과가 없는지 검사한다.
void TestRejectsUnsatisfiableRanges() {
    const auto start_at_size = ParseRangeHeader("bytes=100-");
    Expect(start_at_size && !ResolveByteRange(*start_at_size, 100), "크기 100의 bytes=100-");

    const auto zero_suffix = ParseRangeHeader("bytes=-0");
    Expect(zero_suffix && !ResolveByteRange(*zero_suffix, 100), "bytes=-0");

    const auto empty_file = ParseRangeHeader("bytes=0-");
    Expect(empty_file && !ResolveByteRange(*empty_file, 0), "크기 0의 bytes=0-");

    const auto empty_file_suffix = ParseRangeHeader("bytes=-5");
    Expect(empty_file_suffix && !ResolveByteRange(*empty_file_suffix, 0), "크기 0의 bytes=-5");
}


// 명세 "범위 요청 무시"의 형식들을 범위 없음으로 해석하는지 검사한다.
void TestIgnoresInvalidRangeHeaders() {
    Expect(!ParseRangeHeader("bytes=0-9,20-29"), "여러 범위");
    Expect(!ParseRangeHeader("bytes=abc"), "숫자가 아닌 범위");
    Expect(!ParseRangeHeader("items=0-9"), "bytes가 아닌 단위");
    Expect(!ParseRangeHeader("bytes=9-0"), "끝 위치가 시작 위치보다 작음");
    Expect(!ParseRangeHeader("bytes=0-18446744073709551616"), "uint64를 넘는 끝 위치");
    Expect(!ParseRangeHeader("bytes=18446744073709551616-"), "uint64를 넘는 시작 위치");
    Expect(!ParseRangeHeader("bytes=-"), "숫자가 없는 범위");
    Expect(!ParseRangeHeader("bytes=+1-2"), "부호가 붙은 위치");
    Expect(!ParseRangeHeader("bytes 0-9"), "= 없는 값");
}


// 요청의 Range·If-Range 상태에 따라 적용할 범위를 고르는지 검사한다.
void TestSelectsRequestedRange() {
    HttpRequest request;
    Expect(!SelectRequestedRange(request), "Range가 없으면 범위 없음");

    request.range = "bytes=0-9";
    Expect(SelectRequestedRange(request).has_value(), "Range 하나는 적용");

    request.has_if_range = true;
    Expect(!SelectRequestedRange(request), "If-Range가 함께 오면 범위 없음");

    HttpRequest repeated;
    repeated.is_range_repeated = true;
    Expect(!SelectRequestedRange(repeated), "Range가 여러 번 오면 범위 없음");
}


}  // namespace


int main() {
    TestResolvesSatisfiableRanges();
    TestRejectsUnsatisfiableRanges();
    TestIgnoresInvalidRangeHeaders();
    TestSelectsRequestedRange();
    return vms::test::ExitCode();
}
