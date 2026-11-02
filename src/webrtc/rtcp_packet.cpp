#include "webrtc/rtcp_packet.h"

#include <format>


namespace vms::webrtc {


namespace {


// RTCP 공통 헤더 크기(바이트)
constexpr std::size_t kRtcpHeaderSize = 4;


// 보고 블록 하나의 크기(바이트)
constexpr std::size_t kReportBlockSize = 24;


// SR에서 보낸 쪽 SSRC 뒤, 보고 블록 앞에 오는 송신 정보 크기(바이트)
constexpr std::size_t kSenderInfoSize = 20;


// SDES 항목 종류 중 CNAME
constexpr std::uint8_t kSdesCnameItemType = 1;


// 빅 엔디언 32비트 값을 읽는다.
//
// bytes: 4바이트 이상인 데이터
// 반환값: 앞 4바이트의 값
std::uint32_t ReadUint32(std::span<const std::uint8_t> bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
}


// 24바이트 보고 블록 하나를 읽는다.
//
// bytes: 보고 블록으로 시작하는 24바이트 이상인 데이터
// 반환값: 해석한 보고 블록
RtcpReportBlock ParseReportBlock(std::span<const std::uint8_t> bytes) {
    // 누적 손실은 부호 있는 24비트이므로, 최상위 비트가 1이면 2^24를 빼 음수로 만든다.
    const std::int32_t cumulative_lost_bits = (bytes[5] << 16) | (bytes[6] << 8) | bytes[7];
    return RtcpReportBlock{
        .source_ssrc = ReadUint32(bytes),
        .fraction_lost = bytes[4],
        .cumulative_lost =
            (cumulative_lost_bits & 0x800000) != 0 ? cumulative_lost_bits - 0x1000000 : cumulative_lost_bits,
        .extended_highest_sequence_number = ReadUint32(bytes.subspan(8)),
        .jitter = ReadUint32(bytes.subspan(12)),
        .last_sender_report = ReadUint32(bytes.subspan(16)),
        .delay_since_last_sender_report = ReadUint32(bytes.subspan(20)),
    };
}


// SR·RR 본문에서 보고 블록을 읽는다.
//
// blocks: 첫 보고 블록부터 본문 끝까지의 데이터
// count: 헤더의 보고 블록 수
// 반환값: 보고 블록 목록. 데이터가 count개보다 짧으면 오류 메시지
std::expected<std::vector<RtcpReportBlock>, std::string> ParseReportBlocks(std::span<const std::uint8_t> blocks,
                                                                           std::uint8_t count) {
    if (blocks.size() < kReportBlockSize * count) {
        return std::unexpected(
            std::format("보고 블록 {}개({}바이트)가 남은 본문 {}바이트보다 김", count, kReportBlockSize * count,
                        blocks.size()));
    }
    std::vector<RtcpReportBlock> report_blocks;
    report_blocks.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        report_blocks.push_back(ParseReportBlock(blocks.subspan(kReportBlockSize * index)));
    }
    return report_blocks;
}


// SDES 첫 청크의 항목에서 CNAME을 찾는다.
//
// items: 첫 청크의 SSRC 뒤부터 본문 끝까지의 데이터
// 반환값: CNAME 원본 바이트. 종료 항목(종류 0)이나 본문 끝까지 CNAME이 없으면 빈 optional.
//   항목 길이가 본문을 넘으면 오류 메시지
std::expected<std::optional<std::string>, std::string> FindSdesCname(std::span<const std::uint8_t> items) {
    std::size_t offset = 0;
    while (offset < items.size() && items[offset] != 0) {
        if (offset + 2 > items.size()) {
            return std::unexpected("SDES 항목 길이 필드가 잘림");
        }
        const std::uint8_t item_type = items[offset];
        const std::size_t item_size = items[offset + 1];
        if (offset + 2 + item_size > items.size()) {
            return std::unexpected(std::format("SDES 항목 길이 {}바이트가 남은 본문보다 김", item_size));
        }
        if (item_type == kSdesCnameItemType) {
            const auto content = items.subspan(offset + 2, item_size);
            return std::string(content.begin(), content.end());
        }
        offset += 2 + item_size;
    }
    return std::nullopt;
}


// 헤더 뒤 본문을 종류별로 해석해 packet에 채운다.
//
// packet: 종류 번호와 count를 채운 패킷. 보낸 쪽 SSRC, 보고 블록, CNAME이 채워진다.
// body: 헤더 뒤부터 padding 앞까지의 본문
// 반환값: 성공하면 빈 값, 본문이 종류의 형식보다 짧으면 오류 메시지
std::expected<void, std::string> ParseBody(RtcpPacket& packet, std::span<const std::uint8_t> body) {
    if (body.size() >= 4) {
        packet.sender_ssrc = ReadUint32(body);
    }

    switch (packet.packet_type) {
        case kRtcpSenderReportType: {
            if (body.size() < 4 + kSenderInfoSize) {
                return std::unexpected(std::format("SR 본문이 송신 정보보다 짧음: {}바이트", body.size()));
            }
            auto report_blocks = ParseReportBlocks(body.subspan(4 + kSenderInfoSize), packet.count);
            if (!report_blocks) {
                return std::unexpected(report_blocks.error());
            }
            packet.report_blocks = *std::move(report_blocks);
            return {};
        }
        case kRtcpReceiverReportType: {
            if (body.size() < 4) {
                return std::unexpected(std::format("RR 본문이 SSRC보다 짧음: {}바이트", body.size()));
            }
            auto report_blocks = ParseReportBlocks(body.subspan(4), packet.count);
            if (!report_blocks) {
                return std::unexpected(report_blocks.error());
            }
            packet.report_blocks = *std::move(report_blocks);
            return {};
        }
        case kRtcpSourceDescriptionType: {
            if (packet.count == 0) {
                return {};
            }
            if (body.size() < 4) {
                return std::unexpected(std::format("SDES 본문이 청크 SSRC보다 짧음: {}바이트", body.size()));
            }
            auto cname = FindSdesCname(body.subspan(4));
            if (!cname) {
                return std::unexpected(cname.error());
            }
            packet.cname = *std::move(cname);
            return {};
        }
        default:
            return {};
    }
}


}  // namespace


