#include "recording/fmp4.h"

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

#include "media/h264_file.h"
#include "media/h264_sps.h"
#include "recording/recording_index.h"


namespace vms::recording {


namespace {


using media::H264NalUnitType;


// trun 플래그: data-offset, 샘플별 길이·크기·플래그가 있음
constexpr std::uint32_t kTrunFlags = 0x000001 | 0x000100 | 0x000200 | 0x000400;


// tfhd 플래그: default-base-is-moof. 조각 안 위치의 기준을 moof 시작으로 한다.
constexpr std::uint32_t kTfhdDefaultBaseIsMoof = 0x020000;


// sync 샘플 플래그: 다른 샘플에 의존하지 않음(sample_depends_on = 2)
constexpr std::uint32_t kSyncSampleFlags = 0x02000000;


// 일반 샘플 플래그: 다른 샘플에 의존함(sample_depends_on = 1), sync 샘플이 아님
constexpr std::uint32_t kNonSyncSampleFlags = 0x01010000;


// 녹화 파일의 유일한 트랙 번호
constexpr std::uint32_t kTrackId = 1;


// 단위 행렬(16.16, 2.30 고정소수). mvhd·tkhd에 쓴다.
constexpr std::array<std::uint32_t, 9> kUnityMatrix = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};


// ISO BMFF 상자를 빅 엔디언으로 이어 쓰는 버퍼.
//
// BeginBox로 연 상자는 EndBox로 닫을 때 크기를 채운다. 열고 닫는 순서는 호출자가 맞춘다.
class BoxWriter {
public:
    // 1바이트를 쓴다.
    void WriteU8(std::uint8_t value) { bytes_.push_back(value); }


    // 2바이트 빅 엔디언 값을 쓴다.
    void WriteU16(std::uint16_t value) {
        WriteU8(static_cast<std::uint8_t>(value >> 8));
        WriteU8(static_cast<std::uint8_t>(value));
    }


    // 4바이트 빅 엔디언 값을 쓴다.
    void WriteU32(std::uint32_t value) {
        WriteU16(static_cast<std::uint16_t>(value >> 16));
        WriteU16(static_cast<std::uint16_t>(value));
    }


    // 8바이트 빅 엔디언 값을 쓴다.
    void WriteU64(std::uint64_t value) {
        WriteU32(static_cast<std::uint32_t>(value >> 32));
        WriteU32(static_cast<std::uint32_t>(value));
    }


    // 0 바이트를 count개 쓴다.
    void WriteZeros(std::size_t count) { bytes_.insert(bytes_.end(), count, 0); }


    // 네 글자 코드를 쓴다.
    void WriteFourCc(std::string_view code) { bytes_.insert(bytes_.end(), code.begin(), code.end()); }


    // 바이트 열을 그대로 쓴다.
    void WriteBytes(std::span<const std::uint8_t> bytes) { bytes_.insert(bytes_.end(), bytes.begin(), bytes.end()); }


    // 상자를 연다.
    //
    // type: 네 글자 상자 종류
    // 반환값: 상자 시작 위치. EndBox에 넘긴다.
    std::size_t BeginBox(std::string_view type) {
        const std::size_t start = bytes_.size();
        WriteU32(0);
        WriteFourCc(type);
        return start;
    }


    // 버전과 플래그를 가진 상자(full box)를 연다.
    //
    // type: 네 글자 상자 종류
    // version: 상자 버전
    // flags: 24비트 플래그
    // 반환값: 상자 시작 위치. EndBox에 넘긴다.
    std::size_t BeginFullBox(std::string_view type, std::uint8_t version, std::uint32_t flags) {
        const std::size_t start = BeginBox(type);
        WriteU32((static_cast<std::uint32_t>(version) << 24) | (flags & 0xFFFFFF));
        return start;
    }


    // 상자를 닫고 맨 앞 크기 칸을 채운다.
    //
    // start: BeginBox가 돌려준 위치
    void EndBox(std::size_t start) { PatchU32(start, static_cast<std::uint32_t>(bytes_.size() - start)); }


