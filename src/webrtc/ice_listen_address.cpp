#include "webrtc/ice_listen_address.h"

#include <cstdint>
#include <format>
#include <optional>
#include <unordered_set>

#include "core/network_interfaces.h"


namespace vms::webrtc {


namespace {


// `--ice-address` 값 안의 주소 하나를 해석한다.
//
// text: 점 표기 IPv4 주소
// role: 오류 메시지에 넣을 주소의 역할. 예: "bind address"
// 반환값: 해석한 주소. 비어 있거나 IPv4 형식이 아니면 원인을 담은 오류 메시지
std::expected<in_addr, std::string> ParseAddressPart(std::string_view text, std::string_view role) {
    if (text.empty()) {
        return std::unexpected(std::format("{}가 비어 있습니다", role));
    }

    const std::optional<in_addr> address = core::ParseIpv4Address(text);
    if (!address) {
        return std::unexpected(std::format("{}가 IPv4 형식이 아닙니다: {}", role, text));
    }
    return *address;
}


}  // namespace


std::expected<IceListenAddress, std::string> ParseIceListenAddress(std::string_view text) {
    const std::size_t separator = text.find('=');
    const std::string_view bind_text = text.substr(0, separator);
    auto bind_address = ParseAddressPart(bind_text, "bind address");
    if (!bind_address) {
        return std::unexpected(bind_address.error());
    }

    IceListenAddress listen_address{.bind_address = *bind_address, .announced_addresses = {}};
    if (separator == std::string_view::npos) {
        listen_address.announced_addresses.push_back(*bind_address);
        return listen_address;
    }

    // `=` 뒤는 쉼표로 나눈 announced address 목록이다. 빈 항목도 오류로 보기 위해 끝까지 나눈다.
    std::string_view rest = text.substr(separator + 1);
    while (true) {
        const std::size_t comma = rest.find(',');
        auto announced_address = ParseAddressPart(rest.substr(0, comma), "announced address");
        if (!announced_address) {
            return std::unexpected(announced_address.error());
        }
        listen_address.announced_addresses.push_back(*announced_address);

        if (comma == std::string_view::npos) {
            return listen_address;
        }
        rest.remove_prefix(comma + 1);
    }
}


std::expected<void, std::string> CheckIceListenAddresses(std::span<const IceListenAddress> addresses) {
    std::unordered_set<std::uint32_t> bind_addresses;
    std::unordered_set<std::uint32_t> announced_addresses;
    for (const IceListenAddress& listen_address : addresses) {
        if (!bind_addresses.insert(listen_address.bind_address.s_addr).second) {
            return std::unexpected(std::format("같은 bind address를 두 번 지정했습니다: {}",
                                               core::FormatIpv4Address(listen_address.bind_address)));
        }

        for (const in_addr& announced_address : listen_address.announced_addresses) {
            if (!announced_addresses.insert(announced_address.s_addr).second) {
                return std::unexpected(std::format("같은 announced address가 두 번 나옵니다: {}",
                                                   core::FormatIpv4Address(announced_address)));
            }
        }
    }
    return {};
}


}  // namespace vms::webrtc
