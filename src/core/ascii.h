#pragma once

#include <algorithm>
#include <string_view>


namespace vms::core {


// ASCII 대문자를 소문자로 바꾼다. 그 밖의 문자는 그대로 둔다.
//
// character: 바꿀 문자
// 반환값: 'A'~'Z'이면 대응하는 소문자, 아니면 character
constexpr char ToLowerAscii(char character) {
    return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a')
                                                  : character;
}


// 두 문자열이 ASCII 대소문자를 무시하고 같은지 확인한다.
//
// left, right: 비교할 문자열
// 반환값: 길이가 같고 각 문자가 대소문자를 무시하고 같으면 true
//
// HTTP 헤더 이름, 미디어 타입, SDP 코덱 이름처럼 규격상 대소문자를 구분하지 않는 ASCII 토큰 비교에 쓴다.
constexpr bool EqualsIgnoringAsciiCase(std::string_view left, std::string_view right) {
    return std::ranges::equal(left, right, {}, ToLowerAscii, ToLowerAscii);
}


// 문자열 앞뒤의 공백과 탭을 뺀다.
//
// text: 다듬을 문자열
// 반환값: text 안의 부분 문자열. text가 가리키는 원본이 살아 있는 동안에만 유효하다.
constexpr std::string_view TrimAsciiWhitespace(std::string_view text) {
    constexpr std::string_view kWhitespace = " \t";
    const std::size_t begin = text.find_first_not_of(kWhitespace);
    if (begin == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(kWhitespace);
    return text.substr(begin, end - begin + 1);
}


}  // namespace vms::core
