#include "core/network_interfaces.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#include <algorithm>
#include <array>
#include <format>
#include <memory>

#include "core/system_error.h"


namespace vms::core {


std::expected<std::vector<in_addr>, std::error_code> ListNonLoopbackIpv4Addresses() {
    ifaddrs* raw_interfaces = nullptr;
    if (::getifaddrs(&raw_interfaces) != 0) {
        return std::unexpected(LastSystemError());
    }
    const std::unique_ptr<ifaddrs, decltype(&::freeifaddrs)> interfaces(raw_interfaces,
                                                                         &::freeifaddrs);

    std::vector<in_addr> addresses;
    for (const ifaddrs* entry = interfaces.get(); entry != nullptr; entry = entry->ifa_next) {
        // 주소가 없는 항목(링크 계층 정보 등)과 IPv4가 아닌 주소는 건너뛴다.
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        const in_addr address = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)->sin_addr;
        const bool is_duplicate = std::ranges::any_of(addresses, [&](const in_addr& existing) {
            return existing.s_addr == address.s_addr;
        });
        if (!is_duplicate) {
            addresses.push_back(address);
        }
    }
    return addresses;
}


std::string FormatIpv4Address(in_addr address) {
    std::array<char, INET_ADDRSTRLEN> text{};
    ::inet_ntop(AF_INET, &address, text.data(), text.size());
    return std::string(text.data());
}


std::optional<in_addr> ParseIpv4Address(std::string_view text) {
    // inet_pton은 NUL로 끝나는 문자열을 받으므로 복사한다.
    const std::string terminated(text);
    in_addr address{};
    if (::inet_pton(AF_INET, terminated.c_str(), &address) != 1) {
        return std::nullopt;
    }
    return address;
}


std::string FormatIpv4Endpoint(const sockaddr_in& endpoint) {
    return std::format("{}:{}", FormatIpv4Address(endpoint.sin_addr), ntohs(endpoint.sin_port));
}


}  // namespace vms::core
