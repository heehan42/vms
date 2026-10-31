#include "webrtc/rtp_packet.h"

#include <array>
#include <cstdint>
#include <vector>

#include "test_support.h"

using vms::test::Expect;
using vms::webrtc::BuildRtpPacket;
using vms::webrtc::RtpHeader;


namespace {


// V=2, M=1, PT=102인 헤더의 바이트 배치와 페이로드 위치를 검사한다.
void TestHeaderLayout() {
    const std::array<std::uint8_t, 3> payload = {0xAA, 0xBB, 0xCC};
    const std::vector<std::uint8_t> packet = BuildRtpPacket(RtpHeader{.marker = true,
                                                                      .payload_type = 102,
                                                                      .sequence_number = 0x1234,
                                                                      .timestamp = 0x89ABCDEF,
                                                                      .ssrc = 0x01020304},
                                                            payload);
    const std::vector<std::uint8_t> expected = {0x80, 0xE6, 0x12, 0x34, 0x89, 0xAB, 0xCD, 0xEF,
                                                0x01, 0x02, 0x03, 0x04, 0xAA, 0xBB, 0xCC};
    Expect(packet == expected, "헤더 필드는 네트워크 바이트 순서이고 페이로드가 뒤에 붙어야 한다");
}


// marker가 꺼져 있으면 두 번째 바이트는 payload type만 담는다.
void TestMarkerCleared() {
    const std::vector<std::uint8_t> packet =
        BuildRtpPacket(RtpHeader{.marker = false, .payload_type = 102}, std::span<const std::uint8_t>());
    Expect(packet.size() == 12 && packet[0] == 0x80 && packet[1] == 102, "marker 없는 두 번째 바이트는 0x66");
}


}  // namespace


int main() {
    TestHeaderLayout();
    TestMarkerCleared();
    return vms::test::ExitCode();
}
