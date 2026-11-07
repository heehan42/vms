#include "recording/recording_index.h"

#include <cstdint>
#include <span>
#include <string_view>

#include "test_support.h"

using vms::recording::Crc32;
using vms::recording::DecodeRecordingIndexHeader;
using vms::recording::DecodeRecordingIndexRecord;
using vms::recording::EncodeRecordingIndexHeader;
using vms::recording::EncodeRecordingIndexRecord;
using vms::recording::kRecordingIndexHeaderSize;
using vms::recording::kRecordingIndexRecordSize;
using vms::recording::RecordingIndexHeader;
using vms::recording::RecordingIndexRecord;
using vms::test::Expect;


namespace {


// 알려진 입력의 CRC32 값을 검사한다.
void TestCrc32KnownValues() {
    constexpr std::string_view kCheckInput = "123456789";
    const auto* const bytes = reinterpret_cast<const std::uint8_t*>(kCheckInput.data());
    Expect(Crc32(std::span(bytes, kCheckInput.size())) == 0xCBF43926U, "\"123456789\"의 CRC32");
    Expect(Crc32({}) == 0, "빈 입력의 CRC32");
}


// 헤더의 왕복, 바이트 변조, 잘림을 검사한다.
void TestHeader() {
    const RecordingIndexHeader header{.start_unix_ms = 1793786465123};
    auto bytes = EncodeRecordingIndexHeader(header);
    Expect(bytes[0] == 'V' && bytes[1] == 'M' && bytes[2] == 'S' && bytes[3] == 'I', "헤더 매직");

    const auto decoded = DecodeRecordingIndexHeader(bytes);
    Expect(decoded && decoded->start_unix_ms == header.start_unix_ms, "헤더 왕복");
    Expect(!DecodeRecordingIndexHeader(std::span(bytes).first(kRecordingIndexHeaderSize - 1)), "잘린 헤더");

    for (std::size_t index = 0; index < bytes.size(); ++index) {
        auto corrupted = bytes;
        corrupted[index] ^= 0x01;
        if (DecodeRecordingIndexHeader(corrupted)) {
            Expect(false, "바이트 하나를 바꾼 헤더는 실패");
            break;
        }
    }
}


// 레코드의 왕복, 바이트 변조, 잘림을 검사한다.
void TestRecord() {
    const RecordingIndexRecord record{.start_time = 0x1'0000'0003ULL,
                                      .duration = 90000,
                                      .offset = 0x2'0000'0010ULL,
                                      .size = 250000,
                                      .fragment_crc32 = 0xDEADBEEF};
    const auto bytes = EncodeRecordingIndexRecord(record);
    const auto decoded = DecodeRecordingIndexRecord(bytes);
    Expect(decoded.has_value(), "레코드 해석");
    if (decoded) {
        Expect(decoded->start_time == record.start_time && decoded->duration == record.duration &&
                   decoded->offset == record.offset && decoded->size == record.size &&
                   decoded->fragment_crc32 == record.fragment_crc32,
               "레코드 왕복");
    }
    Expect(bytes[0] == 0x03 && bytes[4] == 0x01, "리틀 엔디언 시작 시각");

    Expect(!DecodeRecordingIndexRecord(std::span(bytes).first(kRecordingIndexRecordSize / 2)), "반쪽 레코드");
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        auto corrupted = bytes;
        corrupted[index] ^= 0x80;
        if (DecodeRecordingIndexRecord(corrupted)) {
            Expect(false, "바이트 하나를 바꾼 레코드는 실패");
            break;
        }
    }
}


}  // namespace


int main() {
    TestCrc32KnownValues();
    TestHeader();
    TestRecord();
    return vms::test::ExitCode();
}
