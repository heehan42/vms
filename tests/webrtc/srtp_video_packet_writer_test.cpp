#include "webrtc/srtp_video_packet_writer.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "rtp_test_support.h"
#include "srtp_test_receiver.h"
#include "test_support.h"

using vms::media::H264File;
using vms::test::Expect;
using vms::test::MakeSyntheticH264File;
using vms::test::ParseRtpPacket;
using vms::test::ReassembleNalUnits;
using vms::test::ReceivedRtpPacket;
using vms::test::SrtpTestReceiver;
using vms::test::TransmittedNalUnits;
using vms::webrtc::SrtpVideoPacketWriter;
using vms::webrtc::SrtpVideoPacketWriterConfig;


namespace {


// 송신 측과 수신 측이 함께 쓰는 마스터 키
constexpr std::array<std::uint8_t, 16> kMasterKey = {0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
                                                     0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F};


// 송신 측과 수신 측이 함께 쓰는 마스터 솔트
constexpr std::array<std::uint8_t, 14> kMasterSalt = {0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
                                                      0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D};


// 테스트 설정의 SSRC
constexpr std::uint32_t kSsrc = 0x0BADCAFE;


// 테스트 설정의 payload type
constexpr std::uint8_t kPayloadType = 96;


// 순번 초기값을 정해 송신 공통부를 만든다.
//
// initial_sequence_number: 첫 패킷의 순번
// 반환값: 만든 객체
SrtpVideoPacketWriter CreateWriter(std::uint16_t initial_sequence_number) {
    return *SrtpVideoPacketWriter::Create(SrtpVideoPacketWriterConfig{.ssrc = kSsrc,
                                                                       .payload_type = kPayloadType,
                                                                       .initial_sequence_number = initial_sequence_number},
                                          kMasterKey, kMasterSalt);
}


// SRTP 패킷 목록을 복호화해 헤더와 페이로드를 읽는다.
//
// packets: WriteFrame의 결과
// receiver: 순번 순환을 따라가는 테스트 수신 측
// 반환값: 순서대로의 RTP 패킷. 복호화나 해석이 하나라도 실패하면 빈 optional
std::optional<std::vector<ReceivedRtpPacket>> ReceivePackets(const std::vector<std::vector<std::uint8_t>>& packets,
                                                             SrtpTestReceiver& receiver) {
    std::vector<ReceivedRtpPacket> received;
    for (const auto& srtp_packet : packets) {
        const auto rtp_packet = receiver.Unprotect(srtp_packet);
        auto parsed = rtp_packet ? ParseRtpPacket(*rtp_packet) : std::nullopt;
        if (!parsed) {
            return std::nullopt;
        }
        received.push_back(*std::move(parsed));
    }
    return received;
}


// 프레임을 넘어 순번이 이어지고 65535 다음이 0이며, 넘긴 타임스탬프가 그대로 쓰이고, 마지막 패킷에만
// marker가 있는지 검사한다.
void TestHeadersAcrossFrames() {
    const H264File video = MakeSyntheticH264File();
    SrtpVideoPacketWriter writer = CreateWriter(65533);
    auto receiver = *SrtpTestReceiver::Create(kMasterKey, kMasterSalt);

    // 타임스탬프는 일정하지 않은 값을 넘겨, 쓰는 쪽이 계산하지 않고 그대로 쓰는지 본다.
    constexpr std::array<std::uint32_t, 3> kTimestamps = {0xFFFFFFF0, 7, 123456};
    std::vector<std::uint16_t> sequence_numbers;
    for (std::size_t index = 0; index < kTimestamps.size(); ++index) {
        const auto packets = writer.WriteFrame(video.Frames()[index], kTimestamps[index]);
        const auto received = packets ? ReceivePackets(*packets, receiver) : std::nullopt;
        if (!received || received->empty()) {
            Expect(false, "프레임마다 복호화할 수 있는 패킷이 있어야 한다");
            return;
        }

        std::vector<std::vector<std::uint8_t>> payloads;
        for (std::size_t packet_index = 0; packet_index < received->size(); ++packet_index) {
            const ReceivedRtpPacket& packet = (*received)[packet_index];
            Expect(packet.marker == (packet_index + 1 == received->size()), "마지막 패킷에만 marker");
            Expect(packet.timestamp == kTimestamps[index], "넘긴 타임스탬프가 그대로 쓰임");
            Expect(packet.ssrc == kSsrc && packet.payload_type == kPayloadType, "SSRC와 payload type은 설정 값");
            sequence_numbers.push_back(packet.sequence_number);
            payloads.push_back(packet.payload);
        }
        Expect(ReassembleNalUnits(payloads) == TransmittedNalUnits(video.Frames()[index]),
               "재조립한 NAL은 원래 프레임(AUD 제외)과 같음");
    }

    Expect(sequence_numbers.size() > 3 && sequence_numbers[0] == 65533, "첫 순번은 초기값");
    for (std::size_t index = 1; index < sequence_numbers.size(); ++index) {
        Expect(sequence_numbers[index] == static_cast<std::uint16_t>(sequence_numbers[index - 1] + 1),
               "순번은 프레임을 넘어 1씩 늘고 65535 다음은 0");
    }
}


// 순번 초기값을 정하지 않으면 무작위로 정해도 만들 수 있는지 검사한다.
void TestRandomInitialSequenceNumber() {
    const auto writer = SrtpVideoPacketWriter::Create(
        SrtpVideoPacketWriterConfig{.ssrc = kSsrc, .payload_type = kPayloadType, .initial_sequence_number = {}}, kMasterKey, kMasterSalt);
    Expect(writer.has_value(), "순번 초기값 없이 생성");
    Expect(vms::webrtc::GenerateInitialRtpTimestamp().has_value(), "타임스탬프 초기값 난수 생성");
}


}  // namespace


int main() {
    TestHeadersAcrossFrames();
    TestRandomInitialSequenceNumber();
    return vms::test::ExitCode();
}
