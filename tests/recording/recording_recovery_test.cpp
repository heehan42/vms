#include "recording/recording_recovery.h"

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#include "recording/recording_file_name.h"
#include "recording/recording_index.h"
#include "recording/recording_file_store.h"
#include "recording_test_support.h"
#include "test_support.h"

namespace fs = std::filesystem;

using vms::recording::CameraRecoveryReport;
using vms::recording::FormatRecordingFileStem;
using vms::recording::kRecordingIndexHeaderSize;
using vms::recording::kRecordingIndexRecordSize;
using vms::recording::RecordingOptions;
using vms::recording::RecordingRecoveryResult;
using vms::recording::RecordingFileStore;
using vms::recording::RecoverRecordings;
using vms::recording::test::MakeCloseTask;
using vms::recording::test::MakeOpenTask;
using vms::recording::test::MakeWriteTask;
using vms::recording::test::ReadIndex;
using vms::recording::test::TemporaryDirectory;
using vms::recording::test::VerifyRecordingPair;
using vms::test::Expect;


namespace {


// 2026-11-04 10:00:05 UTC의 유닉스 밀리초
constexpr std::int64_t kStartUnixMs = 1793786405LL * 1000;


// 1초(90kHz)
constexpr std::uint32_t kOneSecond = 90000;


// 정상 파일 짝 하나의 조각 수
constexpr std::uint32_t kFragmentCount = 3;


// RecordingFileStore로 카메라 폴더마다 조각 3개짜리 정상 파일 짝을 하나씩 만든다.
//
// directory: 녹화 디렉터리
// camera_names: 만들 카메라 폴더 이름
void WriteNormalPairs(const fs::path& directory, std::initializer_list<const char*> camera_names) {
    RecordingOptions options;
    options.directory = directory;
    options.retention_seconds = 3600;
    options.min_free_bytes = 0;
    options.query_free_space = [](const fs::path&) -> std::expected<std::uint64_t, std::error_code> {
        return std::numeric_limits<std::uint64_t>::max();
    };
    for (const char* camera_name : camera_names) {
        fs::create_directories(directory / camera_name);
    }

    RecordingFileStore store(std::move(options), {});
    for (const char* camera_name : camera_names) {
        store.Execute(MakeOpenTask(camera_name, kStartUnixMs));
        for (std::uint32_t index = 0; index < kFragmentCount; ++index) {
            store.Execute(MakeWriteTask(camera_name, index * kOneSecond, kOneSecond, 1000));
        }
        store.Execute(MakeCloseTask(camera_name));
    }
}


// 카메라 폴더 안 파일 짝의 경로 앞부분
fs::path StemPath(const fs::path& directory, const char* camera_name) {
    return directory / camera_name / FormatRecordingFileStem(kStartUnixMs, 0);
}


// 확장자를 붙인 경로
fs::path WithExtension(const fs::path& stem_path, const char* extension) {
    return fs::path(stem_path.string() + extension);
}


// 파일 안 한 바이트를 뒤집는다.
void FlipByte(const fs::path& path, std::uint64_t offset) {
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    stream.seekg(static_cast<std::streamoff>(offset));
    const int value = stream.get();
    stream.seekp(static_cast<std::streamoff>(offset));
    stream.put(static_cast<char>(value ^ 0xFF));
}


// 카메라 폴더의 요약을 찾는다.
const CameraRecoveryReport* FindReport(const RecordingRecoveryResult& result, std::string_view camera_folder) {
    for (const CameraRecoveryReport& report : result.reports) {
        if (report.camera_folder == camera_folder) {
            return &report;
        }
    }
    return nullptr;
}


// 손상 종류마다 카메라 폴더 하나를 만들어 복구 결과를 검사한다.
void TestRecovery() {
    const TemporaryDirectory temporary;
    const fs::path& directory = temporary.Path();
    WriteNormalPairs(directory, {"normal", "short_tail", "bad_fragment", "half_record", "bad_header", "no_valid",
                                 "orphans", "unrelated"});

    const auto normal_index = ReadIndex(WithExtension(StemPath(directory, "normal"), ".idx"));
    if (!normal_index || normal_index->records.size() != kFragmentCount) {
        Expect(false, "정상 파일 짝 준비");
        return;
    }
    const auto& records = normal_index->records;
    const std::uint64_t normal_video_size = fs::file_size(WithExtension(StemPath(directory, "normal"), ".mp4"));

    // .mp4 꼬리를 마지막 조각 중간에서 자른다.
    fs::resize_file(WithExtension(StemPath(directory, "short_tail"), ".mp4"), records[2].offset + 10);
    // 두 번째 조각의 mdat 바이트 하나를 바꾼다.
    FlipByte(WithExtension(StemPath(directory, "bad_fragment"), ".mp4"), records[1].offset + records[1].size - 1);
    // 마지막 레코드를 반쪽만 남긴다.
    fs::resize_file(WithExtension(StemPath(directory, "half_record"), ".idx"),
                    kRecordingIndexHeaderSize + 2 * kRecordingIndexRecordSize + kRecordingIndexRecordSize / 2);
    FlipByte(WithExtension(StemPath(directory, "bad_header"), ".idx"), 9);
    FlipByte(WithExtension(StemPath(directory, "no_valid"), ".idx"), kRecordingIndexHeaderSize + 1);
    // 짝 없는 .mp4와 .idx를 만든다.
    fs::rename(WithExtension(StemPath(directory, "orphans"), ".idx"),
               directory / "orphans" / (FormatRecordingFileStem(kStartUnixMs, 1) + ".idx"));
    // 이름 규칙에 맞지 않는 항목과 녹화 디렉터리 바로 아래의 일반 파일
    std::ofstream(directory / "unrelated" / "notes.txt") << "memo";
    std::ofstream(directory / "unrelated" / "20261104T100005Z.tmp") << "temp";
    fs::create_directories(directory / "unrelated" / "20261104T100006Z.mp4");
    std::ofstream(directory / "readme.txt") << "top";

    const auto result = RecoverRecordings(directory);
    Expect(result.has_value(), "복구 성공");
    if (!result) {
        return;
    }

    const auto& normal_files = result->files.at("normal");
    Expect(normal_files.size() == 1 && normal_files[0].start_unix_ms == kStartUnixMs &&
               normal_files[0].duration == kFragmentCount * kOneSecond,
           "정상 파일은 시작 시각·길이와 함께 목록에 들어감");
    Expect(fs::file_size(WithExtension(StemPath(directory, "normal"), ".mp4")) == normal_video_size,
           "정상 파일은 그대로");
    const CameraRecoveryReport* normal_report = FindReport(*result, "normal");
    Expect(normal_report && normal_report->kept_count == 1 && normal_report->truncated_count == 0 &&
               normal_report->deleted_count == 0,
           "정상 폴더 요약");

    // 잘린 꼬리: 세 번째 레코드부터 빠지고 두 파일이 잘린다.
    const fs::path short_tail = StemPath(directory, "short_tail");
    Expect(VerifyRecordingPair(short_tail) == 2, "꼬리가 잘린 파일은 레코드 2개");
    Expect(fs::file_size(WithExtension(short_tail, ".mp4")) == records[1].offset + records[1].size,
           "녹화 파일은 마지막 유효 조각 끝에서 잘림");
    Expect(fs::file_size(WithExtension(short_tail, ".idx")) == kRecordingIndexHeaderSize + 2 * kRecordingIndexRecordSize,
           "색인 파일은 마지막 유효 레코드 끝에서 잘림");
    Expect(result->files.at("short_tail")[0].duration == 2 * kOneSecond, "잘린 파일의 길이");
    Expect(FindReport(*result, "short_tail")->truncated_count == 1, "잘라 낸 짝 수");

    Expect(VerifyRecordingPair(StemPath(directory, "bad_fragment")) == 1, "바이트가 바뀐 조각부터 빠짐");
    Expect(VerifyRecordingPair(StemPath(directory, "half_record")) == 2, "반쪽 레코드가 빠짐");
    Expect(fs::file_size(WithExtension(StemPath(directory, "half_record"), ".mp4")) ==
               records[1].offset + records[1].size,
           "반쪽 레코드의 조각도 잘림");

    for (const char* camera_name : {"bad_header", "no_valid"}) {
        Expect(!fs::exists(WithExtension(StemPath(directory, camera_name), ".mp4")) &&
                   !fs::exists(WithExtension(StemPath(directory, camera_name), ".idx")),
               std::string(camera_name) + ": 짝을 지움");
        Expect(result->files.at(camera_name).empty(), std::string(camera_name) + ": 목록에 없음");
    }
    Expect(fs::is_empty(directory / "orphans"), "짝 없는 .mp4·.idx를 지움");
    Expect(FindReport(*result, "orphans")->deleted_count == 2, "짝 없는 파일 2개 삭제");

    Expect(fs::exists(directory / "unrelated" / "notes.txt") &&
               fs::exists(directory / "unrelated" / "20261104T100005Z.tmp") &&
               fs::is_directory(directory / "unrelated" / "20261104T100006Z.mp4"),
           "이름 규칙에 맞지 않는 항목은 그대로");
    Expect(fs::exists(directory / "readme.txt"), "녹화 디렉터리 바로 아래 일반 파일은 그대로");
    Expect(!result->files.contains("readme.txt"), "일반 파일은 카메라 폴더가 아님");
}


}  // namespace


int main() {
    TestRecovery();
    return vms::test::ExitCode();
}
