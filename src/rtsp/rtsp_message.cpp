#include "rtsp/rtsp_message.h"

#include <algorithm>
#include <charconv>
#include <format>

#include "core/ascii.h"


namespace vms::rtsp {


namespace {


// 헤더 영역의 끝 표시
constexpr std::string_view kHeaderTerminator = "\r\n\r\n";


// 줄 끝 표시
constexpr std::string_view kLineEnd = "\r\n";


// 응답 상태 줄의 버전 접두사
constexpr std::string_view kStatusLinePrefix = "RTSP/1.0 ";


// 요청에 넣는 User-Agent 값
constexpr std::string_view kUserAgent = "vms";


// 10진수 문자열 전체를 부호 없는 정수로 바꾼다.
//
// text: 바꿀 문자열
// 반환값: 값. 비었거나 숫자가 아닌 문자가 있거나 범위를 넘으면 빈 optional
template <typename Integer>
std::optional<Integer> ParseDecimal(std::string_view text) {
    Integer value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}


// ';'로 구분된 매개변수 목록에서 "name=value"의 value를 찾는다.
//
// parameters: 예: "RTP/AVP;unicast;server_port=6970-6971"
// name: 찾을 매개변수 이름(대소문자 무시)
// 반환값: '=' 뒤의 값(앞뒤 공백 제외). 없으면 빈 optional. parameters가 가리키는 원본이 살아 있는 동안 유효하다.
std::optional<std::string_view> FindParameterValue(std::string_view parameters, std::string_view name) {
    while (!parameters.empty()) {
        const std::size_t separator = parameters.find(';');
        const std::string_view parameter = core::TrimAsciiWhitespace(parameters.substr(0, separator));
        const std::size_t equals = parameter.find('=');
        if (equals != std::string_view::npos &&
            core::EqualsIgnoringAsciiCase(core::TrimAsciiWhitespace(parameter.substr(0, equals)), name)) {
            return core::TrimAsciiWhitespace(parameter.substr(equals + 1));
        }
        if (separator == std::string_view::npos) {
            break;
        }
        parameters.remove_prefix(separator + 1);
    }
    return std::nullopt;
}


// 상태 줄을 해석한다.
//
// line: 줄 끝을 뺀 상태 줄
// response: 상태 코드와 이유 문구를 채울 응답
// 반환값: 성공하면 true
bool ParseStatusLine(std::string_view line, RtspResponse& response) {
    if (!line.starts_with(kStatusLinePrefix)) {
        return false;
    }
    line.remove_prefix(kStatusLinePrefix.size());
    const auto status_code = ParseDecimal<int>(line.substr(0, 3));
    if (!status_code || *status_code < 100 || (line.size() > 3 && line[3] != ' ')) {
        return false;
    }
    response.status_code = *status_code;
    response.reason_phrase = line.size() > 4 ? std::string(line.substr(4)) : std::string();
    return true;
}


// Base64 문자 하나의 6비트 값을 돌려준다.
//
// character: Base64 문자
// 반환값: 0~63. 표준 Base64 알파벳이 아니면 빈 optional
std::optional<std::uint8_t> Base64Value(char character) {
    if (character >= 'A' && character <= 'Z') {
        return static_cast<std::uint8_t>(character - 'A');
    }
    if (character >= 'a' && character <= 'z') {
        return static_cast<std::uint8_t>(character - 'a' + 26);
    }
    if (character >= '0' && character <= '9') {
        return static_cast<std::uint8_t>(character - '0' + 52);
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return std::nullopt;
}


// Base64 문자열 하나를 바이트로 되돌린다.
//
// text: 끝의 '=' 채움은 있어도 없어도 된다.
// 반환값: 되돌린 바이트. 알파벳이 아닌 문자가 있거나 길이가 맞지 않으면 빈 optional
std::optional<std::vector<std::uint8_t>> DecodeBase64(std::string_view text) {
    while (text.ends_with('=')) {
        text.remove_suffix(1);
    }
    if (text.size() % 4 == 1) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes;
    std::uint32_t accumulator = 0;
    int bit_count = 0;
    for (const char character : text) {
        const auto value = Base64Value(character);
        if (!value) {
            return std::nullopt;
        }
        accumulator = (accumulator << 6) | *value;
        bit_count += 6;
        if (bit_count >= 8) {
            bit_count -= 8;
            bytes.push_back(static_cast<std::uint8_t>(accumulator >> bit_count));
        }
    }
    return bytes;
}


}  // namespace


std::optional<std::string_view> RtspResponse::FindHeader(std::string_view name) const {
    for (const RtspHeader& header : headers) {
        if (core::EqualsIgnoringAsciiCase(header.name, name)) {
            return header.value;
        }
    }
    return std::nullopt;
}


std::string BuildRtspRequest(std::string_view method, std::string_view url, std::uint32_t cseq,
                             std::span<const RtspHeader> headers) {
    std::string request = std::format("{} {} RTSP/1.0\r\nCSeq: {}\r\nUser-Agent: {}\r\n", method, url, cseq, kUserAgent);
    for (const RtspHeader& header : headers) {
        request += std::format("{}: {}\r\n", header.name, header.value);
    }
    request += kLineEnd;
    return request;
}


std::expected<std::optional<RtspResponse>, std::string> ParseRtspResponse(std::string_view bytes) {
    const std::size_t header_end = bytes.find(kHeaderTerminator);
    if (header_end == std::string_view::npos) {
        if (bytes.size() > kMaxRtspHeaderSize) {
            return std::unexpected(std::format("응답 헤더가 {}바이트를 넘음", kMaxRtspHeaderSize));
        }
        return std::nullopt;
    }
    const std::size_t body_begin = header_end + kHeaderTerminator.size();
    if (body_begin > kMaxRtspHeaderSize) {
        return std::unexpected(std::format("응답 헤더가 {}바이트를 넘음", kMaxRtspHeaderSize));
    }

    RtspResponse response;
    std::string_view header_lines = bytes.substr(0, header_end);
    const std::size_t status_line_end = header_lines.find(kLineEnd);
    if (!ParseStatusLine(header_lines.substr(0, status_line_end), response)) {
        return std::unexpected(std::format("응답 상태 줄 형식 오류: {}", header_lines.substr(0, status_line_end)));
    }
    header_lines = status_line_end == std::string_view::npos ? std::string_view()
                                                             : header_lines.substr(status_line_end + kLineEnd.size());

    while (!header_lines.empty()) {
        const std::size_t line_end = header_lines.find(kLineEnd);
        const std::string_view line = header_lines.substr(0, line_end);
        const std::size_t colon = line.find(':');
        const std::string_view name = colon == std::string_view::npos ? std::string_view()
                                                                      : core::TrimAsciiWhitespace(line.substr(0, colon));
        if (name.empty()) {
            return std::unexpected(std::format("응답 헤더 줄 형식 오류: {}", line));
        }
        response.headers.push_back(
            RtspHeader{.name = std::string(name), .value = std::string(core::TrimAsciiWhitespace(line.substr(colon + 1)))});
        header_lines = line_end == std::string_view::npos ? std::string_view()
                                                          : header_lines.substr(line_end + kLineEnd.size());
    }

    std::size_t content_length = 0;
    if (const auto length_text = response.FindHeader("Content-Length")) {
        const auto parsed = ParseDecimal<std::size_t>(*length_text);
        if (!parsed) {
            return std::unexpected(std::format("잘못된 Content-Length: {}", *length_text));
        }
        content_length = *parsed;
    }
    if (content_length > kMaxRtspBodySize) {
        return std::unexpected(std::format("응답 본문이 {}바이트를 넘음 ({})", kMaxRtspBodySize, content_length));
    }
    if (bytes.size() - body_begin < content_length) {
        return std::nullopt;
    }

    response.body = std::string(bytes.substr(body_begin, content_length));
    response.consumed_size = body_begin + content_length;
    return response;
}


std::expected<RtspSession, std::string> ParseSessionHeader(std::string_view value) {
    const std::size_t separator = value.find(';');
    RtspSession session{.id = std::string(core::TrimAsciiWhitespace(value.substr(0, separator)))};
    if (session.id.empty()) {
        return std::unexpected(std::format("Session 헤더에 ID가 없음: {}", value));
    }
    if (separator == std::string_view::npos) {
        return session;
    }

    if (const auto timeout_text = FindParameterValue(value.substr(separator + 1), "timeout")) {
        const auto timeout = ParseDecimal<unsigned int>(*timeout_text);
        if (!timeout || *timeout == 0) {
            return std::unexpected(std::format("Session 헤더의 timeout이 양의 정수가 아님: {}", value));
        }
        session.timeout = std::chrono::seconds(*timeout);
    }
    return session;
}


std::expected<RtspTransport, std::string> ParseTransportHeader(std::string_view value) {
    const std::string_view specifier = core::TrimAsciiWhitespace(value.substr(0, value.find(';')));
    if (!core::EqualsIgnoringAsciiCase(specifier, "RTP/AVP") && !core::EqualsIgnoringAsciiCase(specifier, "RTP/AVP/UDP")) {
        return std::unexpected(std::format("UDP 전송이 아닌 Transport: {}", value));
    }

    bool is_unicast = false;
    for (std::string_view rest = value; !rest.empty();) {
        const std::size_t separator = rest.find(';');
        is_unicast = is_unicast || core::EqualsIgnoringAsciiCase(core::TrimAsciiWhitespace(rest.substr(0, separator)),
                                                                 "unicast");
        rest = separator == std::string_view::npos ? std::string_view() : rest.substr(separator + 1);
    }
    if (!is_unicast) {
        return std::unexpected(std::format("unicast가 아닌 Transport: {}", value));
    }

    const auto server_port = FindParameterValue(value, "server_port");
    const std::size_t dash = server_port ? server_port->find('-') : std::string_view::npos;
    const auto rtp_port = dash != std::string_view::npos ? ParseDecimal<std::uint16_t>(server_port->substr(0, dash))
                                                         : std::nullopt;
    const auto rtcp_port = dash != std::string_view::npos ? ParseDecimal<std::uint16_t>(server_port->substr(dash + 1))
                                                          : std::nullopt;
    if (!rtp_port || !rtcp_port || *rtp_port == 0 || *rtcp_port == 0) {
        return std::unexpected(std::format("Transport에 server_port 쌍이 없음: {}", value));
    }
    return RtspTransport{.server_rtp_port = *rtp_port, .server_rtcp_port = *rtcp_port};
}


std::string ResolveControlUrl(std::string_view base, std::string_view control) {
    if (control == "*") {
        return std::string(base);
    }
    constexpr std::string_view kScheme = "rtsp://";
    if (control.size() >= kScheme.size() && core::EqualsIgnoringAsciiCase(control.substr(0, kScheme.size()), kScheme)) {
        return std::string(control);
    }

    std::string url(base);
    if (!url.ends_with('/')) {
        url.push_back('/');
    }
    url += control;
    return url;
}


std::string_view ContentBaseOf(const RtspResponse& response, std::string_view request_url) {
    if (const auto content_base = response.FindHeader("Content-Base")) {
        return *content_base;
    }
    if (const auto content_location = response.FindHeader("Content-Location")) {
        return *content_location;
    }
    return request_url;
}


std::expected<RtspVideoTrack, std::string> SelectH264VideoTrack(const webrtc::SdpSessionDescription& description) {
    for (const webrtc::SdpMediaSection& section : description.media_sections) {
        if (section.media != "video") {
            continue;
        }
        for (const std::string& format : section.formats) {
            const auto payload_type = ParseDecimal<unsigned int>(format);
            if (!payload_type || *payload_type > 127) {
                continue;
            }

            // rtpmap 값은 "<payload type> <코덱>/<클록>[/<채널>]" 형식이다.
            const std::string rtpmap_prefix = format + " ";
            const bool is_h264 = std::ranges::any_of(section.attributes, [&](const webrtc::SdpAttribute& attribute) {
                return attribute.name == "rtpmap" && attribute.value.starts_with(rtpmap_prefix) &&
                       core::EqualsIgnoringAsciiCase(
                           core::TrimAsciiWhitespace(std::string_view(attribute.value).substr(rtpmap_prefix.size())),
                           "H264/90000");
            });
            if (!is_h264) {
                continue;
            }

            RtspVideoTrack track{
                .payload_type = static_cast<std::uint8_t>(*payload_type), .control = "*", .sprop_parameter_sets = {}};
            for (const webrtc::SdpAttribute& attribute : section.attributes) {
                if (attribute.name == "control") {
                    track.control = attribute.value;
                } else if (attribute.name == "fmtp" && attribute.value.starts_with(rtpmap_prefix)) {
                    const auto sprop = FindParameterValue(
                        std::string_view(attribute.value).substr(rtpmap_prefix.size()), "sprop-parameter-sets");
                    track.sprop_parameter_sets = sprop ? std::string(*sprop) : std::string();
                }
            }
            return track;
        }
    }
    return std::unexpected("SDP에 H264/90000 비디오 섹션이 없음");
}


std::string SessionControlOf(const webrtc::SdpSessionDescription& description) {
    for (const webrtc::SdpAttribute& attribute : description.session_attributes) {
        if (attribute.name == "control") {
            return attribute.value;
        }
    }
    return "*";
}


std::vector<std::vector<std::uint8_t>> DecodeSpropParameterSets(std::string_view value) {
    std::vector<std::vector<std::uint8_t>> nal_units;
    while (!value.empty()) {
        const std::size_t separator = value.find(',');
        auto decoded = DecodeBase64(core::TrimAsciiWhitespace(value.substr(0, separator)));
        if (decoded && !decoded->empty()) {
            nal_units.push_back(*std::move(decoded));
        }
        if (separator == std::string_view::npos) {
            break;
        }
        value.remove_prefix(separator + 1);
    }
    return nal_units;
}


}  // namespace vms::rtsp
