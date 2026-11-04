#include "rtsp/rtsp_url.h"

#include <netdb.h>
#include <sys/socket.h>

#include <charconv>
#include <format>
#include <memory>

#include "core/ascii.h"


namespace vms::rtsp {


namespace {


// URL의 스킴과 구분자
constexpr std::string_view kSchemePrefix = "rtsp://";


}  // namespace


std::expected<RtspUrl, std::string> ParseRtspUrl(std::string_view text) {
    if (text.size() < kSchemePrefix.size() ||
        !core::EqualsIgnoringAsciiCase(text.substr(0, kSchemePrefix.size()), kSchemePrefix)) {
        return std::unexpected(std::format("rtsp:// URL이 아님: {}", text));
    }

    const std::string_view rest = text.substr(kSchemePrefix.size());
    const std::size_t path_begin = rest.find('/');
    const std::string_view authority = rest.substr(0, path_begin);
    if (authority.contains('@')) {
        return std::unexpected(std::format("사용자 정보(@)가 있는 URL은 지원하지 않음(인증 미지원): {}", text));
    }

    RtspUrl url;
    url.request_url = std::string(kSchemePrefix) + std::string(rest);
    url.path = path_begin == std::string_view::npos ? std::string() : std::string(rest.substr(path_begin));

    const std::size_t port_separator = authority.find(':');
    url.host = authority.substr(0, port_separator);
    if (url.host.empty()) {
        return std::unexpected(std::format("URL에 호스트가 없음: {}", text));
    }
    if (port_separator != std::string_view::npos) {
        const std::string_view port_text = authority.substr(port_separator + 1);
        unsigned int port = 0;
        const auto [end, error] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        if (error != std::errc() || end != port_text.data() + port_text.size() || port == 0 || port > 65535) {
            return std::unexpected(std::format("URL의 포트가 1~65535 정수가 아님: {}", text));
        }
        url.port = static_cast<std::uint16_t>(port);
    }
    return url;
}


std::expected<in_addr, std::string> ResolveIpv4Address(const std::string& host) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* raw_results = nullptr;
    const int result = ::getaddrinfo(host.c_str(), nullptr, &hints, &raw_results);
    if (result != 0) {
        return std::unexpected(std::format("호스트를 IPv4 주소로 해석할 수 없음 ({}): {}", host, ::gai_strerror(result)));
    }

    const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> results(raw_results, &::freeaddrinfo);
    return reinterpret_cast<const sockaddr_in*>(results->ai_addr)->sin_addr;
}


}  // namespace vms::rtsp
