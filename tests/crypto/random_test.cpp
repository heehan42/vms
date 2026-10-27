#include "crypto/random.h"

#include <algorithm>
#include <string_view>

#include "test_support.h"

using vms::crypto::GenerateIceCharString;
using vms::test::Expect;


namespace {


// 요청한 길이만큼 ice-char 문자로만 이루어진 문자열을 만드는지 검사한다.
void TestGeneratesIceCharacters() {
    const auto text = GenerateIceCharString(24);
    if (!text) {
        Expect(false, "난수 문자열 생성이 성공해야 한다");
        return;
    }
    Expect(text->size() == 24, "요청한 길이");

    constexpr std::string_view kAllowed =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Expect(std::ranges::all_of(*text, [&](char character) { return kAllowed.contains(character); }),
           "ice-char 문자만 포함");
}


// 두 번 만든 문자열이 서로 다른지 검사한다.
void TestGeneratesDifferentValues() {
    const auto first = GenerateIceCharString(24);
    const auto second = GenerateIceCharString(24);
    Expect(first && second && *first != *second, "두 번 만든 값은 달라야 한다");
}


}  // namespace


int main() {
    TestGeneratesIceCharacters();
    TestGeneratesDifferentValues();
    return vms::test::ExitCode();
}
