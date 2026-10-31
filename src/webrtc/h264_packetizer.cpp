#include "webrtc/h264_packetizer.h"

#include <algorithm>
#include <span>
#include <utility>


namespace vms::webrtc {


namespace {


// FU-A 패킷의 NAL 유형 값(RFC 6184)
constexpr std::uint8_t kFuANalUnitType = 28;


// FU-A 앞에 붙는 FU indicator와 FU header의 크기(바이트)
constexpr std::size_t kFuAHeaderSize = 2;


// FU header의 시작(S) 비트
constexpr std::uint8_t kFuStartBit = 0x80;


// FU header의 끝(E) 비트
constexpr std::uint8_t kFuEndBit = 0x40;


// 큰 NAL 하나를 FU-A 조각 페이로드로 나눠 덧붙인다.
//
// nal_unit: max_payload_size보다 큰 NAL 단위
// max_payload_size: 조각 페이로드의 최대 크기
// payloads: 조각을 덧붙일 목록
void AppendFuAFragments(std::span<const std::uint8_t> nal_unit, std::size_t max_payload_size,
                        std::vector<std::vector<std::uint8_t>>& payloads) {
    // NAL 헤더는 조각마다 indicator와 header로 다시 표현하므로 본문에서 뺀다.
    const std::uint8_t nal_header = nal_unit[0];
    const auto fu_indicator = static_cast<std::uint8_t>((nal_header & 0xE0) | kFuANalUnitType);
    const auto original_type = static_cast<std::uint8_t>(nal_header & 0x1F);
    std::span<const std::uint8_t> remaining = nal_unit.subspan(1);
    const std::size_t max_fragment_size = max_payload_size - kFuAHeaderSize;

    bool is_first = true;
    while (!remaining.empty()) {
        const std::size_t fragment_size = std::min(remaining.size(), max_fragment_size);
        const bool is_last = fragment_size == remaining.size();
        std::uint8_t fu_header = original_type;
        if (is_first) {
            fu_header |= kFuStartBit;
        }
        if (is_last) {
            fu_header |= kFuEndBit;
        }

        std::vector<std::uint8_t> payload;
        payload.reserve(kFuAHeaderSize + fragment_size);
        payload.push_back(fu_indicator);
        payload.push_back(fu_header);
        payload.insert(payload.end(), remaining.begin(), remaining.begin() + fragment_size);
        payloads.push_back(std::move(payload));

        remaining = remaining.subspan(fragment_size);
        is_first = false;
    }
}


}  // namespace


std::vector<std::vector<std::uint8_t>> PacketizeH264Frame(const media::H264Frame& frame,
                                                         std::size_t max_payload_size) {
    std::vector<std::vector<std::uint8_t>> payloads;
    for (const std::span<const std::uint8_t> nal_unit : frame.nal_units) {
        if (media::NalUnitTypeOf(nal_unit) ==
            std::to_underlying(media::H264NalUnitType::kAccessUnitDelimiter)) {
            continue;
        }
        if (nal_unit.size() <= max_payload_size) {
            payloads.emplace_back(nal_unit.begin(), nal_unit.end());
            continue;
        }
        AppendFuAFragments(nal_unit, max_payload_size, payloads);
    }
    return payloads;
}


}  // namespace vms::webrtc
