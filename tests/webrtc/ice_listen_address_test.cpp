#include "webrtc/ice_listen_address.h"

#include <string>
#include <string_view>
#include <vector>

#include "core/network_interfaces.h"
#include "test_support.h"

using vms::core::FormatIpv4Address;
using vms::test::Expect;
using vms::webrtc::CheckIceListenAddresses;
using vms::webrtc::IceListenAddress;
using vms::webrtc::ParseIceListenAddress;


namespace {


// 주소 목록을 점 표기 문자열 목록으로 바꾼다.
//
// addresses: 네트워크 바이트 순서의 주소 목록
// 반환값: 같은 순서의 점 표기 문자열 목록. 검사 결과를 기대값과 비교하는 데 쓴다.
std::vector<std::string> FormatAddresses(const std::vector<in_addr>& addresses) {
    std::vector<std::string> formatted;
    for (const in_addr& address : addresses) {
        formatted.push_back(FormatIpv4Address(address));
    }
    return formatted;
}


// 값을 해석하고 실패하면 테스트를 실패로 기록한다.
//
// text: `--ice-address` 값
// 반환값: 해석한 값. 실패하면 빈 값
IceListenAddress ParseOrFail(std::string_view text) {
    auto parsed = ParseIceListenAddress(text);
    Expect(parsed.has_value(), std::string("해석 성공: ") + std::string(text));
    return parsed ? *parsed : IceListenAddress{};
}


void TestBindAddressOnlyAnnouncesBindAddress() {
    const IceListenAddress parsed = ParseOrFail("192.168.0.10");
    Expect(FormatIpv4Address(parsed.bind_address) == "192.168.0.10", "bind address");
    Expect(FormatAddresses(parsed.announced_addresses) == std::vector<std::string>{"192.168.0.10"},
           "announced address는 bind address 하나");
}


void TestKeepsAnnouncedAddressOrder() {
    const IceListenAddress parsed = ParseOrFail("192.168.0.10=192.168.0.10,203.0.113.5");
    Expect(FormatIpv4Address(parsed.bind_address) == "192.168.0.10", "bind address");
    Expect(FormatAddresses(parsed.announced_addresses) ==
               std::vector<std::string>{"192.168.0.10", "203.0.113.5"},
           "announced address 두 개가 쓴 순서대로 들어감");
}


void TestDoesNotAddBindAddressToAnnouncedAddresses() {
    const IceListenAddress parsed = ParseOrFail("192.168.0.10=203.0.113.5");
    Expect(FormatIpv4Address(parsed.bind_address) == "192.168.0.10", "bind address");
    Expect(FormatAddresses(parsed.announced_addresses) == std::vector<std::string>{"203.0.113.5"},
           "bind address가 announced address에 들어가지 않음");
}


void TestRejectsMalformedValues() {
    for (const std::string_view text :
         {"192.168.0.10=", "192.168.0.10=,203.0.113.5", "192.168.0.10=203.0.113.5,",
          "192.168.0.10=bad", "bad=203.0.113.5", "=203.0.113.5", ""}) {
        Expect(!ParseIceListenAddress(text).has_value(), std::string("해석 실패: ") + std::string(text));
    }
}


void TestRejectsDuplicateBindAddress() {
    const std::vector<IceListenAddress> addresses = {
        ParseOrFail("127.0.0.1"),
        ParseOrFail("127.0.0.1=203.0.113.5"),
    };
    Expect(!CheckIceListenAddresses(addresses).has_value(), "bind address 중복 거부");
}


void TestRejectsDuplicateAnnouncedAddressAcrossValues() {
    const std::vector<IceListenAddress> addresses = {
        ParseOrFail("192.168.0.10=192.168.0.10,203.0.113.5"),
        ParseOrFail("127.0.0.1=203.0.113.5"),
    };
    Expect(!CheckIceListenAddresses(addresses).has_value(), "서로 다른 값에 걸친 announced address 중복 거부");
}


void TestRejectsDuplicateAnnouncedAddressInOneValue() {
    const std::vector<IceListenAddress> addresses = {ParseOrFail("127.0.0.1=203.0.113.5,203.0.113.5")};
    Expect(!CheckIceListenAddresses(addresses).has_value(), "한 값 안의 announced address 중복 거부");
}


void TestAcceptsDistinctAddresses() {
    const std::vector<IceListenAddress> addresses = {
        ParseOrFail("192.168.0.10=192.168.0.10,203.0.113.5"),
        ParseOrFail("127.0.0.1"),
    };
    Expect(CheckIceListenAddresses(addresses).has_value(), "중복 없는 목록 허용");
}


}  // namespace


int main() {
    TestBindAddressOnlyAnnouncesBindAddress();
    TestKeepsAnnouncedAddressOrder();
    TestDoesNotAddBindAddressToAnnouncedAddresses();
    TestRejectsMalformedValues();
    TestRejectsDuplicateBindAddress();
    TestRejectsDuplicateAnnouncedAddressAcrossValues();
    TestRejectsDuplicateAnnouncedAddressInOneValue();
    TestAcceptsDistinctAddresses();
    return vms::test::ExitCode();
}
