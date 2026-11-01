#include "http/query_string.h"

#include <optional>
#include <string>
#include <string_view>

#include "test_support.h"

using vms::http::FindQueryParameter;
using vms::http::QueryParameterError;
using vms::test::Expect;


namespace {


// 찾은 값이 기대값과 같은지 확인한다.
//
// target: 요청 대상
// expected: 기대하는 디코딩 값
// description: 실패 시 출력할 설명
void ExpectValue(std::string_view target, std::string_view expected, std::string_view description) {
    const auto found = FindQueryParameter(target, "video");
    Expect(found.has_value() && found->has_value() && **found == expected, description);
}


// 매개변수가 없다고 판단하는지 확인한다.
//
// target: 요청 대상
// description: 실패 시 출력할 설명
void ExpectAbsent(std::string_view target, std::string_view description) {
    const auto found = FindQueryParameter(target, "video");
    Expect(found.has_value() && !found->has_value(), description);
}


// 기대한 오류로 거부하는지 확인한다.
//
// target: 요청 대상
// error: 기대하는 오류
// description: 실패 시 출력할 설명
void ExpectError(std::string_view target, QueryParameterError error, std::string_view description) {
    const auto found = FindQueryParameter(target, "video");
    Expect(!found.has_value() && found.error() == error, description);
}


}  // namespace


int main() {
    ExpectAbsent("/offer", "쿼리 없음");
    ExpectAbsent("/offer?", "빈 쿼리");
    ExpectAbsent("/offer?debug=1&other=2", "매개변수 없음");
    ExpectAbsent("/offer?videos=a.h264", "이름이 접두어만 같음");

    ExpectValue("/offer?video=a.h264", "a.h264", "단순 값");
    ExpectValue("/offer?debug=1&video=b.h264&x=", "b.h264", "여러 쌍 중간");
    ExpectValue("/offer?video=%EC%8B%9C%ED%97%98.h264", "\xEC\x8B\x9C\xED\x97\x98.h264", "퍼센트 인코딩된 한글");
    ExpectValue("/offer?video=%ec%8b%9c", "\xEC\x8B\x9C", "소문자 16진수");
    ExpectValue("/offer?video=a+b%20c", "a b c", "'+'와 %20은 공백");
    ExpectValue("/offer?video=a%2Bb%26c%3Dd", "a+b&c=d", "인코딩된 구분자는 값의 일부");
    ExpectValue("/offer?video=..%2Fsecret.h264", "../secret.h264", "경로처럼 보이는 값도 그대로 디코딩");
    ExpectValue("/offer?video", "", "'=' 없는 쌍");
    ExpectValue("/offer?video=", "", "빈 값");
    ExpectValue("/offer?vid%65o=a.h264", "a.h264", "인코딩된 이름");
    ExpectValue("/offer?&&video=a.h264&", "a.h264", "빈 쌍 무시");

    ExpectError("/offer?video=a.h264&video=b.h264", QueryParameterError::kDuplicate, "중복");
    ExpectError("/offer?video=a.h264&video", QueryParameterError::kDuplicate, "값 없는 중복");
    ExpectError("/offer?video=%zz", QueryParameterError::kInvalidEncoding, "16진수가 아님");
    ExpectError("/offer?video=abc%4", QueryParameterError::kInvalidEncoding, "끝에서 한 자리");
    ExpectError("/offer?video=abc%", QueryParameterError::kInvalidEncoding, "끝의 '%'");
    ExpectError("/offer?video=a.h264&debug=%G0", QueryParameterError::kInvalidEncoding, "다른 이름의 잘못된 인코딩");
    return vms::test::ExitCode();
}
