#include "webrtc/h264_packetizer.h"

#include <cstdint>
#include <vector>

#include "test_support.h"

using vms::media::H264Frame;
using vms::test::Expect;
using vms::webrtc::PacketizeH264Frame;


namespace {


// 헤더 바이트 뒤에 정해진 패턴의 본문을 붙인 NAL 단위를 만든다.
//
// nal_header: NAL 헤더 바이트
// size: NAL 전체 크기(헤더 포함)
// 반환값: 만든 NAL 단위
std::vector<std::uint8_t> MakeNalUnit(std::uint8_t nal_header, std::size_t size) {
    std::vector<std::uint8_t> nal_unit(size);
    nal_unit[0] = nal_header;
    for (std::size_t index = 1; index < size; ++index) {
        nal_unit[index] = static_cast<std::uint8_t>(index * 7);
    }
    return nal_unit;
}


// 100바이트 NAL은 그대로 페이로드 하나가 된다.
void TestSmallNalUnit() {
    const std::vector<std::uint8_t> nal_unit = MakeNalUnit(0x41, 100);
    const auto payloads = PacketizeH264Frame(H264Frame{.nal_units = {nal_unit}});
    Expect(payloads.size() == 1 && payloads[0] == nal_unit, "작은 NAL은 단일 NAL 패킷이어야 한다");
}


// 1200바이트는 단일 NAL 패킷, 1201바이트는 FU-A 두 개다.
void TestSizeBoundary() {
    const std::vector<std::uint8_t> exact = MakeNalUnit(0x41, 1200);
    const auto exact_payloads = PacketizeH264Frame(H264Frame{.nal_units = {exact}});
    Expect(exact_payloads.size() == 1 && exact_payloads[0] == exact, "1200바이트 NAL은 단일 NAL 패킷");

    const std::vector<std::uint8_t> over = MakeNalUnit(0x41, 1201);
    const auto over_payloads = PacketizeH264Frame(H264Frame{.nal_units = {over}});
    Expect(over_payloads.size() == 2 && (over_payloads[0][0] & 0x1F) == 28, "1201바이트 NAL은 FU-A 두 개");
}


// 3000바이트 IDR NAL은 FU-A 세 조각이 되고, 재조립하면 원본과 같다.
void TestFuAFragments() {
    const std::vector<std::uint8_t> nal_unit = MakeNalUnit(0x65, 3000);
    const auto payloads = PacketizeH264Frame(H264Frame{.nal_units = {nal_unit}});
    if (payloads.size() != 3) {
        Expect(false, "3000바이트 NAL은 FU-A 3개여야 한다");
        return;
    }
    Expect(payloads[0][0] == 0x7C && payloads[1][0] == 0x7C && payloads[2][0] == 0x7C,
           "FU indicator는 F·NRI(0x60)와 유형 28이어야 한다");
    Expect(payloads[0][1] == 0x85 && payloads[1][1] == 0x05 && payloads[2][1] == 0x45,
           "FU header는 S·E 비트와 원래 유형 5여야 한다");
    Expect(payloads[0].size() == 1200 && payloads[1].size() == 1200 && payloads[2].size() == 605,
           "조각 본문은 1198, 1198, 603바이트여야 한다");

    std::vector<std::uint8_t> reassembled = {
        static_cast<std::uint8_t>((payloads[0][0] & 0xE0) | (payloads[0][1] & 0x1F))};
    for (const auto& payload : payloads) {
        reassembled.insert(reassembled.end(), payload.begin() + 2, payload.end());
    }
    Expect(reassembled == nal_unit, "조각을 이어 붙이고 NAL 헤더를 복원하면 원본과 같아야 한다");
}


// AUD는 결과에 넣지 않고, 나머지 NAL은 순서를 유지한다.
void TestSkipsAccessUnitDelimiter() {
    const std::vector<std::uint8_t> aud = {0x09, 0xF0};
    const std::vector<std::uint8_t> sps = MakeNalUnit(0x67, 10);
    const std::vector<std::uint8_t> slice = MakeNalUnit(0x65, 50);
    const auto payloads = PacketizeH264Frame(H264Frame{.nal_units = {aud, sps, slice}});
    Expect(payloads.size() == 2 && payloads[0] == sps && payloads[1] == slice, "AUD는 결과에 없어야 한다");
}


}  // namespace


int main() {
    TestSmallNalUnit();
    TestSizeBoundary();
    TestFuAFragments();
    TestSkipsAccessUnitDelimiter();
    return vms::test::ExitCode();
}
