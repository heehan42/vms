#include "media/h264_sps.h"

#include <utility>
#include <vector>

#include "media/h264_file.h"


namespace vms::media {


namespace {


// RBSP 비트열을 앞에서부터 읽는 상태. 끝을 넘어 읽으면 실패 상태가 되고 이후 값은 모두 0이다.
class BitReader {
public:
    // bytes: 에뮬레이션 방지 바이트를 뺀 RBSP. 이 객체보다 오래 살아 있어야 한다.
    explicit BitReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}


    // 비트 하나를 읽는다.
    std::uint32_t ReadBit() {
        if (bit_position_ >= bytes_.size() * 8) {
            has_overrun_ = true;
            return 0;
        }
        const std::uint32_t bit = (bytes_[bit_position_ / 8] >> (7 - bit_position_ % 8)) & 1;
        ++bit_position_;
        return bit;
    }


    // count비트(32 이하)를 부호 없는 정수로 읽는다.
    std::uint32_t ReadBits(int count) {
        std::uint32_t value = 0;
        for (int index = 0; index < count; ++index) {
            value = (value << 1) | ReadBit();
        }
        return value;
    }


    // ue(v) 지수 골롬 부호를 읽는다. 앞의 0이 31개를 넘으면 실패 상태가 된다.
    std::uint32_t ReadUnsignedExpGolomb() {
        int leading_zero_count = 0;
        while (ReadBit() == 0) {
            if (has_overrun_ || ++leading_zero_count > 31) {
                has_overrun_ = true;
                return 0;
            }
        }
        return (std::uint32_t{1} << leading_zero_count) - 1 + ReadBits(leading_zero_count);
    }


    // se(v) 부호 있는 지수 골롬 부호를 읽는다.
    std::int32_t ReadSignedExpGolomb() {
        const std::uint32_t code = ReadUnsignedExpGolomb();
        const auto magnitude = static_cast<std::int32_t>((code + 1) / 2);
        return code % 2 == 1 ? magnitude : -magnitude;
    }


    // 끝을 넘어 읽었거나 부호가 잘못되었는지 여부
    bool HasOverrun() const { return has_overrun_; }


private:
    // 읽을 RBSP
    std::span<const std::uint8_t> bytes_;


    // 다음에 읽을 비트 위치
    std::size_t bit_position_ = 0;


    // 끝을 넘어 읽었는지 여부
    bool has_overrun_ = false;
};


// NAL 본문에서 에뮬레이션 방지 바이트(00 00 03의 03)를 뺀다.
//
// nal_body: NAL 헤더 뒤의 바이트
// 반환값: RBSP
std::vector<std::uint8_t> RemoveEmulationPrevention(std::span<const std::uint8_t> nal_body) {
    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(nal_body.size());
    int zero_count = 0;
    for (const std::uint8_t byte : nal_body) {
        if (zero_count >= 2 && byte == 0x03) {
            zero_count = 0;
            continue;
        }
        zero_count = byte == 0 ? zero_count + 1 : 0;
        rbsp.push_back(byte);
    }
    return rbsp;
}


// High 계열처럼 SPS에 chroma_format_idc 등이 들어가는 profile_idc인지 확인한다.
bool HasChromaFormatFields(std::uint32_t profile_idc) {
    switch (profile_idc) {
        case 100:
        case 110:
        case 122:
        case 244:
        case 44:
        case 83:
        case 86:
        case 118:
        case 128:
        case 138:
        case 139:
        case 134:
        case 135:
            return true;
        default:
            return false;
    }
}


// scaling_list() 하나를 읽고 버린다.
//
// reader: SPS 읽기 상태
// size: 목록 크기(16 또는 64)
void SkipScalingList(BitReader& reader, int size) {
    std::int32_t last_scale = 8;
    std::int32_t next_scale = 8;
    for (int index = 0; index < size && next_scale != 0; ++index) {
        next_scale = (last_scale + reader.ReadSignedExpGolomb() + 256) % 256;
        last_scale = next_scale == 0 ? last_scale : next_scale;
    }
}


}  // namespace


