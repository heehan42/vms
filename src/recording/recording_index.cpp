#include "recording/recording_index.h"

#include <algorithm>
#include <format>


namespace vms::recording {


namespace {


// 헤더 맨 앞의 매직
constexpr std::array<std::uint8_t, 4> kIndexMagic = {'V', 'M', 'S', 'I'};


// 이 코드가 쓰고 읽는 색인 형식 버전
constexpr std::uint32_t kIndexVersion = 1;


// 헤더에서 CRC32를 계산하는 앞부분의 길이. 나머지 4바이트가 그 CRC32다.
constexpr std::size_t kHeaderCrcCoveredSize = kRecordingIndexHeaderSize - 4;


// 레코드에서 CRC32를 계산하는 앞부분의 길이. 나머지 4바이트가 그 CRC32다.
constexpr std::size_t kRecordCrcCoveredSize = kRecordingIndexRecordSize - 4;


// 반사형 다항식 0xEDB88320의 바이트별 CRC32 표를 만든다.
//
// 반환값: 바이트 값 하나를 처리했을 때의 CRC 변화량 256개
constexpr std::array<std::uint32_t, 256> MakeCrc32Table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t byte = 0; byte < 256; ++byte) {
        std::uint32_t value = byte;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1) ? (0xEDB88320U ^ (value >> 1)) : (value >> 1);
        }
        table[byte] = value;
    }
    return table;
}


// 컴파일할 때 만든 CRC32 표
constexpr std::array<std::uint32_t, 256> kCrc32Table = MakeCrc32Table();


// 부호 없는 정수를 리틀 엔디언으로 쓴다.
//
// destination: 쓸 위치. sizeof(Integer) 바이트 이상이어야 한다.
// value: 쓸 값
template <typename Integer>
void StoreLittleEndian(std::uint8_t* destination, Integer value) {
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        destination[index] = static_cast<std::uint8_t>(value >> (8 * index));
    }
}


// 리틀 엔디언 부호 없는 정수를 읽는다.
//
// source: 읽을 위치. sizeof(Integer) 바이트 이상이어야 한다.
// 반환값: 읽은 값
template <typename Integer>
Integer LoadLittleEndian(const std::uint8_t* source) {
    Integer value = 0;
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        value |= static_cast<Integer>(static_cast<Integer>(source[index]) << (8 * index));
    }
    return value;
}


}  // namespace


std::uint32_t Crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::uint8_t byte : bytes) {
        crc = kCrc32Table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}


std::array<std::uint8_t, kRecordingIndexHeaderSize> EncodeRecordingIndexHeader(const RecordingIndexHeader& header) {
    std::array<std::uint8_t, kRecordingIndexHeaderSize> bytes{};
    std::copy(kIndexMagic.begin(), kIndexMagic.end(), bytes.begin());
    StoreLittleEndian(&bytes[4], kIndexVersion);
    StoreLittleEndian(&bytes[8], static_cast<std::uint64_t>(header.start_unix_ms));
    StoreLittleEndian(&bytes[16], kRecordingTimescale);
    // 20~27은 예약 영역으로 0을 둔다.
    StoreLittleEndian(&bytes[kHeaderCrcCoveredSize], Crc32(std::span(bytes).first(kHeaderCrcCoveredSize)));
    return bytes;
}


std::expected<RecordingIndexHeader, std::string> DecodeRecordingIndexHeader(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kRecordingIndexHeaderSize) {
        return std::unexpected(std::format("색인 헤더가 잘림 ({}바이트)", bytes.size()));
    }
    const std::uint32_t stored_crc = LoadLittleEndian<std::uint32_t>(&bytes[kHeaderCrcCoveredSize]);
    if (Crc32(bytes.first(kHeaderCrcCoveredSize)) != stored_crc) {
        return std::unexpected("색인 헤더 CRC32 불일치");
    }
    if (!std::equal(kIndexMagic.begin(), kIndexMagic.end(), bytes.begin())) {
        return std::unexpected("색인 매직 불일치");
    }
    const std::uint32_t version = LoadLittleEndian<std::uint32_t>(&bytes[4]);
    if (version != kIndexVersion) {
        return std::unexpected(std::format("지원하지 않는 색인 버전: {}", version));
    }
    const std::uint32_t timescale = LoadLittleEndian<std::uint32_t>(&bytes[16]);
    if (timescale != kRecordingTimescale) {
        return std::unexpected(std::format("지원하지 않는 시간 단위: {}", timescale));
    }
    return RecordingIndexHeader{.start_unix_ms = static_cast<std::int64_t>(LoadLittleEndian<std::uint64_t>(&bytes[8]))};
}


std::array<std::uint8_t, kRecordingIndexRecordSize> EncodeRecordingIndexRecord(const RecordingIndexRecord& record) {
    std::array<std::uint8_t, kRecordingIndexRecordSize> bytes{};
    StoreLittleEndian(&bytes[0], record.start_time);
    StoreLittleEndian(&bytes[8], record.duration);
    StoreLittleEndian(&bytes[12], record.offset);
    StoreLittleEndian(&bytes[20], record.size);
    StoreLittleEndian(&bytes[24], record.fragment_crc32);
    // 28~31은 플래그, 32~35는 예약 영역이며 지금은 모두 0이다.
    StoreLittleEndian(&bytes[kRecordCrcCoveredSize], Crc32(std::span(bytes).first(kRecordCrcCoveredSize)));
    return bytes;
}


std::expected<RecordingIndexRecord, std::string> DecodeRecordingIndexRecord(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kRecordingIndexRecordSize) {
        return std::unexpected(std::format("색인 레코드가 잘림 ({}바이트)", bytes.size()));
    }
    const std::uint32_t stored_crc = LoadLittleEndian<std::uint32_t>(&bytes[kRecordCrcCoveredSize]);
    if (Crc32(bytes.first(kRecordCrcCoveredSize)) != stored_crc) {
        return std::unexpected("색인 레코드 CRC32 불일치");
    }
    return RecordingIndexRecord{
        .start_time = LoadLittleEndian<std::uint64_t>(&bytes[0]),
        .duration = LoadLittleEndian<std::uint32_t>(&bytes[8]),
        .offset = LoadLittleEndian<std::uint64_t>(&bytes[12]),
        .size = LoadLittleEndian<std::uint32_t>(&bytes[20]),
        .fragment_crc32 = LoadLittleEndian<std::uint32_t>(&bytes[24]),
    };
}


}  // namespace vms::recording