std::expected<std::vector<RtcpPacket>, std::string> ParseRtcpCompoundPacket(std::span<const std::uint8_t> compound) {
    std::vector<RtcpPacket> packets;
    std::size_t offset = 0;
    while (offset < compound.size()) {
        const std::span<const std::uint8_t> remaining = compound.subspan(offset);
        if (remaining.size() < kRtcpHeaderSize) {
            return std::unexpected(std::format("{}번째 패킷 위치에 헤더보다 짧은 {}바이트가 남음", packets.size() + 1,
                                               remaining.size()));
        }
        const std::uint8_t version = remaining[0] >> 6;
        if (version != 2) {
            return std::unexpected(std::format("{}번째 패킷의 버전이 2가 아님: {}", packets.size() + 1, version));
        }

        // length는 32비트 단어 수에서 1을 뺀 값이다.
        const std::size_t packet_size = (static_cast<std::size_t>((remaining[2] << 8) | remaining[3]) + 1) * 4;
        if (packet_size > remaining.size()) {
            return std::unexpected(std::format("{}번째 패킷 길이 {}바이트가 남은 데이터 {}바이트보다 김",
                                               packets.size() + 1, packet_size, remaining.size()));
        }
        const std::span<const std::uint8_t> packet_bytes = remaining.first(packet_size);

        std::span<const std::uint8_t> body = packet_bytes.subspan(kRtcpHeaderSize);
        const bool has_padding = (remaining[0] & 0x20) != 0;
        if (has_padding) {
            const std::size_t padding_size = packet_bytes.back();
            if (padding_size == 0 || padding_size > body.size()) {
                return std::unexpected(std::format("{}번째 패킷의 padding 길이 {}가 본문 {}바이트에 맞지 않음",
                                                   packets.size() + 1, padding_size, body.size()));
            }
            body = body.first(body.size() - padding_size);
        }

        RtcpPacket packet;
        packet.packet_type = remaining[1];
        packet.count = static_cast<std::uint8_t>(remaining[0] & 0x1F);
        packet.size = packet_size;
        if (auto parsed = ParseBody(packet, body); !parsed) {
            return std::unexpected(std::format("{}번째 패킷(PT {}): {}", packets.size() + 1, packet.packet_type,
                                               parsed.error()));
        }
        packets.push_back(std::move(packet));
        offset += packet_size;
    }
    return packets;
}


}  // namespace vms::webrtc
