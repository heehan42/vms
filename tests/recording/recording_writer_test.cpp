#include "recording/recording_writer.h"

#include <cstdint>
#include <filesystem>
#include <latch>
#include <limits>
#include <utility>

#include "recording/recording_file_name.h"
#include "recording_test_support.h"
#include "test_support.h"

namespace fs = std::filesystem;

using vms::recording::FormatRecordingFileStem;
using vms::recording::RecordingOptions;
using vms::recording::RecordingWriter;
using vms::recording::test::MakeCloseTask;
using vms::recording::test::MakeOpenTask;
using vms::recording::test::MakeWriteTask;
using vms::recording::test::TemporaryDirectory;
using vms::recording::test::VerifyRecordingPair;
using vms::test::Expect;


namespace {


// 2026-11-04 10:00:05 UTC의 유닉스 밀리초
constexpr std::int64_t kBaseUnixMs = 1793786405LL * 1000;


// 1초(90kHz)
constexpr std::uint32_t kOneSecond = 90000;


// 테스트용 녹화 설정을 만들고 front 카메라 폴더를 준비한다.
RecordingOptions MakeOptions(const fs::path& directory) {
    fs::create_directories(directory / "front");
    RecordingOptions options;
    options.directory = directory;
    options.segment_seconds = 60;
    options.retention_seconds = 3600;
    options.sync_interval_seconds = 1;
    options.min_free_bytes = 0;
    options.query_free_space = [](const fs::path&) -> std::expected<std::uint64_t, std::error_code> {
        return std::numeric_limits<std::uint64_t>::max();
    };
    return options;
}


// front 폴더 안 파일 짝의 경로 앞부분(확장자 제외)
fs::path StemPath(const fs::path& directory, std::int64_t start_unix_ms) {
    return directory / "front" / FormatRecordingFileStem(start_unix_ms, 0);
}


// 녹화 스레드가 큐의 작업을 순서대로 실행해 파일 짝을 만드는지 검사한다.
void TestExecutesTasksInOrder() {
    const TemporaryDirectory temporary;
    RecordingWriter writer(MakeOptions(temporary.Path()), {});
    writer.Push(MakeOpenTask("front", kBaseUnixMs));
    for (std::uint32_t index = 0; index < 3; ++index) {
        writer.Push(MakeWriteTask("front", index * kOneSecond, kOneSecond));
    }
    writer.Push(MakeCloseTask("front"));
    writer.WaitUntilIdle();
    Expect(VerifyRecordingPair(StemPath(temporary.Path(), kBaseUnixMs)) == 3, "조각 3개가 기록됨");
}


// 쓰지 않은 바이트 합이 상한 이상이면 여유가 없다고 답하고, 쓰기를 마치면 다시 여유가 생기는지 검사한다.
void TestHasRoom() {
    const TemporaryDirectory temporary;
    RecordingOptions options = MakeOptions(temporary.Path());
    options.queue_byte_limit = 1000;

    // 녹화 스레드가 열기 작업의 여유 공간 조회에서 멈춰 있게 해 큐가 줄지 않게 한다.
    std::latch release_writer(1);
    options.query_free_space = [&release_writer](const fs::path&) -> std::expected<std::uint64_t, std::error_code> {
        release_writer.wait();
        return std::numeric_limits<std::uint64_t>::max();
    };
    RecordingWriter writer(std::move(options), {});

    writer.Push(MakeOpenTask("front", kBaseUnixMs));
    Expect(writer.HasRoom(), "열기 작업은 크기 0");
    writer.Push(MakeWriteTask("front", 0, kOneSecond, 300));
    Expect(writer.HasRoom(), "합 600은 상한 1000보다 작음");
    writer.Push(MakeWriteTask("front", kOneSecond, kOneSecond, 300));
    Expect(!writer.HasRoom(), "합 1200은 상한 이상");

    release_writer.count_down();
    writer.WaitUntilIdle();
    Expect(writer.HasRoom(), "쓰기를 마치면 다시 여유가 생김");
    writer.Push(MakeCloseTask("front"));
    writer.WaitUntilIdle();
    Expect(VerifyRecordingPair(StemPath(temporary.Path(), kBaseUnixMs)) == 2, "상한을 넘긴 조각도 거부하지 않고 기록");
}


// 소멸할 때 큐에 남은 작업을 모두 쓰고 파일을 닫는지 검사한다.
void TestDrainsQueueOnDestruction() {
    const TemporaryDirectory temporary;
    {
        RecordingWriter writer(MakeOptions(temporary.Path()), {});
        writer.Push(MakeOpenTask("front", kBaseUnixMs));
        for (std::uint32_t index = 0; index < 50; ++index) {
            writer.Push(MakeWriteTask("front", index * kOneSecond, kOneSecond, 10'000));
        }
    }
    Expect(VerifyRecordingPair(StemPath(temporary.Path(), kBaseUnixMs)) == 50, "남은 조각 50개를 모두 기록");
}


}  // namespace


int main() {
    TestExecutesTasksInOrder();
    TestHasRoom();
    TestDrainsQueueOnDestruction();
    return vms::test::ExitCode();
}
