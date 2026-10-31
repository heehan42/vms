#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "media/h264_file.h"


namespace vms::test {


// 테스트가 복호화한 RTP 패킷에서 읽은 헤더 값과 페이로드.
struct ReceivedRtpPacket {
    // marker 비트
    bool marker = false;


    // payload type
    std::uint8_t payload_type = 0;


    // 순번
    std::uint16_t sequence_number = 0;


    // 타임스탬프
    std::uint32_t timestamp = 0;


    // SSRC
    std::uint32_t ssrc = 0;


    // 12바이트 고정 헤더 뒤의 페이로드
    std::vector<std::uint8_t> payload;
};


// 서버가 보내는 형식(CSRC·확장 없음)의 RTP 패킷을 읽는다.
//
// rtp_packet: 복호화한 RTP 패킷
// 반환값: 헤더 값과 페이로드. 12바이트보다 짧거나 버전이 2가 아니면 빈 optional
inline std::optional<ReceivedRtpPacket> ParseRtpPacket(std::span<const std::uint8_t> rtp_packet) {
    if (rtp_packet.size() < 12 || (rtp_packet[0] >> 6) != 2) {
        return std::nullopt;
    }
    const auto read32 = [&](std::size_t offset) {
        return (static_cast<std::uint32_t>(rtp_packet[offset]) << 24) |
               (static_cast<std::uint32_t>(rtp_packet[offset + 1]) << 16) |
               (static_cast<std::uint32_t>(rtp_packet[offset + 2]) << 8) | rtp_packet[offset + 3];
    };
    return ReceivedRtpPacket{
        .marker = (rtp_packet[1] & 0x80) != 0,
        .payload_type = static_cast<std::uint8_t>(rtp_packet[1] & 0x7F),
        .sequence_number = static_cast<std::uint16_t>((rtp_packet[2] << 8) | rtp_packet[3]),
        .timestamp = read32(4),
        .ssrc = read32(8),
        .payload = std::vector<std::uint8_t>(rtp_packet.begin() + 12, rtp_packet.end()),
    };
}


// 한 프레임의 RTP 페이로드(단일 NAL 패킷, FU-A)를 NAL 단위 목록으로 재조립한다.
//
// payloads: 한 프레임의 페이로드. 순번 순서여야 한다.
// 반환값: 재조립한 NAL 단위 목록. FU-A 조각의 S·E 순서가 틀리면 빈 optional
inline std::optional<std::vector<std::vector<std::uint8_t>>> ReassembleNalUnits(
    const std::vector<std::vector<std::uint8_t>>& payloads) {
    std::vector<std::vector<std::uint8_t>> nal_units;
    std::optional<std::vector<std::uint8_t>> fragmented_nal;
    for (const std::vector<std::uint8_t>& payload : payloads) {
        if (payload.empty()) {
            return std::nullopt;
        }
        if ((payload[0] & 0x1F) != 28) {
            if (fragmented_nal) {
                return std::nullopt;
            }
            nal_units.push_back(payload);
            continue;
        }

        if (payload.size() < 2) {
            return std::nullopt;
        }
        const bool is_start = (payload[1] & 0x80) != 0;
        const bool is_end = (payload[1] & 0x40) != 0;
        if (is_start == fragmented_nal.has_value()) {
            return std::nullopt;
        }
        if (is_start) {
            fragmented_nal =
                std::vector<std::uint8_t>{static_cast<std::uint8_t>((payload[0] & 0xE0) | (payload[1] & 0x1F))};
        }
        fragmented_nal->insert(fragmented_nal->end(), payload.begin() + 2, payload.end());
        if (is_end) {
            nal_units.push_back(*std::move(fragmented_nal));
            fragmented_nal.reset();
        }
    }
    if (fragmented_nal) {
        return std::nullopt;
    }
    return nal_units;
}


// 소스 프레임에서 서버가 보내는 NAL 단위 목록(AUD 제외)을 만든다.
//
// frame: 소스 프레임
// 반환값: AUD를 뺀 NAL 단위의 복사본
inline std::vector<std::vector<std::uint8_t>> TransmittedNalUnits(const media::H264Frame& frame) {
    std::vector<std::vector<std::uint8_t>> nal_units;
    for (const std::span<const std::uint8_t> nal_unit : frame.nal_units) {
        if (media::NalUnitTypeOf(nal_unit) != std::to_underlying(media::H264NalUnitType::kAccessUnitDelimiter)) {
            nal_units.emplace_back(nal_unit.begin(), nal_unit.end());
        }
    }
    return nal_units;
}


// 헤더 바이트 뒤에 본문을 붙인 NAL 단위를 4바이트 시작 코드와 함께 덧붙인다.
//
// bytes: 덧붙일 Annex-B 버퍼
// nal_header: NAL 헤더
// first_body_byte: 헤더 다음 바이트. 슬라이스이면 최상위 비트가 1이어야 픽처의 첫 슬라이스다.
// size: NAL 전체 크기(2 이상)
inline void AppendSyntheticNalUnit(std::vector<std::uint8_t>& bytes, std::uint8_t nal_header,
                                   std::uint8_t first_body_byte, std::size_t size) {
    bytes.insert(bytes.end(), {0x00, 0x00, 0x00, 0x01, nal_header, first_body_byte});
    for (std::size_t index = 2; index < size; ++index) {
        // 시작 코드가 생기지 않도록 0이 아닌 값만 쓴다.
        bytes.push_back(static_cast<std::uint8_t>(index % 251 + 1));
    }
}


// 프레임 3개짜리 합성 동영상을 만든다.
//
// 반환값: SPS·PPS·3000바이트 IDR(FU-A 3개), 100바이트 슬라이스, AUD·1500바이트 슬라이스(FU-A 2개) 프레임
inline media::H264File MakeSyntheticH264File() {
    std::vector<std::uint8_t> bytes;
    AppendSyntheticNalUnit(bytes, 0x67, 0x42, 8);
    AppendSyntheticNalUnit(bytes, 0x68, 0xCE, 4);
    AppendSyntheticNalUnit(bytes, 0x65, 0x88, 3000);
    AppendSyntheticNalUnit(bytes, 0x41, 0x9A, 100);
    AppendSyntheticNalUnit(bytes, 0x09, 0xF0, 2);
    AppendSyntheticNalUnit(bytes, 0x41, 0x9B, 1500);
    return *media::H264File::FromBytes(std::move(bytes));
}


}  // namespace vms::test
