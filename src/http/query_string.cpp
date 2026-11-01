#include "http/query_string.h"

#include <cstddef>


namespace vms::http {


namespace {


// 16진수 문자 하나의 값을 구한다.
//
// character: 검사할 문자
// 반환값: '0'~'9', 'A'~'F', 'a'~'f'이면 0~15, 아니면 빈 optional
std::optional<int> HexDigitValue(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    return std::nullopt;
}


// 쿼리 문자열의 이름이나 값 하나를 퍼센트 디코딩한다.
//
// encoded: 디코딩할 문자열
// 반환값: %XX를 바이트로, '+'를 공백으로 바꾼 결과. '%' 뒤에 16진수 두 자리가 없으면 kInvalidEncoding
std::expected<std::string, QueryParameterError> DecodeQueryComponent(std::string_view encoded) {
    std::string decoded;
    decoded.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const char character = encoded[index];
        if (character == '+') {
            decoded.push_back(' ');
            continue;
        }
        if (character != '%') {
            decoded.push_back(character);
            continue;
        }

        const std::optional<int> high = index + 1 < encoded.size() ? HexDigitValue(encoded[index + 1]) : std::nullopt;
        const std::optional<int> low = index + 2 < encoded.size() ? HexDigitValue(encoded[index + 2]) : std::nullopt;
        if (!high || !low) {
            return std::unexpected(QueryParameterError::kInvalidEncoding);
        }
        decoded.push_back(static_cast<char>((*high << 4) | *low));
        index += 2;
    }
    return decoded;
}


}  // namespace


std::expected<std::optional<std::string>, QueryParameterError> FindQueryParameter(std::string_view target,
                                                                                   std::string_view name) {
    const std::size_t query_start = target.find('?');
    if (query_start == std::string_view::npos) {
        return std::nullopt;
    }

    // 다른 이름의 쌍에 있는 잘못된 인코딩도 오류로 보기 위해, 찾은 뒤에도 끝까지 모든 쌍을 디코딩한다.
    std::optional<std::string> found;
    std::string_view remaining = target.substr(query_start + 1);
    while (!remaining.empty()) {
        const std::size_t pair_end = remaining.find('&');
        const std::string_view pair = remaining.substr(0, pair_end);
        remaining = pair_end == std::string_view::npos ? std::string_view() : remaining.substr(pair_end + 1);
        if (pair.empty()) {
            continue;
        }

        const std::size_t separator = pair.find('=');
        auto pair_name = DecodeQueryComponent(pair.substr(0, separator));
        auto pair_value = DecodeQueryComponent(separator == std::string_view::npos ? std::string_view()
                                                                                    : pair.substr(separator + 1));
        if (!pair_name || !pair_value) {
            return std::unexpected(QueryParameterError::kInvalidEncoding);
        }
        if (*pair_name != name) {
            continue;
        }
        if (found) {
            return std::unexpected(QueryParameterError::kDuplicate);
        }
        found = *std::move(pair_value);
    }
    return found;
}


}  // namespace vms::http