std::optional<H264Resolution> ParseH264SpsResolution(std::span<const std::uint8_t> sps) {
    if (sps.size() < 4 || NalUnitTypeOf(sps) != std::to_underlying(H264NalUnitType::kSps)) {
        return std::nullopt;
    }
    const std::vector<std::uint8_t> rbsp = RemoveEmulationPrevention(sps.subspan(1));
    BitReader reader(rbsp);

    const std::uint32_t profile_idc = reader.ReadBits(8);
    reader.ReadBits(16);  // constraint_set 플래그와 level_idc
    reader.ReadUnsignedExpGolomb();  // seq_parameter_set_id

    std::uint32_t chroma_format_idc = 1;
    bool has_separate_colour_planes = false;
    if (HasChromaFormatFields(profile_idc)) {
        chroma_format_idc = reader.ReadUnsignedExpGolomb();
        if (chroma_format_idc == 3) {
            has_separate_colour_planes = reader.ReadBit() == 1;
        }
        reader.ReadUnsignedExpGolomb();  // bit_depth_luma_minus8
        reader.ReadUnsignedExpGolomb();  // bit_depth_chroma_minus8
        reader.ReadBit();                // qpprime_y_zero_transform_bypass_flag
        if (reader.ReadBit() == 1) {     // seq_scaling_matrix_present_flag
            const int list_count = chroma_format_idc == 3 ? 12 : 8;
            for (int index = 0; index < list_count; ++index) {
                if (reader.ReadBit() == 1) {
                    SkipScalingList(reader, index < 6 ? 16 : 64);
                }
            }
        }
    }

    reader.ReadUnsignedExpGolomb();  // log2_max_frame_num_minus4
    const std::uint32_t pic_order_cnt_type = reader.ReadUnsignedExpGolomb();
    if (pic_order_cnt_type == 0) {
        reader.ReadUnsignedExpGolomb();  // log2_max_pic_order_cnt_lsb_minus4
    } else if (pic_order_cnt_type == 1) {
        reader.ReadBit();                // delta_pic_order_always_zero_flag
        reader.ReadSignedExpGolomb();    // offset_for_non_ref_pic
        reader.ReadSignedExpGolomb();    // offset_for_top_to_bottom_field
        const std::uint32_t cycle_length = reader.ReadUnsignedExpGolomb();
        if (cycle_length > 255) {
            return std::nullopt;
        }
        for (std::uint32_t index = 0; index < cycle_length; ++index) {
            reader.ReadSignedExpGolomb();
        }
    }
    reader.ReadUnsignedExpGolomb();  // max_num_ref_frames
    reader.ReadBit();                // gaps_in_frame_num_value_allowed_flag
    const std::uint32_t width_in_macroblocks = reader.ReadUnsignedExpGolomb() + 1;
    const std::uint32_t height_in_map_units = reader.ReadUnsignedExpGolomb() + 1;
    const std::uint32_t frame_mbs_only = reader.ReadBit();
    if (frame_mbs_only == 0) {
        reader.ReadBit();  // mb_adaptive_frame_field_flag
    }
    reader.ReadBit();  // direct_8x8_inference_flag

    std::uint32_t crop_left = 0;
    std::uint32_t crop_right = 0;
    std::uint32_t crop_top = 0;
    std::uint32_t crop_bottom = 0;
    if (reader.ReadBit() == 1) {
        crop_left = reader.ReadUnsignedExpGolomb();
        crop_right = reader.ReadUnsignedExpGolomb();
        crop_top = reader.ReadUnsignedExpGolomb();
        crop_bottom = reader.ReadUnsignedExpGolomb();
    }
    if (reader.HasOverrun() || chroma_format_idc > 3 || width_in_macroblocks > 1024 || height_in_map_units > 1024) {
        return std::nullopt;
    }

    // 잘라 내기 단위는 크로마 샘플링과 필드 부호화 여부에 따라 달라진다(식 7-19~7-22).
    const bool is_monochrome_or_separate = chroma_format_idc == 0 || has_separate_colour_planes;
    const std::uint32_t crop_unit_x = is_monochrome_or_separate ? 1 : (chroma_format_idc == 3 ? 1 : 2);
    const std::uint32_t crop_unit_y =
        (is_monochrome_or_separate ? 1 : (chroma_format_idc == 1 ? 2 : 1)) * (2 - frame_mbs_only);
    const std::uint32_t coded_width = width_in_macroblocks * 16;
    const std::uint32_t coded_height = (2 - frame_mbs_only) * height_in_map_units * 16;
    const std::uint64_t crop_width = std::uint64_t{crop_unit_x} * (std::uint64_t{crop_left} + crop_right);
    const std::uint64_t crop_height = std::uint64_t{crop_unit_y} * (std::uint64_t{crop_top} + crop_bottom);
    if (crop_width >= coded_width || crop_height >= coded_height) {
        return std::nullopt;
    }
    return H264Resolution{.width = static_cast<std::uint32_t>(coded_width - crop_width),
                          .height = static_cast<std::uint32_t>(coded_height - crop_height)};
}


}  // namespace vms::media