    // 이미 쓴 위치의 4바이트 값을 바꾼다.
    //
    // position: 바꿀 위치. 이미 쓴 범위 안이어야 한다.
    // value: 새 값
    void PatchU32(std::size_t position, std::uint32_t value) {
        for (int index = 0; index < 4; ++index) {
            bytes_[position + index] = static_cast<std::uint8_t>(value >> (24 - 8 * index));
        }
    }


    // 지금까지 쓴 바이트 수를 돌려준다.
    std::size_t Size() const { return bytes_.size(); }


    // 쓴 바이트를 넘겨준다. 이 뒤에는 객체를 쓰지 않는다.
    std::vector<std::uint8_t> Take() && { return std::move(bytes_); }


private:
    // 쓴 바이트
    std::vector<std::uint8_t> bytes_;
};


// 단위 행렬을 쓴다.
void WriteUnityMatrix(BoxWriter& writer) {
    for (const std::uint32_t value : kUnityMatrix) {
        writer.WriteU32(value);
    }
}


// ftyp 상자를 쓴다.
void WriteFtyp(BoxWriter& writer) {
    const std::size_t ftyp = writer.BeginBox("ftyp");
    writer.WriteFourCc("iso6");
    writer.WriteU32(0);
    for (const std::string_view brand : {"iso6", "isom", "avc1", "mp41"}) {
        writer.WriteFourCc(brand);
    }
    writer.EndBox(ftyp);
}


// mvhd 상자를 쓴다. 시간 단위 1000, 길이 0이다.
void WriteMvhd(BoxWriter& writer) {
    const std::size_t mvhd = writer.BeginFullBox("mvhd", 0, 0);
    writer.WriteU32(0);  // creation_time
    writer.WriteU32(0);  // modification_time
    writer.WriteU32(1000);
    writer.WriteU32(0);  // duration: 조각으로만 이루어진 파일
    writer.WriteU32(0x00010000);  // rate 1.0
    writer.WriteU16(0x0100);      // volume 1.0
    writer.WriteZeros(2 + 8);
    WriteUnityMatrix(writer);
    writer.WriteZeros(24);  // pre_defined
    writer.WriteU32(kTrackId + 1);
    writer.EndBox(mvhd);
}


// tkhd 상자를 쓴다.
//
// resolution: 표시 크기. 16.16 고정소수로 쓴다.
void WriteTkhd(BoxWriter& writer, const media::H264Resolution& resolution) {
    // 플래그 3: track_enabled | track_in_movie
    const std::size_t tkhd = writer.BeginFullBox("tkhd", 0, 3);
    writer.WriteU32(0);  // creation_time
    writer.WriteU32(0);  // modification_time
    writer.WriteU32(kTrackId);
    writer.WriteU32(0);  // reserved
    writer.WriteU32(0);  // duration
    writer.WriteZeros(8);
    writer.WriteU16(0);  // layer
    writer.WriteU16(0);  // alternate_group
    writer.WriteU16(0);  // volume: 영상 트랙
    writer.WriteU16(0);
    WriteUnityMatrix(writer);
    writer.WriteU32(resolution.width << 16);
    writer.WriteU32(resolution.height << 16);
    writer.EndBox(tkhd);
}


// avc1 샘플 엔트리와 그 안의 avcC를 쓴다.
//
// sps, pps: 매개변수 집합. 길이는 호출자가 검사했다.
// resolution: SPS에서 읽은 크기
void WriteAvc1(BoxWriter& writer, std::span<const std::uint8_t> sps, std::span<const std::uint8_t> pps,
               const media::H264Resolution& resolution) {
    const std::size_t avc1 = writer.BeginBox("avc1");
    writer.WriteZeros(6);
    writer.WriteU16(1);  // data_reference_index
    writer.WriteZeros(2 + 2 + 12);
    writer.WriteU16(static_cast<std::uint16_t>(resolution.width));
    writer.WriteU16(static_cast<std::uint16_t>(resolution.height));
    writer.WriteU32(0x00480000);  // 72dpi
    writer.WriteU32(0x00480000);
    writer.WriteU32(0);
    writer.WriteU16(1);  // frame_count
    writer.WriteZeros(32);  // compressorname
    writer.WriteU16(0x0018);
    writer.WriteU16(0xFFFF);  // pre_defined = -1

    // AVCDecoderConfigurationRecord(ISO/IEC 14496-15 5.3.3.1). NAL 길이 필드는 4바이트이고, 매개변수 집합은 하나씩이다.
    const std::size_t avcc = writer.BeginBox("avcC");
    writer.WriteU8(1);
    writer.WriteU8(sps[1]);  // profile_idc
    writer.WriteU8(sps[2]);  // 제약 플래그
    writer.WriteU8(sps[3]);  // level_idc
    writer.WriteU8(0xFF);    // 예약 6비트 + lengthSizeMinusOne = 3
    writer.WriteU8(0xE1);    // 예약 3비트 + numOfSequenceParameterSets = 1
    writer.WriteU16(static_cast<std::uint16_t>(sps.size()));
    writer.WriteBytes(sps);
    writer.WriteU8(1);
    writer.WriteU16(static_cast<std::uint16_t>(pps.size()));
    writer.WriteBytes(pps);
    writer.EndBox(avcc);

    writer.EndBox(avc1);
}


// 비어 있는 프레임 표 상자(stts, stsc, stco)를 쓴다.
void WriteEmptyTable(BoxWriter& writer, std::string_view type) {
    const std::size_t box = writer.BeginFullBox(type, 0, 0);
    writer.WriteU32(0);  // entry_count
    writer.EndBox(box);
}


// trak 상자 전체를 쓴다.
void WriteTrak(BoxWriter& writer, std::span<const std::uint8_t> sps, std::span<const std::uint8_t> pps,
               const media::H264Resolution& resolution) {
    const std::size_t trak = writer.BeginBox("trak");
    WriteTkhd(writer, resolution);

    const std::size_t mdia = writer.BeginBox("mdia");
    const std::size_t mdhd = writer.BeginFullBox("mdhd", 0, 0);
    writer.WriteU32(0);  // creation_time
    writer.WriteU32(0);  // modification_time
    writer.WriteU32(kRecordingTimescale);
    writer.WriteU32(0);       // duration
    writer.WriteU16(0x55C4);  // language "und"
    writer.WriteU16(0);
    writer.EndBox(mdhd);

    const std::size_t hdlr = writer.BeginFullBox("hdlr", 0, 0);
    writer.WriteU32(0);
    writer.WriteFourCc("vide");
    writer.WriteZeros(12);
    for (const char letter : std::string_view("VideoHandler")) {
        writer.WriteU8(static_cast<std::uint8_t>(letter));
    }
    writer.WriteU8(0);
    writer.EndBox(hdlr);

    const std::size_t minf = writer.BeginBox("minf");
    const std::size_t vmhd = writer.BeginFullBox("vmhd", 0, 1);
    writer.WriteZeros(2 + 6);  // graphicsmode, opcolor
    writer.EndBox(vmhd);

    const std::size_t dinf = writer.BeginBox("dinf");
    const std::size_t dref = writer.BeginFullBox("dref", 0, 0);
    writer.WriteU32(1);
    // 플래그 1: 데이터가 이 파일 안에 있음
    writer.EndBox(writer.BeginFullBox("url ", 0, 1));
    writer.EndBox(dref);
    writer.EndBox(dinf);

    const std::size_t stbl = writer.BeginBox("stbl");
    const std::size_t stsd = writer.BeginFullBox("stsd", 0, 0);
    writer.WriteU32(1);
    WriteAvc1(writer, sps, pps, resolution);
    writer.EndBox(stsd);
    WriteEmptyTable(writer, "stts");
    WriteEmptyTable(writer, "stsc");
    const std::size_t stsz = writer.BeginFullBox("stsz", 0, 0);
    writer.WriteU32(0);  // sample_size
    writer.WriteU32(0);  // sample_count
    writer.EndBox(stsz);
    WriteEmptyTable(writer, "stco");
    writer.EndBox(stbl);

    writer.EndBox(minf);
    writer.EndBox(mdia);
    writer.EndBox(trak);
}


// 프레임의 샘플 크기(길이 접두 4바이트 포함)를 구한다.
std::uint32_t SampleSizeOf(const Fmp4Frame& frame) {
    std::size_t size = 0;
    for (const auto& nal_unit : frame.nal_units) {
        if (IsFragmentNalUnit(nal_unit)) {
            size += 4 + nal_unit.size();
        }
    }
    return static_cast<std::uint32_t>(size);
}


}  // namespace


