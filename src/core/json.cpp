#include "core/json.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>


namespace vms::core {


namespace {


// UTF-8 이어지는 바이트(10xxxxxx)인지 확인한다.
//
// value: 검사할 바이트
// 반환값: 상위 두 비트가 10이면 true
constexpr bool IsContinuationByte(std::uint8_t value) { return (value & 0xC0) == 0x80; }


}  // namespace


bool IsValidUtf8(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<std::uint8_t>(text[index]);
        if (lead < 0x80) {
            ++index;
            continue;
        }

        // 첫 바이트로 길이와 그 길이가 가장 짧은 형식이 되는 최소 코드 포인트를 정한다.
        // 최소값보다 작은 값은 더 짧게 쓸 수 있었으므로 과잉 표현이다.
        std::size_t length = 0;
        char32_t code_point = 0;
        char32_t minimum = 0;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            code_point = lead & 0x1F;
            minimum = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            code_point = lead & 0x0F;
            minimum = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            code_point = lead & 0x07;
            minimum = 0x10000;
        } else {
            // 이어지는 바이트가 첫 자리에 왔거나 5바이트 이상 형식(F8~FF)이다.
            return false;
        }
        if (text.size() - index < length) {
            return false;
        }

        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<std::uint8_t>(text[index + offset]);
            if (!IsContinuationByte(next)) {
                return false;
            }
            code_point = (code_point << 6) | (next & 0x3F);
        }
        if (code_point < minimum || code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
            return false;
        }
        index += length;
    }
    return true;
}


void AppendJsonString(std::string& output, std::string_view text) {
    output.push_back('"');
    for (const char character : text) {
        if (character == '"' || character == '\\') {
            output.push_back('\\');
            output.push_back(character);
        } else if (static_cast<std::uint8_t>(character) < 0x20) {
            std::format_to(std::back_inserter(output), "\\u{:04x}", static_cast<unsigned int>(character));
        } else {
            output.push_back(character);
        }
    }
    output.push_back('"');
}


}  // namespace vms::core
