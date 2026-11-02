#include "webrtc/rtcp_packet.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "test_support.h"

using vms::test::Expect;
using vms::webrtc::ParseRtcpCompoundPacket;
using vms::webrtc::RtcpPacket;
using vms::webrtc::RtcpReportBlock;


namespace {


// background "RR과 보고 블록"의 예시 RR. 보낸 쪽 SSRC 1, 대상 SSRC 0x12345678
const std::vector<std::uint8_t> kReceiverReport = {
    0x81, 0xC9, 0x00, 0x07, 0x00, 0x00, 0x00, 0x01, 0x12, 0x34, 0x56, 0x78, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x11, 0xEB, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};


// background "SDES와 CNAME"의 예시 SDES. 청크 SSRC 1, CNAME "abcdefghijklmnop"
const std::vector<std::uint8_t> kSourceDescription = {
    0x81, 0xCA, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01, 0x01, 0x10, 'a', 'b', 'c', 'd',
    'e',  'f',  'g',  'h',  'i',  'j',  'k',  'l',  'm',  'n',  'o', 'p', 0x00, 0x00,
};


// 여러 패킷 바이트를 이어 붙여 복합 패킷을 만든다.
//
// packets: 이어 붙일 패킷 바이트
// 반환값: 순서대로 이어 붙인 데이터
std::vector<std::uint8_t> Concatenate(std::initializer_list<std::vector<std::uint8_t>> packets) {
    std::vector<std::uint8_t> compound;
    for (const auto& packet : packets) {
        compound.insert(compound.end(), packet.begin(), packet.end());
    }
    return compound;
}


// 예시 RR의 보고 블록 값과 같은지 확인한다.
//
// block: 해석한 보고 블록
// 반환값: 일곱 값이 모두 예시와 같으면 true
bool IsExampleReportBlock(const RtcpReportBlock& block) {
    return block.source_ssrc == 0x12345678 && block.fraction_lost == 0 && block.cumulative_lost == 0 &&
           block.extended_highest_sequence_number == 70123 && block.jitter == 12 && block.last_sender_report == 0 &&
           block.delay_since_last_sender_report == 0;
}


// background의 RR 예시를 해석한 값이 문서와 같다.
void TestParsesReceiverReport() {
    const auto packets = ParseRtcpCompoundPacket(kReceiverReport);
    if (!packets || packets->size() != 1) {
        Expect(false, "RR 하나로 해석되어야 한다");
        return;
    }
    const RtcpPacket& packet = packets->front();
    Expect(packet.packet_type == 201 && packet.count == 1 && packet.size == 32, "RR 종류·count·길이");
    Expect(packet.sender_ssrc == 1u, "RR 보낸 쪽 SSRC");
    Expect(packet.report_blocks.size() == 1 && IsExampleReportBlock(packet.report_blocks.front()),
           "보고 블록의 다섯 값과 LSR·DLSR");
}


// RR+SDES 복합 패킷은 두 패킷으로 나뉘고 SDES의 CNAME을 읽는다.
void TestParsesReceiverReportWithCname() {
    const auto packets = ParseRtcpCompoundPacket(Concatenate({kReceiverReport, kSourceDescription}));
    if (!packets || packets->size() != 2) {
        Expect(false, "RR+SDES는 두 패킷으로 나뉘어야 한다");
        return;
    }
    Expect(IsExampleReportBlock((*packets)[0].report_blocks.front()), "첫 패킷은 RR");
    const RtcpPacket& sdes = (*packets)[1];
    Expect(sdes.packet_type == 202 && sdes.size == 28 && sdes.sender_ssrc == 1u, "SDES 종류·길이·청크 SSRC");
    Expect(sdes.cname == "abcdefghijklmnop", "SDES의 CNAME");
    Expect(sdes.report_blocks.empty(), "SDES에는 보고 블록이 없음");
}


// SR은 송신 정보 20바이트 뒤의 보고 블록을 읽고, PT 206은 종류와 길이만 기록한다.
void TestParsesSenderReportAndFeedback() {
    std::vector<std::uint8_t> sender_report = {0x81, 0xC8, 0x00, 0x0C, 0x12, 0x34, 0x56, 0x78,
                                               0xEE, 0x92, 0xDD, 0xA0, 0x80, 0x00, 0x00, 0x00,
                                               0x00, 0x0F, 0x42, 0x40, 0x00, 0x00, 0x03, 0xE8,
                                               0x00, 0x0F, 0x42, 0x40};
    sender_report.insert(sender_report.end(), kReceiverReport.begin() + 8, kReceiverReport.end());
    // PLI(FMT 1): 헤더, 보낸 쪽 SSRC, 대상 SSRC로 12바이트
    const std::vector<std::uint8_t> picture_loss = {0x81, 0xCE, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
                                                    0x12, 0x34, 0x56, 0x78};

    const auto packets = ParseRtcpCompoundPacket(Concatenate({sender_report, picture_loss}));
    if (!packets || packets->size() != 2) {
        Expect(false, "SR+PSFB는 두 패킷으로 나뉘어야 한다");
        return;
    }
    const RtcpPacket& sr = (*packets)[0];
    Expect(sr.packet_type == 200 && sr.size == 52 && sr.sender_ssrc == 0x12345678u, "SR 종류·길이·보낸 쪽 SSRC");
    Expect(sr.report_blocks.size() == 1 && IsExampleReportBlock(sr.report_blocks.front()), "SR의 보고 블록");
    const RtcpPacket& feedback = (*packets)[1];
    Expect(feedback.packet_type == 206 && feedback.count == 1 && feedback.size == 12, "PT 206의 종류·FMT·길이");
    Expect(feedback.report_blocks.empty() && !feedback.cname, "PT 206은 내용을 해석하지 않음");
}


// 형식 오류: 남은 길이를 넘는 length, 버전 1, 헤더보다 짧게 남은 데이터, 맞지 않는 padding 길이
void TestRejectsMalformedCompound() {
    std::vector<std::uint8_t> overlong_sdes = kSourceDescription;
    overlong_sdes[3] = 0x07;
    Expect(!ParseRtcpCompoundPacket(Concatenate({kReceiverReport, overlong_sdes})),
           "두 번째 패킷 length가 남은 데이터보다 길면 형식 오류");

    std::vector<std::uint8_t> version_one = kReceiverReport;
    version_one[0] = 0x41;
    Expect(!ParseRtcpCompoundPacket(version_one), "버전 1이면 형식 오류");

    Expect(!ParseRtcpCompoundPacket(Concatenate({kReceiverReport, {0x81, 0xCA}})),
           "4바이트 미만 남은 데이터는 형식 오류");

    // P 비트를 켠 PSFB(12바이트). 마지막 바이트가 padding 길이다.
    std::vector<std::uint8_t> padded = {0xA1, 0xCE, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04};
    Expect(ParseRtcpCompoundPacket(padded).has_value(), "padding 길이 4는 받아들임");
    padded.back() = 0x00;
    Expect(!ParseRtcpCompoundPacket(padded), "padding 길이 0은 형식 오류");
    padded.back() = 0x09;
    Expect(!ParseRtcpCompoundPacket(padded), "padding 길이가 본문 8바이트보다 길면 형식 오류");

    std::vector<std::uint8_t> too_many_blocks = kReceiverReport;
    too_many_blocks[0] = 0x82;
    Expect(!ParseRtcpCompoundPacket(too_many_blocks), "보고 블록 수가 본문보다 많으면 형식 오류");
}


// 누적 손실 FF FF FF는 -1이다.
void TestNegativeCumulativeLost() {
    std::vector<std::uint8_t> report = kReceiverReport;
    report[13] = 0xFF;
    report[14] = 0xFF;
    report[15] = 0xFF;
    const auto packets = ParseRtcpCompoundPacket(report);
    Expect(packets && packets->front().report_blocks.front().cumulative_lost == -1, "누적 손실 FF FF FF는 -1");
}


}  // namespace


int main() {
    TestParsesReceiverReport();
    TestParsesReceiverReportWithCname();
    TestParsesSenderReportAndFeedback();
    TestRejectsMalformedCompound();
    TestNegativeCumulativeLost();
    return vms::test::ExitCode();
}
