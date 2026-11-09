#include "recording/recording_file_store.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <filesystem>
#include <limits>
#include <utility>
#include <variant>
#include <vector>

#include "recording/recording_file_name.h"
#include "recording_test_support.h"
#include "test_support.h"

namespace fs = std::filesystem;

using vms::recording::FormatRecordingFileStem;
using vms::recording::RecordedFile;
using vms::recording::RecordedFileClosed;
using vms::recording::RecordedFileDeleted;
using vms::recording::RecordingCatalogEvent;
using vms::recording::RecordedFileCatalog;
using vms::recording::RecordingOptions;
using vms::recording::RecordingFileStore;
using vms::recording::test::CollectEvents;
using vms::recording::test::FragmentsMatchIndex;
using vms::recording::test::MakeCloseTask;
using vms::recording::test::MakeOpenTask;
using vms::recording::test::MakeWriteTask;
using vms::recording::test::ReadIndex;
using vms::recording::test::TemporaryDirectory;
using vms::recording::test::VerifyRecordingPair;
using vms::test::Expect;


namespace {


// 2026-11-04 10:00:05 UTC의 유닉스 밀리초
constexpr std::int64_t kBaseUnixMs = 1793786405LL * 1000;


// 1초(90kHz)
constexpr std::uint32_t kOneSecond = 90000;


// 여유 공간이 충분하다고 답하는 값
constexpr std::uint64_t kPlentyOfSpace = std::numeric_limits<std::uint64_t>::max();


// 테스트용 녹화 설정을 만들고 카메라 폴더를 준비한다.
//
// directory: 녹화 디렉터리
// free_bytes: 여유 공간 조회가 돌려줄 값을 담은 변수. 테스트가 끝날 때까지 살아 있어야 한다.
RecordingOptions MakeOptions(const fs::path& directory, const std::atomic<std::uint64_t>& free_bytes) {
    for (const char* camera_name : {"front", "back"}) {
        fs::create_directories(directory / camera_name);
    }
    RecordingOptions options;
    options.directory = directory;
    options.segment_seconds = 60;
    options.retention_seconds = 3600;
    options.sync_interval_seconds = 1;
    options.min_free_bytes = 1000;
    options.query_free_space = [&free_bytes](const fs::path&) -> std::expected<std::uint64_t, std::error_code> {
        return free_bytes.load();
    };
    return options;
}


// 카메라 폴더 안 파일 짝의 경로 앞부분(확장자 제외)
fs::path StemPath(const fs::path& directory, const char* camera_name, std::int64_t start_unix_ms,
                  int collision_number = 0) {
    return directory / camera_name / FormatRecordingFileStem(start_unix_ms, collision_number);
}


// 파일 짝이 둘 다 있는지 확인한다.
bool PairExists(const fs::path& stem_path) {
    return fs::exists(stem_path.string() + ".mp4") && fs::exists(stem_path.string() + ".idx");
}


// 파일 짝이 둘 다 없는지 확인한다.
bool PairMissing(const fs::path& stem_path) {
    return !fs::exists(stem_path.string() + ".mp4") && !fs::exists(stem_path.string() + ".idx");
}


// 알림이 지정한 폴더와 파일 이름의 삭제 알림인지 확인한다.
bool IsDeletion(const RecordingCatalogEvent& event, std::string_view camera_folder, const std::string& stem) {
    const auto* deleted = std::get_if<RecordedFileDeleted>(&event);
    return deleted && deleted->camera_folder == camera_folder && deleted->stem == stem;
}


// 작업 순서대로 파일 짝이 생기고, 색인 레코드가 moof로 시작하는 조각을 가리키며 CRC32가 맞는지 검사한다.
void TestWritesFragmentsAndIndex() {
    const TemporaryDirectory temporary;
    std::atomic<std::uint64_t> free_bytes = kPlentyOfSpace;
    RecordingFileStore store(MakeOptions(temporary.Path(), free_bytes), {}, {});

    store.Execute(MakeOpenTask("front", kBaseUnixMs + 123));
    for (std::uint32_t index = 0; index < 3; ++index) {
        store.Execute(MakeWriteTask("front", index * kOneSecond, kOneSecond));
    }
    store.Execute(MakeCloseTask("front"));

    const fs::path stem_path = StemPath(temporary.Path(), "front", kBaseUnixMs);
    Expect(PairExists(stem_path), "시작 시각 이름의 .mp4·.idx 생성");
    Expect(VerifyRecordingPair(stem_path) == 3, "레코드 3개가 moof 조각을 가리키고 CRC32가 맞음");
    const auto index = ReadIndex(stem_path.string() + ".idx");
    Expect(index && index->header.start_unix_ms == kBaseUnixMs + 123, "색인 헤더의 밀리초 시작 시각");
    Expect(index && index->records.size() == 3 && index->records[2].start_time == 2 * kOneSecond &&
               index->records[2].duration == kOneSecond,
           "레코드의 시작 시각과 길이");
}


// 파일을 닫으면 디스크의 색인과 같은 조각 목록을 담아 알리고, 조각 없이 닫은 파일은 알리지 않는지 검사한다.
void TestClosedFileNotification() {
    const TemporaryDirectory temporary;
    std::atomic<std::uint64_t> free_bytes = kPlentyOfSpace;
    std::vector<RecordingCatalogEvent> events;
    RecordingFileStore store(MakeOptions(temporary.Path(), free_bytes), {}, CollectEvents(events));

    store.Execute(MakeOpenTask("front", kBaseUnixMs + 123));
    for (std::uint32_t index = 0; index < 3; ++index) {
        store.Execute(MakeWriteTask("front", index * kOneSecond, kOneSecond));
    }
    Expect(events.empty(), "열린 파일은 알리지 않음");
    store.Execute(MakeCloseTask("front"));
    store.Execute(MakeOpenTask("back", kBaseUnixMs));
    store.Execute(MakeCloseTask("back"));

    Expect(events.size() == 1, "조각 없이 닫은 back 파일은 알리지 않아 알림은 하나");
    const auto* closed = events.empty() ? nullptr : std::get_if<RecordedFileClosed>(&events[0]);
    Expect(closed != nullptr, "닫기 알림");
    if (closed == nullptr) {
        return;
    }
    Expect(closed->camera_folder == "front" && closed->file.stem == FormatRecordingFileStem(kBaseUnixMs, 0) &&
               closed->file.start_unix_ms == kBaseUnixMs + 123 && closed->file.duration == 3 * kOneSecond,
           "닫기 알림의 폴더, 파일 이름, 시작 시각, 길이");
    Expect(FragmentsMatchIndex(closed->file.fragments, StemPath(temporary.Path(), "front", kBaseUnixMs)),
           "닫기 알림의 조각 목록은 디스크의 색인 레코드와 같음");
}


// 같은 이름의 파일이 있으면 번호를 붙이고, 조각 없이 닫은 파일은 남기지 않는지 검사한다.
void TestNameCollisionAndEmptyFile() {
    const TemporaryDirectory temporary;
    std::atomic<std::uint64_t> free_bytes = kPlentyOfSpace;
    RecordingFileStore store(MakeOptions(temporary.Path(), free_bytes), {}, {});

    for (int round = 0; round < 2; ++round) {
        store.Execute(MakeOpenTask("front", kBaseUnixMs));
        store.Execute(MakeWriteTask("front", 0, kOneSecond));
        store.Execute(MakeCloseTask("front"));
    }
    store.Execute(MakeOpenTask("back", kBaseUnixMs));
    store.Execute(MakeCloseTask("back"));

    Expect(VerifyRecordingPair(StemPath(temporary.Path(), "front", kBaseUnixMs)) == 1, "첫 파일");
    Expect(VerifyRecordingPair(StemPath(temporary.Path(), "front", kBaseUnixMs, 1)) == 1, "-1을 붙인 새 파일");
    Expect(PairMissing(StemPath(temporary.Path(), "back", kBaseUnixMs)), "조각 없이 닫은 파일은 지움");
}


// 보관 분량을 넘으면 가장 오래된 닫힌 파일이 지워지고, 끊긴 시간은 분량에 들지 않는지 검사한다.
void TestRetention() {
    const TemporaryDirectory temporary;
    std::atomic<std::uint64_t> free_bytes = kPlentyOfSpace;
    RecordingOptions options = MakeOptions(temporary.Path(), free_bytes);
    options.retention_seconds = 2;
    std::vector<RecordingCatalogEvent> events;
    RecordingFileStore store(std::move(options), {}, CollectEvents(events));

    // 파일마다 1초이고 서로 1시간씩 떨어져 있다. 그 사이 시간은 분량에 들지 않는다.
    const std::int64_t starts[] = {kBaseUnixMs, kBaseUnixMs + 3'600'000, kBaseUnixMs + 7'200'000};
    for (int index = 0; index < 2; ++index) {
        store.Execute(MakeOpenTask("front", starts[index]));
        store.Execute(MakeWriteTask("front", 0, kOneSecond));
        store.Execute(MakeCloseTask("front"));
    }
    Expect(PairExists(StemPath(temporary.Path(), "front", starts[0])), "분량 2초 이하이면 지우지 않음");

    store.Execute(MakeOpenTask("front", starts[2]));
    store.Execute(MakeWriteTask("front", 0, kOneSecond));
    store.Execute(MakeCloseTask("front"));
    Expect(PairMissing(StemPath(temporary.Path(), "front", starts[0])), "분량을 넘으면 가장 오래된 파일 짝 삭제");
    Expect(events.size() == 4 && std::holds_alternative<RecordedFileClosed>(events[2]) &&
               IsDeletion(events[3], "front", FormatRecordingFileStem(starts[0], 0)),
           "세 번째 파일의 닫기 알림 뒤에 가장 오래된 파일의 삭제 알림");
    Expect(PairExists(StemPath(temporary.Path(), "front", starts[1])) &&
               PairExists(StemPath(temporary.Path(), "front", starts[2])),
           "최근 파일은 남음");
}


// 여유 공간이 부족하면 모든 카메라 폴더(등록되지 않은 폴더 포함)에서 가장 오래된 단위 구간을 함께 지우는지 검사한다.
void TestEmergencyDeletion() {
    const TemporaryDirectory temporary;
    std::atomic<std::uint64_t> free_bytes = kPlentyOfSpace;

    // 10:00 구간에 front·back, 10:01 구간에 front 파일을 만든다.
    const std::int64_t front_first = kBaseUnixMs;
    const std::int64_t back_first = kBaseUnixMs + 5'000;
    const std::int64_t front_second = kBaseUnixMs + 60'000;
    {
        RecordingFileStore store(MakeOptions(temporary.Path(), free_bytes), {}, {});
        for (const auto& [camera_name, start] :
             {std::pair{"front", front_first}, std::pair{"back", back_first}, std::pair{"front", front_second}}) {
            store.Execute(MakeOpenTask(camera_name, start));
            store.Execute(MakeWriteTask(camera_name, 0, kOneSecond));
            store.Execute(MakeCloseTask(camera_name));
        }
    }

    // back은 이번 실행에 등록하지 않은 카메라로, 복구 결과 목록에만 있다.
    const RecordedFileCatalog catalog = {
        {"back", {RecordedFile{.stem = FormatRecordingFileStem(back_first, 0), .start_unix_ms = back_first,
                               .duration = kOneSecond, .fragments = {}}}},
        {"front",
         {RecordedFile{.stem = FormatRecordingFileStem(front_first, 0), .start_unix_ms = front_first,
                       .duration = kOneSecond, .fragments = {}},
          RecordedFile{.stem = FormatRecordingFileStem(front_second, 0), .start_unix_ms = front_second,
                       .duration = kOneSecond, .fragments = {}}}},
    };
    RecordingOptions options = MakeOptions(temporary.Path(), free_bytes);
    std::atomic<int> low_answers = 1;
    options.query_free_space = [&low_answers](const fs::path&) -> std::expected<std::uint64_t, std::error_code> {
        return low_answers.fetch_sub(1) > 0 ? 0 : kPlentyOfSpace;
    };
    std::vector<RecordingCatalogEvent> events;
    RecordingFileStore store(std::move(options), catalog, CollectEvents(events));
    store.Execute(MakeOpenTask("front", kBaseUnixMs + 120'000));

    Expect(PairMissing(StemPath(temporary.Path(), "front", front_first)), "가장 오래된 구간의 front 파일 삭제");
    Expect(PairMissing(StemPath(temporary.Path(), "back", back_first)), "같은 구간의 등록되지 않은 back 파일 삭제");
    Expect(PairExists(StemPath(temporary.Path(), "front", front_second)), "다음 구간 파일은 남음");
    Expect(PairExists(StemPath(temporary.Path(), "front", kBaseUnixMs + 120'000)), "삭제 뒤 새 파일을 엶");
    Expect(events.size() == 2 && IsDeletion(events[0], "back", FormatRecordingFileStem(back_first, 0)) &&
               IsDeletion(events[1], "front", FormatRecordingFileStem(front_first, 0)),
           "비상 삭제로 지운 파일마다 삭제 알림");
}


// 여유 공간이 계속 부족해도 열린 파일은 지우지 않는지 검사한다.
void TestOpenFileIsNotDeleted() {
    const TemporaryDirectory temporary;
    std::atomic<std::uint64_t> free_bytes = kPlentyOfSpace;
    RecordingFileStore store(MakeOptions(temporary.Path(), free_bytes), {}, {});

    store.Execute(MakeOpenTask("front", kBaseUnixMs));
    store.Execute(MakeWriteTask("front", 0, kOneSecond));

    free_bytes = 0;
    store.Execute(MakeOpenTask("back", kBaseUnixMs + 60'000));
    store.Execute(MakeWriteTask("back", 0, kOneSecond));
    store.Execute(MakeWriteTask("front", kOneSecond, kOneSecond));
    store.Execute(MakeCloseTask("front"));
    store.Execute(MakeCloseTask("back"));

    Expect(VerifyRecordingPair(StemPath(temporary.Path(), "front", kBaseUnixMs)) == 2, "열려 있던 파일은 남아 이어 씀");
    Expect(VerifyRecordingPair(StemPath(temporary.Path(), "back", kBaseUnixMs + 60'000)) == 1,
           "지울 파일이 없어도 새 파일을 엶");
}


}  // namespace


int main() {
    TestWritesFragmentsAndIndex();
    TestClosedFileNotification();
    TestNameCollisionAndEmptyFile();
    TestRetention();
    TestEmergencyDeletion();
    TestOpenFileIsNotDeleted();
    return vms::test::ExitCode();
}
