#pragma once

#include <stdlib.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "recording/recorded_file.h"
#include "recording/recording_catalog_event.h"
#include "recording/recording_index.h"
#include "recording/recording_task.h"


namespace vms::recording::test {


// 320x240 Constrained Baseline SPS. ffmpeg(libx264)로 만든 값이라 해상도를 읽을 수 있다.
inline const std::vector<std::uint8_t> kSps = {0x67, 0x42, 0xC0, 0x0D, 0xD9, 0x01, 0x41, 0xFB,
                                               0x01, 0x10, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00,
                                               0x00, 0x03, 0x03, 0xC0, 0xF1, 0x42, 0xA4, 0x80};


// kSps와 짝인 PPS
inline const std::vector<std::uint8_t> kPps = {0x68, 0xCB, 0x83, 0xCB, 0x20};


// 테스트 하나가 쓰는 임시 디렉터리. 소멸할 때 안의 내용과 함께 지운다.
class TemporaryDirectory {
public:
    // 시스템 임시 디렉터리 아래에 새 디렉터리를 만든다.
    TemporaryDirectory() {
        std::string pattern = (std::filesystem::temp_directory_path() / "vms_recording_test_XXXXXX").string();
        if (::mkdtemp(pattern.data()) != nullptr) {
            path_ = pattern;
        }
    }


    // 디렉터리를 지운다.
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }


    // 같은 디렉터리를 두 번 지우지 않도록 복사를 금지한다.
    TemporaryDirectory(const TemporaryDirectory&) = delete;


    // 같은 디렉터리를 두 번 지우지 않도록 복사 대입을 금지한다.
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;


    // 만든 디렉터리 경로. 만들지 못했으면 비어 있다.
    const std::filesystem::path& Path() const { return path_; }


private:
    // 만든 디렉터리 경로
    std::filesystem::path path_;
};


// 열기 작업을 만든다.
inline RecordingTask MakeOpenTask(const std::string& camera_name, std::int64_t start_unix_ms) {
    return RecordingTask{.camera_name = camera_name,
                         .action = OpenFileTask{.start_unix_ms = start_unix_ms, .sps = kSps, .pps = kPps}};
}


// 지정한 길이의 GOP 하나를 담은 조각 작업을 만든다.
//
// camera_name: 카메라 이름
// start_time: 첫 프레임 재생 시각(90kHz)
// duration: 조각 길이(90kHz). 프레임 두 개로 나눈다.
// payload_size: 프레임마다 넣을 슬라이스 NAL 크기(바이트)
inline RecordingTask MakeWriteTask(const std::string& camera_name, std::uint64_t start_time, std::uint32_t duration,
                                   std::size_t payload_size = 100) {
    std::vector<std::uint8_t> idr(payload_size, 0xAB);
    idr[0] = 0x65;
    std::vector<std::uint8_t> slice(payload_size, 0xCD);
    slice[0] = 0x41;
    WriteFragmentTask write;
    write.frames.push_back(Fmp4Frame{.nal_units = {idr}, .decode_time = start_time, .duration = duration / 2});
    write.frames.push_back(
        Fmp4Frame{.nal_units = {slice}, .decode_time = start_time + duration / 2, .duration = duration - duration / 2});
    return RecordingTask{.camera_name = camera_name, .action = std::move(write)};
}


// 닫기 작업을 만든다.
inline RecordingTask MakeCloseTask(const std::string& camera_name) {
    return RecordingTask{.camera_name = camera_name, .action = CloseFileTask{}};
}


// 파일 전체를 읽는다. 열 수 없으면 빈 벡터다.
inline std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}


// 색인 파일을 읽어 헤더와 레코드를 해석한 결과.
struct IndexContents {
    // 헤더
    RecordingIndexHeader header;


    // 레코드. CRC가 맞지 않는 레코드에서 멈춘다.
    std::vector<RecordingIndexRecord> records;
};


// 색인 파일을 읽는다.
//
// path: .idx 경로
// 반환값: 헤더와 앞에서부터 유효한 레코드. 헤더가 깨졌으면 빈 optional
inline std::optional<IndexContents> ReadIndex(const std::filesystem::path& path) {
    const std::vector<std::uint8_t> bytes = ReadFileBytes(path);
    const auto header = DecodeRecordingIndexHeader(bytes);
    if (!header) {
        return std::nullopt;
    }
    IndexContents contents{.header = *header, .records = {}};
    for (std::size_t offset = kRecordingIndexHeaderSize; offset + kRecordingIndexRecordSize <= bytes.size();
         offset += kRecordingIndexRecordSize) {
        const auto record = DecodeRecordingIndexRecord(std::span(bytes).subspan(offset));
        if (!record) {
            break;
        }
        contents.records.push_back(*record);
    }
    return contents;
}


// 색인의 모든 레코드가 녹화 파일의 moof로 시작하는 조각을 가리키고 CRC32가 맞는지 확인한다.
//
// stem_path: 확장자를 뺀 파일 짝 경로
// 반환값: 레코드 수. 헤더가 깨졌거나 맞지 않는 레코드가 있으면 빈 optional
inline std::optional<std::size_t> VerifyRecordingPair(const std::filesystem::path& stem_path) {
    const auto index = ReadIndex(std::filesystem::path(stem_path.string() + ".idx"));
    const std::vector<std::uint8_t> video = ReadFileBytes(std::filesystem::path(stem_path.string() + ".mp4"));
    if (!index) {
        return std::nullopt;
    }
    for (const RecordingIndexRecord& record : index->records) {
        if (record.offset + record.size > video.size() || record.size < 8) {
            return std::nullopt;
        }
        const auto fragment = std::span(video).subspan(record.offset, record.size);
        if (std::string(fragment.begin() + 4, fragment.begin() + 8) != "moof" ||
            Crc32(fragment) != record.fragment_crc32) {
            return std::nullopt;
        }
    }
    return index->records.size();
}


// 조각 목록이 색인 파일의 레코드(CRC 제외)와 수와 값이 모두 같은지 확인한다.
//
// fragments: 확인할 조각 목록
// stem_path: 확장자를 뺀 파일 짝 경로
// 반환값: 같으면 true. 색인 헤더가 깨졌으면 false
inline bool FragmentsMatchIndex(const std::vector<RecordedFragment>& fragments,
                                const std::filesystem::path& stem_path) {
    const auto index = ReadIndex(std::filesystem::path(stem_path.string() + ".idx"));
    if (!index || index->records.size() != fragments.size()) {
        return false;
    }
    for (std::size_t position = 0; position < fragments.size(); ++position) {
        const RecordedFragment& fragment = fragments[position];
        const RecordingIndexRecord& record = index->records[position];
        if (fragment.start_time != record.start_time || fragment.duration != record.duration ||
            fragment.offset != record.offset || fragment.size != record.size) {
            return false;
        }
    }
    return true;
}


// 닫힌 파일 목록의 변경 알림을 받은 순서대로 모으는 함수를 만든다.
//
// events: 알림을 모을 목록. 돌려준 함수보다 오래 살아 있어야 한다.
// 반환값: RecordingFileStore·RecordingWriter에 넘길 알림 함수
//
// 잠금 없이 목록에 더하므로, RecordingWriter에 넘긴 경우 WaitUntilIdle이 반환된 뒤나 소멸한 뒤에만 목록을 읽는다.
inline RecordingCatalogListener CollectEvents(std::vector<RecordingCatalogEvent>& events) {
    return [&events](RecordingCatalogEvent event) { events.push_back(std::move(event)); };
}


}  // namespace vms::recording::test
