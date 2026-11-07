#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>


namespace vms::recording {


// 색인 파일 헤더의 크기(바이트)
inline constexpr std::size_t kRecordingIndexHeaderSize = 32;


// 색인 레코드 하나의 크기(바이트). 레코드는 헤더 뒤에 이 크기로 이어 붙는다.
inline constexpr std::size_t kRecordingIndexRecordSize = 40;


// 색인과 녹화 파일의 재생 시각 단위(Hz). RTP H.264 타임스탬프와 같다.
inline constexpr std::uint32_t kRecordingTimescale = 90000;


// 바이트 열의 CRC32를 계산한다.
//
// bytes: 계산할 바이트
// 반환값: 다항식 0xEDB88320(반사형), 초기값 0xFFFFFFFF, 마지막 비트 반전의 CRC32. zlib의 crc32와 같다.
std::uint32_t Crc32(std::span<const std::uint8_t> bytes);


// 색인 파일 헤더의 내용.
//
// 매직, 버전, 시간 단위는 고정값이라 담지 않는다.
struct RecordingIndexHeader {
    // 녹화 파일의 시작 시각(유닉스 밀리초, UTC). 녹화 파일 안 재생 시각 0이 이 시각이다.
    std::int64_t start_unix_ms = 0;
};


// 조각(moof+mdat) 하나를 가리키는 색인 레코드.
struct RecordingIndexRecord {
    // 조각 첫 프레임의 재생 시각(90kHz, 파일 시작 기준)
    std::uint64_t start_time = 0;


    // 조각이 덮는 시간(90kHz)
    std::uint32_t duration = 0;


    // 녹화 파일 안에서 조각이 시작하는 위치(바이트)
    std::uint64_t offset = 0;


    // 조각의 크기(바이트)
    std::uint32_t size = 0;


    // 조각 바이트 전체의 CRC32
    std::uint32_t fragment_crc32 = 0;
};


// 색인 헤더를 바이트로 만든다.
//
// header: 기록할 헤더
// 반환값: 매직 "VMSI", 버전 1, 시작 시각, 시간 단위 90000, 예약, 헤더 CRC32 순의 32바이트(리틀 엔디언)
std::array<std::uint8_t, kRecordingIndexHeaderSize> EncodeRecordingIndexHeader(const RecordingIndexHeader& header);


// 바이트에서 색인 헤더를 읽는다.
//
// bytes: 색인 파일 앞부분. kRecordingIndexHeaderSize 이상이어야 한다.
// 반환값: 읽은 헤더. 바이트가 모자라거나, 헤더 CRC·매직·버전·시간 단위가 맞지 않으면 원인 메시지
std::expected<RecordingIndexHeader, std::string> DecodeRecordingIndexHeader(std::span<const std::uint8_t> bytes);


// 색인 레코드를 바이트로 만든다.
//
// record: 기록할 레코드
// 반환값: 시작 시각, 길이, 위치, 크기, 조각 CRC32, 플래그(0), 예약(0), 레코드 CRC32 순의 40바이트(리틀 엔디언)
std::array<std::uint8_t, kRecordingIndexRecordSize> EncodeRecordingIndexRecord(const RecordingIndexRecord& record);


// 바이트에서 색인 레코드 하나를 읽는다.
//
// bytes: 레코드가 시작하는 위치부터의 바이트. kRecordingIndexRecordSize 이상이어야 한다.
// 반환값: 읽은 레코드. 바이트가 모자라거나 레코드 CRC32가 맞지 않으면 원인 메시지
//
// 레코드가 가리키는 조각이 실제로 녹화 파일에 있는지는 확인하지 않는다. 그 검사는 복구가 한다.
std::expected<RecordingIndexRecord, std::string> DecodeRecordingIndexRecord(std::span<const std::uint8_t> bytes);


}  // namespace vms::recording
