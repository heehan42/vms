#include "recording/fmp4.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "test_support.h"

using vms::recording::BuildFmp4Fragment;
using vms::recording::BuildFmp4InitSegment;
using vms::recording::Fmp4Frame;
using vms::test::Expect;


namespace {


// 320x240 Constrained Baseline(레벨 1.3) SPS. ffmpeg(libx264)로 만든 값이다.
const std::vector<std::uint8_t> kSps = {0x67, 0x42, 0xC0, 0x0D, 0xD9, 0x01, 0x41, 0xFB, 0x01, 0x10, 0x00, 0x00,
                                        0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0xC0, 0xF1, 0x42, 0xA4, 0x80};


// kSps와 짝인 PPS
const std::vector<std::uint8_t> kPps = {0x68, 0xCB, 0x83, 0xCB, 0x20};


// AUD NAL
const std::vector<std::uint8_t> kAud = {0x09, 0xF0};


// IDR 슬라이스 NAL(내용은 검사용 임의 값)
const std::vector<std::uint8_t> kIdrSlice = {0x65, 0x88, 0x84, 0x21, 0x10};


// SEI NAL. 조각에 남아야 한다.
const std::vector<std::uint8_t> kSei = {0x06, 0x05, 0x01, 0xAA, 0x80};


// 일반 슬라이스 NAL
const std::vector<std::uint8_t> kSlice = {0x41, 0x9A, 0x02, 0x03};


// 테스트에서 읽은 상자 하나의 위치.
struct Box {
    // 네 글자 종류
    std::string type;


    // 상자 시작 위치(크기 칸)
    std::size_t start = 0;


    // 상자 전체 크기
    std::size_t size = 0;


    // 내용(크기·종류 8바이트 뒤)
    std::span<const std::uint8_t> payload;
};


// 4바이트 빅 엔디언 값을 읽는다.
std::uint32_t ReadU32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) | bytes[offset + 3];
}


// 8바이트 빅 엔디언 값을 읽는다.
std::uint64_t ReadU64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint64_t>(ReadU32(bytes, offset)) << 32) | ReadU32(bytes, offset + 4);
}


// 바이트 열을 이어진 상자 목록으로 나눈다.
//
// bytes: 상자가 이어진 바이트
// base: bytes 첫 바이트의 바깥 기준 위치. Box::start에 더한다.
// 반환값: 상자 목록. 크기가 맞지 않으면 빈 optional
std::optional<std::vector<Box>> ParseBoxes(std::span<const std::uint8_t> bytes, std::size_t base = 0) {
    std::vector<Box> boxes;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 8) {
            return std::nullopt;
        }
        const std::size_t size = ReadU32(bytes, offset);
        if (size < 8 || size > bytes.size() - offset) {
            return std::nullopt;
        }
        boxes.push_back(Box{.type = std::string(reinterpret_cast<const char*>(&bytes[offset + 4]), 4),
                            .start = base + offset,
                            .size = size,
                            .payload = bytes.subspan(offset + 8, size - 8)});
        offset += size;
    }
    return boxes;
}


// 상자 목록의 종류를 순서대로 이어 붙인다.
std::string TypesOf(const std::vector<Box>& boxes) {
    std::string types;
    for (const Box& box : boxes) {
        types += types.empty() ? box.type : " " + box.type;
    }
    return types;
}


// 이름으로 상자를 찾는다. 없으면 nullptr이다.
const Box* FindBox(const std::vector<Box>& boxes, std::string_view type) {
    for (const Box& box : boxes) {
        if (box.type == type) {
            return &box;
        }
    }
    return nullptr;
}


// 상자의 하위 상자를 읽는다.
//
// box: 부모 상자
// skip: 하위 상자 앞에 있는 고정 필드 길이
std::vector<Box> ChildrenOf(const Box& box, std::size_t skip = 0) {
    return ParseBoxes(box.payload.subspan(skip), box.start + 8 + skip).value_or(std::vector<Box>{});
}


