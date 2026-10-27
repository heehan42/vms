#include "core/network_interfaces.h"

#include <arpa/inet.h>

#include <algorithm>

#include "test_support.h"

using vms::core::FormatIpv4Address;
using vms::core::FormatIpv4Endpoint;
using vms::core::ListNonLoopbackIpv4Addresses;
using vms::core::ParseIpv4Address;
using vms::test::Expect;


namespace {


// 조회가 성공하고 결과에 루프백 주소(127.0.0.0/8)가 없는지 검사한다.
void TestListsWithoutLoopback() {
    const auto addresses = ListNonLoopbackIpv4Addresses();
    if (!addresses) {
        Expect(false, "인터페이스 주소 조회가 성공해야 한다");
        return;
    }
    const bool has_loopback = std::ranges::any_of(*addresses, [](const in_addr& address) {
        return (ntohl(address.s_addr) >> 24) == 127;
    });
    Expect(!has_loopback, "결과에 루프백 주소가 없어야 한다");
}


// 점 표기 문자열과 주소 사이의 변환이 서로 맞는지 검사한다.
void TestParsesAndFormatsAddress() {
    const auto address = ParseIpv4Address("192.168.0.10");
    Expect(address && FormatIpv4Address(*address) == "192.168.0.10", "주소 변환 왕복");

    Expect(!ParseIpv4Address("192.168.0"), "자리 수가 모자란 주소는 거부해야 한다");
    Expect(!ParseIpv4Address("256.0.0.1"), "범위를 넘는 주소는 거부해야 한다");
    Expect(!ParseIpv4Address("localhost"), "이름은 거부해야 한다");
}


// 주소와 포트를 로그용 문자열로 만드는지 검사한다.
void TestFormatsEndpoint() {
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr = *ParseIpv4Address("127.0.0.1");
    endpoint.sin_port = htons(50000);
    Expect(FormatIpv4Endpoint(endpoint) == "127.0.0.1:50000", "주소:포트 형식");
}


}  // namespace


int main() {
    TestListsWithoutLoopback();
    TestParsesAndFormatsAddress();
    TestFormatsEndpoint();
    return vms::test::ExitCode();
}