bool IsFragmentNalUnit(std::span<const std::uint8_t> nal_unit) {
    if (nal_unit.empty()) {
        return false;
    }
    const auto type = static_cast<H264NalUnitType>(media::NalUnitTypeOf(nal_unit));
    return type != H264NalUnitType::kAccessUnitDelimiter && type != H264NalUnitType::kSps &&
           type != H264NalUnitType::kPps;
}


std::expected<std::vector<std::uint8_t>, std::string> BuildFmp4InitSegment(std::span<const std::uint8_t> sps,
                                                                           std::span<const std::uint8_t> pps) {
    if (sps.size() < 4 || sps.size() > 0xFFFF) {
        return std::unexpected("SPS 길이가 avcC에 맞지 않음");
    }
    if (pps.empty() || pps.size() > 0xFFFF) {
        return std::unexpected("PPS 길이가 avcC에 맞지 않음");
    }
    const auto resolution = media::ParseH264SpsResolution(sps);
    if (!resolution || resolution->width > 0xFFFF || resolution->height > 0xFFFF) {
        return std::unexpected("SPS에서 해상도를 읽을 수 없음");
    }

    BoxWriter writer;
    WriteFtyp(writer);

    const std::size_t moov = writer.BeginBox("moov");
    WriteMvhd(writer);
    WriteTrak(writer, sps, pps, *resolution);
    const std::size_t mvex = writer.BeginBox("mvex");
    const std::size_t trex = writer.BeginFullBox("trex", 0, 0);
    writer.WriteU32(kTrackId);
    writer.WriteU32(1);  // default_sample_description_index
    writer.WriteU32(0);  // default_sample_duration
    writer.WriteU32(0);  // default_sample_size
    writer.WriteU32(0);  // default_sample_flags
    writer.EndBox(trex);
    writer.EndBox(mvex);
    writer.EndBox(moov);
    return std::move(writer).Take();
}


