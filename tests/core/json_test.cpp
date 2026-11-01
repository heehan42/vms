#include "core/json.h"

#include <string>
#include <string_view>

#include "test_support.h"

using vms::core::AppendJsonString;
using vms::core::IsValidUtf8;
using vms::test::Expect;


namespace {


// 문자열 하나를 이스케이프한 결과를 돌려준다.
//
// text: 이스케이프할 문자열
// 반환값: 앞뒤 큰따옴표를 포함한 JSON 문자열 값
std::string ToJsonString(std::string_view text) {
    std::string output;
    AppendJsonString(output, text);
    return output;
}


void TestValidUtf8() {
    Expect(IsValidUtf8(""), "빈 문자열");
    Expect(IsValidUtf8("test_pattern.h264"), "ASCII");
    Expect(IsValidUtf8("\xEC\x8B\x9C\xED\x97\x98.h264"), "한글(시험)");
    Expect(IsValidUtf8("\xC2\x80"), "2바이트 최소값 U+0080");
    Expect(IsValidUtf8("\xED\x9F\xBF"), "서로게이트 바로 앞 U+D7FF");
    Expect(IsValidUtf8("\xF0\x9F\x8E\xA5"), "4바이트 문자 U+1F3A5");
    Expect(IsValidUtf8("\xF4\x8F\xBF\xBF"), "최대값 U+10FFFF");
}


void TestInvalidUtf8() {
    Expect(!IsValidUtf8("\xEC\x8B"), "잘린 3바이트 시퀀스");
    Expect(!IsValidUtf8("\xEC\x41\x9C"), "이어지는 바이트가 아님");
    Expect(!IsValidUtf8("\x8B"), "이어지는 바이트로 시작");
    Expect(!IsValidUtf8("\xBD\xC3\xC7\xE8"), "CP949로 쓴 시험");
    Expect(!IsValidUtf8("\xC0\xAF"), "'/'의 2바이트 과잉 표현");
    Expect(!IsValidUtf8("\xE0\x80\xAF"), "'/'의 3바이트 과잉 표현");
    Expect(!IsValidUtf8("\xF0\x8F\xBF\xBF"), "U+FFFF의 4바이트 과잉 표현");
    Expect(!IsValidUtf8("\xED\xA0\x80"), "서로게이트 U+D800");
    Expect(!IsValidUtf8("\xF4\x90\x80\x80"), "U+10FFFF 초과");
    Expect(!IsValidUtf8("\xF8\x88\x80\x80\x80"), "5바이트 형식");
}


void TestAppendJsonString() {
    Expect(ToJsonString("a.h264") == "\"a.h264\"", "그대로 쓰는 ASCII");
    Expect(ToJsonString("\xEC\x8B\x9C\xED\x97\x98 \"1\".h264") == "\"\xEC\x8B\x9C\xED\x97\x98 \\\"1\\\".h264\"",
           "한글은 그대로, 큰따옴표는 이스케이프");
    Expect(ToJsonString("a\\b") == "\"a\\\\b\"", "백슬래시");
    Expect(ToJsonString("a\x01" "b") == "\"a\\u0001b\"", "제어 문자 U+0001");
    Expect(ToJsonString("a\nb\x1F") == "\"a\\u000ab\\u001f\"", "줄바꿈과 U+001F");
    Expect(ToJsonString("/~\x7F") == "\"/~\x7F\"", "U+0020 이상 ASCII는 그대로");

    std::string output = "{\"id\":";
    AppendJsonString(output, "x");
    Expect(output == "{\"id\":\"x\"", "기존 내용 뒤에 덧붙임");
}


}  // namespace


int main() {
    TestValidUtf8();
    TestInvalidUtf8();
    TestAppendJsonString();
    return vms::test::ExitCode();
}
