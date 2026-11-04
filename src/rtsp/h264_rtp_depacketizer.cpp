#include "rtsp/h264_rtp_depacketizer.h"

#include <algorithm>
#include <utility>


namespace vms::rtsp {


namespace {


// RTP 고정 헤더 크기(바이트)
constexpr std::size_t kRtpFixedHeaderSize = 12;


// STAP-A 패킷의 NAL 유형 값
constexpr std::uint8_t kStapAType = 24;


// FU-A 패킷의 NAL 유형 값
constexpr std::uint8_t kFuAType = 28;


// 네트워크 바이트 순서의 16비트 값을 읽는다.
std::uint16_t Read16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
}


// 네트워크 바이트 순서의 32비트 값을 읽는다.
std::uint32_t Read32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) | bytes[offset + 3];
}


// 페이로드가 시작 조각이 아닌 FU-A인지 확인한다.
//
// payload: RTP 페이로드
// 반환값: FU-A이고 FU header의 S 비트가 꺼져 있으면 true
bool IsFragmentContinuation(std::span<const std::uint8_t> payload) {
    return payload.size() >= 2 && (payload[0] & 0x1F) == kFuAType && (payload[1] & 0x80) == 0;
}


}  // namespace


H264RtpDepacketizer::H264RtpDepacketizer(std::uint8_t payload_type, FrameHandler frame_handler,
                                         LossHandler loss_handler)
    : payload_type_(payload_type), frame_handler_(std::move(frame_handler)), loss_handler_(std::move(loss_handler)) {}


void H264RtpDepacketizer::HandlePacket(std::span<const std::uint8_t> packet) {
    if (packet.size() < kRtpFixedHeaderSize || (packet[0] >> 6) != 2 || (packet[1] & 0x7F) != payload_type_) {
        return;
    }
    const bool has_padding = (packet[0] & 0x20) != 0;
    const bool has_extension = (packet[0] & 0x10) != 0;
    const bool marker = (packet[1] & 0x80) != 0;
    const std::uint16_t sequence_number = Read16(packet, 2);
    const std::uint32_t timestamp = Read32(packet, 4);
    const std::uint32_t ssrc = Read32(packet, 8);

    // 서버는 CSRC·확장·패딩을 넣지 않지만, 카메라는 넣을 수 있으므로 건너뛰고 페이로드만 남긴다.
    std::size_t payload_begin = kRtpFixedHeaderSize + 4 * static_cast<std::size_t>(packet[0] & 0x0F);
    if (has_extension) {
        if (payload_begin + 4 > packet.size()) {
            return;
        }
        payload_begin += 4 + 4 * static_cast<std::size_t>(Read16(packet, payload_begin + 2));
    }
    std::size_t payload_end = packet.size();
    if (has_padding) {
        const std::size_t padding_size = packet.back();
        if (padding_size == 0 || padding_size > payload_end) {
            return;
        }
        payload_end -= padding_size;
    }
    if (payload_begin > payload_end) {
        return;
    }
    const std::span<const std::uint8_t> payload = packet.subspan(payload_begin, payload_end - payload_begin);

    // 카메라가 재시작하면 SSRC가 바뀌고 순번·타임스탬프 원점도 바뀌므로 새 스트림처럼 시작한다.
    if (ssrc_ && *ssrc_ != ssrc) {
        Reset();
        loss_handler_();
    }
    ssrc_ = ssrc;

    if (expected_sequence_number_ && sequence_number != *expected_sequence_number_) {
        // 빠진 패킷이 만들던 프레임의 것이면 남은 조각도 버린다. 타임스탬프가 바뀐 패킷은 새 프레임의 시작일 수 있어
        // 받아들이되, 시작 조각이 아닌 FU-A는 앞 조각이 빠진 것이므로 그 프레임을 버린다.
        const bool continues_dropped_frame = has_frame_data_ && timestamp == frame_timestamp_;
        ClearFrame();
        loss_handler_();
        if (continues_dropped_frame || IsFragmentContinuation(payload)) {
            discarding_timestamp_ = timestamp;
        }
    }
    expected_sequence_number_ = static_cast<std::uint16_t>(sequence_number + 1);

    // marker가 든 패킷이 빠져도 타임스탬프가 바뀐 것으로 앞 프레임이 끝났음을 안다.
    if (has_frame_data_ && timestamp != frame_timestamp_) {
        EmitFrame();
    }
    if (discarding_timestamp_ && timestamp != *discarding_timestamp_) {
        discarding_timestamp_.reset();
    }

    if (!discarding_timestamp_ && !payload.empty()) {
        if (!has_frame_data_) {
            frame_timestamp_ = timestamp;
            has_frame_data_ = true;
        }
        if (!AppendPayload(payload)) {
            DropFrameAsLoss();
        }
    }

    if (marker) {
        // 버리던 프레임이 끝났으므로 다음 패킷부터 새 프레임으로 받는다.
        if (discarding_timestamp_) {
            discarding_timestamp_.reset();
        } else {
            EmitFrame();
        }
    }
}