std::vector<std::uint8_t> BuildFmp4Fragment(std::span<const Fmp4Frame> frames, std::uint32_t sequence_number) {
    BoxWriter writer;

    const std::size_t moof = writer.BeginBox("moof");
    const std::size_t mfhd = writer.BeginFullBox("mfhd", 0, 0);
    writer.WriteU32(sequence_number);
    writer.EndBox(mfhd);

    const std::size_t traf = writer.BeginBox("traf");
    const std::size_t tfhd = writer.BeginFullBox("tfhd", 0, kTfhdDefaultBaseIsMoof);
    writer.WriteU32(kTrackId);
    writer.EndBox(tfhd);

    const std::size_t tfdt = writer.BeginFullBox("tfdt", 1, 0);
    writer.WriteU64(frames.empty() ? 0 : frames.front().decode_time);
    writer.EndBox(tfdt);

    const std::size_t trun = writer.BeginFullBox("trun", 0, kTrunFlags);
    writer.WriteU32(static_cast<std::uint32_t>(frames.size()));
    // data-offset는 moof 크기가 정해진 뒤에 채운다.
    const std::size_t data_offset_position = writer.Size();
    writer.WriteU32(0);
    for (std::size_t index = 0; index < frames.size(); ++index) {
        writer.WriteU32(frames[index].duration);
        writer.WriteU32(SampleSizeOf(frames[index]));
        writer.WriteU32(index == 0 ? kSyncSampleFlags : kNonSyncSampleFlags);
    }
    writer.EndBox(trun);
    writer.EndBox(traf);
    writer.EndBox(moof);

    // 기준이 moof 시작이므로, moof 크기에 mdat 헤더 8바이트를 더한 곳이 첫 샘플이다.
    writer.PatchU32(data_offset_position, static_cast<std::uint32_t>(writer.Size() - moof + 8));

    const std::size_t mdat = writer.BeginBox("mdat");
    for (const Fmp4Frame& frame : frames) {
        for (const auto& nal_unit : frame.nal_units) {
            if (IsFragmentNalUnit(nal_unit)) {
                writer.WriteU32(static_cast<std::uint32_t>(nal_unit.size()));
                writer.WriteBytes(nal_unit);
            }
        }
    }
    writer.EndBox(mdat);
    return std::move(writer).Take();
}


}  // namespace vms::recording
