#include "http/byte_range.h"

#include <algorithm>
#include <charconv>
#include <system_error>

#include "core/ascii.h"


namespace vms::http {


namespace {


// 위치 숫자를 uint64로 바꾼다.
//
// text: 10진수 숫자만으로 이루어져야 하는 문자열
// 반환값: 바꾼 값. 비었거나 숫자가 아닌 문자가 있거나 uint64 범위를 넘으면 빈 optional
std::optional<std::uint64_t> ParsePosition(std::string_view text) {
    std::uint64_t position = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), position);
    if (text.empty() || error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return position;
}


}  // namespace


std::optional<ByteRangeSpec> ParseRangeHeader(std::string_view value) {
    const std::size_t equals = value.find('=');
    if (equals == std::string_view::npos ||
        !core::EqualsIgnoringAsciiCase(value.substr(0, equals), "bytes")) {
        return std::nullopt;
    }

    // 범위 여러 개는 multipart/byteranges 본문이 필요하므로 지원하지 않고 무시한다.
    const std::string_view range = value.substr(equals + 1);
    if (range.contains(',')) {
        return std::nullopt;
    }
    const std::size_t dash = range.find('-');
    if (dash == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view first_text = range.substr(0, dash);
    const std::string_view last_text = range.substr(dash + 1);

    if (first_text.empty()) {
        const auto suffix_length = ParsePosition(last_text);
        if (!suffix_length) {
            return std::nullopt;
        }
        return SuffixByteRangeSpec{.length = *suffix_length};
    }

    const auto first_position = ParsePosition(first_text);
    if (!first_position) {
        return std::nullopt;
    }
    if (last_text.empty()) {
        return FirstLastByteRangeSpec{.first_position = *first_position, .last_position = std::nullopt};
    }
    const auto last_position = ParsePosition(last_text);
    if (!last_position || *last_position < *first_position) {
        return std::nullopt;
    }
    return FirstLastByteRangeSpec{.first_position = *first_position, .last_position = *last_position};
}


std::optional<ByteRangeSpec> SelectRequestedRange(const HttpRequest& request) {
    if (!request.range || request.is_range_repeated || request.has_if_range) {
        return std::nullopt;
    }
    return ParseRangeHeader(*request.range);
}


std::optional<ByteRange> ResolveByteRange(const ByteRangeSpec& spec, std::uint64_t file_size) {
    if (const auto* suffix = std::get_if<SuffixByteRangeSpec>(&spec)) {
        if (suffix->length == 0 || file_size == 0) {
            return std::nullopt;
        }
        const std::uint64_t length = std::min(suffix->length, file_size);
        return ByteRange{.first_position = file_size - length, .length = length};
    }

    const auto& first_last = std::get<FirstLastByteRangeSpec>(spec);
    if (first_last.first_position >= file_size) {
        return std::nullopt;
    }
    const std::uint64_t last_position =
        std::min(first_last.last_position.value_or(file_size - 1), file_size - 1);
    return ByteRange{.first_position = first_last.first_position,
                     .length = last_position - first_last.first_position + 1};
}


}  // namespace vms::http