// 초기화 구간의 상자 순서, 시간 단위, 해상도, avcC를 검사한다.
void TestInitSegment() {
    const auto init = BuildFmp4InitSegment(kSps, kPps);
    Expect(init.has_value(), "초기화 구간 생성");
    if (!init) {
        return;
    }
    const auto top = ParseBoxes(*init);
    Expect(top && TypesOf(*top) == "ftyp moov", "최상위 상자 순서와 크기");
    if (!top || top->size() != 2) {
        return;
    }

    const auto& ftyp = (*top)[0].payload;
    Expect(std::string_view(reinterpret_cast<const char*>(ftyp.data()), 4) == "iso6", "ftyp major brand");

    const auto moov = ChildrenOf((*top)[1]);
    Expect(TypesOf(moov) == "mvhd trak mvex", "moov 하위 상자");
    const Box* trak = FindBox(moov, "trak");
    const Box* mvex = FindBox(moov, "mvex");
    if (trak == nullptr || mvex == nullptr) {
        return;
    }
    const auto trex = ChildrenOf(*mvex);
    Expect(TypesOf(trex) == "trex" && ReadU32(trex[0].payload, 4) == 1, "trex track_id 1");

    const auto trak_children = ChildrenOf(*trak);
    Expect(TypesOf(trak_children) == "tkhd mdia", "trak 하위 상자");
    const Box* tkhd = FindBox(trak_children, "tkhd");
    if (tkhd != nullptr) {
        // 버전·플래그 4, 시각 8, track_id 4, 예약 4, 길이 4, 예약 8, 기타 8, 행렬 36 뒤에 크기가 있다.
        Expect(ReadU32(tkhd->payload, 76) == (320U << 16), "tkhd 가로 크기");
        Expect(ReadU32(tkhd->payload, 80) == (240U << 16), "tkhd 세로 크기");
    }
    const Box* mdia = FindBox(trak_children, "mdia");
    if (mdia == nullptr) {
        return;
    }
    const auto mdia_children = ChildrenOf(*mdia);
    Expect(TypesOf(mdia_children) == "mdhd hdlr minf", "mdia 하위 상자");
    Expect(ReadU32(mdia_children[0].payload, 12) == 90000, "mdhd timescale 90000");
    Expect(std::string_view(reinterpret_cast<const char*>(&mdia_children[1].payload[8]), 4) == "vide", "hdlr vide");

    const auto minf = ChildrenOf(mdia_children[2]);
    Expect(TypesOf(minf) == "vmhd dinf stbl", "minf 하위 상자");
    const auto stbl = ChildrenOf(minf[2]);
    Expect(TypesOf(stbl) == "stsd stts stsc stsz stco", "stbl 하위 상자");
    const auto stsd = ChildrenOf(stbl[0], 8);
    Expect(TypesOf(stsd) == "avc1", "stsd 샘플 엔트리");
    if (stsd.size() != 1) {
        return;
    }
    // 샘플 엔트리 고정 필드 78바이트 뒤에 avcC가 있다.
    const auto avc1_children = ChildrenOf(stsd[0], 78);
    Expect(TypesOf(avc1_children) == "avcC", "avc1 안의 avcC");
    if (avc1_children.size() != 1) {
        return;
    }
    const auto avcc = avc1_children[0].payload;
    Expect(avcc[0] == 1 && avcc[1] == 0x42 && avcc[2] == 0xC0 && avcc[3] == 0x0D, "avcC 버전·프로파일·레벨");
    Expect(avcc[4] == 0xFF && avcc[5] == 0xE1, "avcC 길이 4바이트, SPS 하나");
    const std::size_t sps_size = (avcc[6] << 8) | avcc[7];
    Expect(sps_size == kSps.size() && std::equal(kSps.begin(), kSps.end(), avcc.begin() + 8), "avcC의 SPS");
    const std::size_t pps_offset = 8 + sps_size;
    const std::size_t pps_size = (avcc[pps_offset + 1] << 8) | avcc[pps_offset + 2];
    Expect(avcc[pps_offset] == 1 && pps_size == kPps.size() &&
               std::equal(kPps.begin(), kPps.end(), avcc.begin() + pps_offset + 3),
           "avcC의 PPS");
}


// 해상도를 읽을 수 없는 SPS를 거부하는지 검사한다.
void TestInitSegmentRejectsBadSps() {
    const std::vector<std::uint8_t> short_sps = {0x67, 0x42, 0xC0};
    Expect(!BuildFmp4InitSegment(short_sps, kPps), "짧은 SPS 거부");
    const std::vector<std::uint8_t> truncated_sps = {0x67, 0x42, 0xC0, 0x1F, 0xDA};
    Expect(!BuildFmp4InitSegment(truncated_sps, kPps), "해상도를 읽을 수 없는 SPS 거부");
    Expect(!BuildFmp4InitSegment(kSps, {}), "빈 PPS 거부");
}