void H264RtpDepacketizer::Reset() {
    ClearFrame();
    ssrc_.reset();
    expected_sequence_number_.reset();
    discarding_timestamp_.reset();
}


void H264RtpDepacketizer::ClearFrame() {
    frame_buffer_.clear();
    frame_nal_ranges_.clear();
    has_frame_data_ = false;
    is_fragment_in_progress_ = false;
}


void H264RtpDepacketizer::DropFrameAsLoss() {
    discarding_timestamp_ = frame_timestamp_;
    ClearFrame();
    loss_handler_();
}


void H264RtpDepacketizer::EmitFrame() {
    if (!has_frame_data_) {
        return;
    }
    if (is_fragment_in_progress_) {
        // 끝 조각 없이 프레임이 끝났으면 NAL이 잘린 것이다. 이미 다음 프레임으로 넘어갔으므로 버리기를 이어 가지 않는다.
        DropFrameAsLoss();
        discarding_timestamp_.reset();
        return;
    }

    media::H264Frame frame;
    frame.nal_units.reserve(frame_nal_ranges_.size());
    for (const NalRange& range : frame_nal_ranges_) {
        frame.nal_units.emplace_back(frame_buffer_.data() + range.offset, range.size);
    }
    const bool has_slice = std::ranges::any_of(frame.nal_units, [](std::span<const std::uint8_t> nal_unit) {
        const std::uint8_t type = media::NalUnitTypeOf(nal_unit);
        return type == std::to_underlying(media::H264NalUnitType::kNonIdrSlice) ||
               type == std::to_underlying(media::H264NalUnitType::kIdrSlice);
    });
    if (has_slice) {
        frame_handler_(frame, frame_timestamp_, media::IsH264Keyframe(frame));
    }
    ClearFrame();
}


bool H264RtpDepacketizer::AppendPayload(std::span<const std::uint8_t> payload) {
    const std::uint8_t type = payload[0] & 0x1F;
    if (type >= 1 && type <= 23) {
        return !is_fragment_in_progress_ && AppendNalUnit(payload);
    }

    if (type == kStapAType) {
        // [STAP-A 헤더 1B][크기 2B][NAL]...
        std::size_t offset = 1;
        while (offset < payload.size()) {
            if (is_fragment_in_progress_ || offset + 2 > payload.size()) {
                return false;
            }
            const std::size_t nal_size = Read16(payload, offset);
            offset += 2;
            if (nal_size == 0 || offset + nal_size > payload.size() ||
                !AppendNalUnit(payload.subspan(offset, nal_size))) {
                return false;
            }
            offset += nal_size;
        }
        return true;
    }

    if (type == kFuAType) {
        // [FU indicator 1B][FU header 1B][조각]. 원래 NAL 헤더는 indicator의 F·NRI와 header의 유형으로 되살린다.
        if (payload.size() < 2) {
            return false;
        }
        const bool is_start = (payload[1] & 0x80) != 0;
        const bool is_end = (payload[1] & 0x40) != 0;
        if (is_start == is_fragment_in_progress_) {
            return false;
        }
        const std::span<const std::uint8_t> fragment = payload.subspan(2);
        if (is_start) {
            const std::uint8_t nal_header = static_cast<std::uint8_t>((payload[0] & 0xE0) | (payload[1] & 0x1F));
            if (!AppendNalUnit(std::span(&nal_header, 1))) {
                return false;
            }
            is_fragment_in_progress_ = true;
        }
        if (frame_buffer_.size() + fragment.size() > kMaxDepacketizedFrameSize) {
            return false;
        }
        frame_buffer_.insert(frame_buffer_.end(), fragment.begin(), fragment.end());
        frame_nal_ranges_.back().size += fragment.size();
        if (is_end) {
            is_fragment_in_progress_ = false;
        }
        return true;
    }

    // STAP-B, MTAP, FU-B 등은 packetization-mode 1에서 쓰지 않으므로 패킷만 버린다.
    return true;
}


bool H264RtpDepacketizer::AppendNalUnit(std::span<const std::uint8_t> nal_unit) {
    if (frame_buffer_.size() + nal_unit.size() > kMaxDepacketizedFrameSize) {
        return false;
    }
    frame_nal_ranges_.push_back(NalRange{.offset = frame_buffer_.size(), .size = nal_unit.size()});
    frame_buffer_.insert(frame_buffer_.end(), nal_unit.begin(), nal_unit.end());
    return true;
}


}  // namespace vms::rtsp
