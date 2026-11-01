#pragma once

#include <string>
#include <string_view>


namespace vms::core {


// 바이트열이 유효한 UTF-8인지 확인한다.
//
// text: 검사할 바이트열
// 반환값: 모든 문자가 가장 짧은 형식의 UTF-8이고 코드 포인트가 U+0000~U+10FFFF이며 서로게이트
//   (U+D800~U+DFFF)가 아니면 true
//
// 잘린 시퀀스, 과잉 표현(예: '/'를 C0 AF로 씀), 서로게이트, 범위 밖 값을 모두 거부한다. JSON 텍스트는
// UTF-8이어야 하므로, JSON에 넣을 외부 문자열(파일 이름 등)을 미리 거르는 데 쓴다.
bool IsValidUtf8(std::string_view text);


// 문자열을 JSON 문자열 값으로 이스케이프해 덧붙인다.
//
// output: 결과를 덧붙일 문자열
// text: 이스케이프할 문자열. 유효한 UTF-8이어야 한다.
//
// 앞뒤 큰따옴표를 포함해 덧붙인다. '"'와 '\'는 백슬래시로, U+0020 미만의 제어 문자는 \u00XX로 바꾸고
// 나머지 바이트는 그대로 둔다. text가 UTF-8이 아니면 결과도 유효한 JSON이 아니므로 IsValidUtf8로 먼저
// 확인한다.
void AppendJsonString(std::string& output, std::string_view text);


}  // namespace vms::core