// 조각의 상자, tfdt·trun 값, data-offset, mdat 내용을 검사한다.
void TestFragment() {
    std::vector<Fmp4Frame> frames(3);
    frames[0] = Fmp4Frame{.nal_units = {kAud, kSps, kPps, kSei, kIdrSlice}, .decode_time = 270000, .duration = 3000};
    frames[1] = Fmp4Frame{.nal_units = {kAud, kSlice}, .decode_time = 273000, .duration = 3003};
    frames[2] = Fmp4Frame{.nal_units = {kSlice, kSlice}, .decode_time = 276003, .duration = 2997};

    const std::vector<std::uint8_t> fragment = BuildFmp4Fragment(frames, 7);
    const auto top = ParseBoxes(fragment);
    Expect(top && TypesOf(*top) == "moof mdat", "조각 상자 순서와 크기");
    if (!top || top->size() != 2) {
        return;
    }
    const Box& moof = (*top)[0];
    const Box& mdat = (*top)[1];

    const auto moof_children = ChildrenOf(moof);
    Expect(TypesOf(moof_children) == "mfhd traf", "moof 하위 상자");
    Expect(ReadU32(moof_children[0].payload, 4) == 7, "mfhd 조각 번호");

    const auto traf = ChildrenOf(moof_children[1]);
    Expect(TypesOf(traf) == "tfhd tfdt trun", "traf 하위 상자");
    if (traf.size() != 3) {
        return;
    }
    Expect(ReadU32(traf[0].payload, 0) == 0x020000 && ReadU32(traf[0].payload, 4) == 1,
           "tfhd default-base-is-moof, track 1");
    Expect(traf[1].payload[0] == 1 && ReadU64(traf[1].payload, 4) == 270000, "tfdt 버전 1과 첫 프레임 시각");

    const auto trun = traf[2].payload;
    Expect(ReadU32(trun, 4) == 3, "trun 프레임 수");
    const std::uint32_t data_offset = ReadU32(trun, 8);
    Expect(moof.start + data_offset == mdat.start + 8, "data-offset이 mdat 데이터 시작을 가리킴");

    // AUD·SPS·PPS는 빠지고, SEI와 슬라이스만 길이 접두와 함께 남는다.
    const std::uint32_t expected_sizes[] = {static_cast<std::uint32_t>(4 + kSei.size() + 4 + kIdrSlice.size()),
                                            static_cast<std::uint32_t>(4 + kSlice.size()),
                                            static_cast<std::uint32_t>(2 * (4 + kSlice.size()))};
    const std::uint32_t expected_durations[] = {3000, 3003, 2997};
    for (std::size_t index = 0; index < 3; ++index) {
        const std::size_t entry = 12 + index * 12;
        Expect(ReadU32(trun, entry) == expected_durations[index], "trun 프레임 길이");
        Expect(ReadU32(trun, entry + 4) == expected_sizes[index], "trun 프레임 크기");
        Expect(ReadU32(trun, entry + 8) == (index == 0 ? 0x02000000U : 0x01010000U), "첫 프레임만 sync 플래그");
    }

    std::vector<std::uint8_t> expected_mdat;
    for (const auto* nal_unit : {&kSei, &kIdrSlice, &kSlice, &kSlice, &kSlice}) {
        const auto size = static_cast<std::uint32_t>(nal_unit->size());
        expected_mdat.insert(expected_mdat.end(), {0, 0, static_cast<std::uint8_t>(size >> 8),
                                                   static_cast<std::uint8_t>(size)});
        expected_mdat.insert(expected_mdat.end(), nal_unit->begin(), nal_unit->end());
    }
    Expect(std::vector<std::uint8_t>(mdat.payload.begin(), mdat.payload.end()) == expected_mdat,
           "mdat은 4바이트 길이 + NAL 순서이며 AUD·SPS·PPS가 없음");
}


}  // namespace


int main() {
    TestInitSegment();
    TestInitSegmentRejectsBadSps();
    TestFragment();
    return vms::test::ExitCode();
}
