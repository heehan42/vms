#include "http/http_request.h"

#include <algorithm>
#include <charconv>
#include <ranges>
#include <utility>

#include "core/ascii.h"


namespace vms::http {


namespace {


// 요청 헤더 영역의 끝을 나타내는 빈 줄
constexpr std::string_view kHeaderTerminator = "\r\n\r\n";


// 줄 구분자
constexpr std::string_view kLineBreak = "\r\n";


// 문자가 HTTP 토큰(메서드, 헤더 이름)에 쓸 수 있는 문자(tchar)인지 확인한다.
//
// character: 검사할 문자
// 반환값: 영문자, 숫자, RFC 9110이 허용하는 기호이면 true
bool IsTokenCharacter(char character) {
    if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9')) {
        return true;
    }
    constexpr std::string_view kTokenSymbols = "!#$%&'*+-.^_`|~";
    return kTokenSymbols.contains(character);
}


// 문자열이 비어 있지 않은 HTTP 토큰인지 확인한다.
//
// text: 검사할 문자열
// 반환값: 한 글자 이상이고 모든 문자가 토큰 문자이면 true
bool IsToken(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, IsTokenCharacter);
}


// 문자열에 제어 문자(탭 제외)가 없는지 확인한다.
//
// text: 검사할 문자열
// 반환값: 0x00~0x1F(탭 제외)와 0x7F가 없으면 true
bool HasNoControlCharacter(std::string_view text) {
    return std::ranges::none_of(text, [](char character) {
        const auto code = static_cast<unsigned char>(character);
        return (code < 0x20 && character != '\t') || code == 0x7F;
    });
}


// "메서드 SP 대상 SP HTTP/1.x" 형식의 요청 줄을 파싱한다.
//
// line: 줄 구분자를 제외한 요청 줄
// 반환값: 메서드와 대상. 형식이 잘못되었으면 빈 optional
std::optional<HttpRequest> ParseRequestLine(std::string_view line) {
    const std::size_t first_space = line.find(' ');
    if (first_space == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t second_space = line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) {
        return std::nullopt;
    }

    const std::string_view method = line.substr(0, first_space);
    const std::string_view target = line.substr(first_space + 1, second_space - first_space - 1);
    const std::string_view version = line.substr(second_space + 1);

    const bool is_valid_target = !target.empty() && !target.contains(' ') &&
                                 HasNoControlCharacter(target);
    const bool is_valid_version = version.size() == 8 && version.starts_with("HTTP/1.") &&
                                  version[7] >= '0' && version[7] <= '9';
    if (!IsToken(method) || !is_valid_target || !is_valid_version) {
        return std::nullopt;
    }
    HttpRequest request;
    request.method = method;
    request.target = target;
    return request;
}


// 헤더 줄 하나의 이름과 값.
//
// 두 필드 모두 파싱한 헤더 영역을 가리키므로, 원본 문자열이 살아 있는 동안에만 유효하다.
struct HeaderField {
    // 헤더 이름. 원문의 대소문자 그대로다.
    std::string_view name;


    // 앞뒤 공백과 탭을 뺀 헤더 값
    std::string_view value;
};


// "이름: 값" 형식의 헤더 줄을 이름과 값으로 나눈다.
//
// line: 줄 구분자를 제외한 헤더 줄
// 반환값: 이름과 앞뒤 공백을 뺀 값. 이름이 토큰이 아니거나 값에 제어 문자가 있으면 빈 optional
std::optional<HeaderField> ParseHeaderLine(std::string_view line) {
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view name = line.substr(0, colon);
    const std::string_view value = line.substr(colon + 1);
    if (!IsToken(name) || !HasNoControlCharacter(value)) {
        return std::nullopt;
    }
    return HeaderField{.name = name, .value = core::TrimAsciiWhitespace(value)};
}


// Content-Length 값을 바이트 수로 바꾼다.
//
// value: 앞뒤 공백을 뺀 헤더 값
// 반환값: 바이트 수. 10진수 숫자만으로 이루어지지 않았거나 size_t 범위를 넘으면 빈 optional
std::optional<std::size_t> ParseContentLength(std::string_view value) {
    std::size_t length = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), length);
    if (value.empty() || error != std::errc() || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return length;
}


// 본문 수신과 라우팅에 쓰는 헤더이면 그 값을 요청에 기록한다.
//
// field: 파싱한 헤더 줄
// request: 값을 기록할 요청
// 반환값: Content-Length가 잘못되었거나 앞서 받은 값과 다르면 false
bool RecordUsedHeader(const HeaderField& field, HttpRequest& request) {
    if (core::EqualsIgnoringAsciiCase(field.name, "Content-Length")) {
        const auto length = ParseContentLength(field.value);
        if (!length) {
            return false;
        }
        // 서로 다른 길이가 오면 어느 쪽이 본문 경계인지 알 수 없다.
        if (request.content_length && *request.content_length != *length) {
            return false;
        }
        request.content_length = *length;
    } else if (core::EqualsIgnoringAsciiCase(field.name, "Content-Type")) {
        request.content_type = std::string(field.value);
    } else if (core::EqualsIgnoringAsciiCase(field.name, "Transfer-Encoding")) {
        request.has_transfer_encoding = true;
    }
    return true;
}


}  // namespace


std::expected<std::optional<std::size_t>, HttpStatus> FindRequestHeaderEnd(
    std::string_view received) {
    const std::size_t terminator_position = received.find(kHeaderTerminator);
    if (terminator_position == std::string_view::npos) {
        // 헤더 끝이 오지 않은 채 한도를 넘었다면 더 기다려도 한도 안에서 끝날 수 없다.
        if (received.size() > kMaxRequestHeaderSize) {
            return std::unexpected(HttpStatus::kBadRequest);
        }
        return std::nullopt;
    }

    const std::size_t header_size = terminator_position + kHeaderTerminator.size();
    if (header_size > kMaxRequestHeaderSize) {
        return std::unexpected(HttpStatus::kBadRequest);
    }
    return header_size;
}


std::expected<HttpRequest, HttpStatus> ParseRequestHeader(std::string_view header) {
    if (!header.ends_with(kHeaderTerminator)) {
        return std::unexpected(HttpStatus::kBadRequest);
    }

    // 마지막 빈 줄을 떼어 내면 "요청 줄\r\n헤더\r\n...헤더" 형태가 된다.
    std::string_view remaining = header.substr(0, header.size() - kHeaderTerminator.size());

    const std::size_t request_line_end = remaining.find(kLineBreak);
    auto request = ParseRequestLine(remaining.substr(0, request_line_end));
    if (!request) {
        return std::unexpected(HttpStatus::kBadRequest);
    }
    if (request_line_end == std::string_view::npos) {
        return *std::move(request);
    }

    remaining.remove_prefix(request_line_end + kLineBreak.size());
    for (const auto line : std::views::split(remaining, kLineBreak)) {
        const auto field = ParseHeaderLine(std::string_view(line.begin(), line.end()));
        if (!field || !RecordUsedHeader(*field, *request)) {
            return std::unexpected(HttpStatus::kBadRequest);
        }
    }
    return *std::move(request);
}


}  // namespace vms::http
