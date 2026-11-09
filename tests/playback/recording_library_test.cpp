#include "playback/recording_library.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "test_support.h"

using vms::playback::RecordingLibrary;
using vms::recording::RecordedFile;
using vms::recording::RecordedFileCatalog;
using vms::recording::RecordedFileClosed;
using vms::recording::RecordedFileDeleted;
using vms::recording::RecordedFragment;
using vms::test::Expect;


namespace {


// 2026-11-08 09:00:00 UTC의 유닉스 밀리초
constexpr std::int64_t kBaseUnixMs = 1794128400LL * 1000;


// 조각 하나(1초, 812바이트 뒤 1000바이트)를 가진 닫힌 파일을 만든다.
//
// stem: 확장자를 뺀 파일 이름
// start_unix_ms: 파일 시작 시각(유닉스 밀리초)
RecordedFile MakeFile(std::string stem, std::int64_t start_unix_ms) {
    return RecordedFile{.stem = std::move(stem),
                        .start_unix_ms = start_unix_ms,
                        .duration = 90000,
                        .fragments = {RecordedFragment{.start_time = 0, .duration = 90000, .offset = 812, .size = 1000}}};
}


// 폴더의 파일 이름을 목록 순서대로 돌려준다. 폴더가 없으면 빈 목록이다.
std::vector<std::string> StemsOf(const RecordingLibrary& library, std::string_view camera_folder) {
    std::vector<std::string> stems;
    if (const auto* files = library.FindFolder(camera_folder)) {
        for (const RecordedFile& file : *files) {
            stems.push_back(file.stem);
        }
    }
    return stems;
}


// 복구 결과로 만들 때 UTF-8이 아닌 폴더와 파일 없는 폴더를 빼고, 조각 목록을 그대로 담는지 검사한다.
void TestBuildFromRecovery() {
    RecordedFileCatalog catalog;
    catalog["front"] = {MakeFile("a", kBaseUnixMs), MakeFile("b", kBaseUnixMs + 60'000)};
    catalog["empty"] = {};
    catalog["bad\xFF"] = {MakeFile("c", kBaseUnixMs)};
    const RecordingLibrary library(std::move(catalog));

    Expect(library.Folders().size() == 1 && library.Folders().contains("front"), "front 폴더만 남음");
    Expect(library.FindFolder("empty") == nullptr, "파일 없는 폴더는 없음");
    Expect(library.FindFolder("bad\xFF") == nullptr, "UTF-8이 아닌 폴더는 뺌");
    Expect((StemsOf(library, "front") == std::vector<std::string>{"a", "b"}), "파일 목록 순서");
    const RecordedFile* file = library.FindFile("front", "b");
    Expect(file && file->fragments.size() == 1 && file->fragments[0].offset == 812 && file->fragments[0].size == 1000,
           "조각 목록을 담음");
}


// 닫기 알림을 시작 시각 순서에 넣고, 새 폴더를 만드는지 검사한다.
void TestApplyClosed() {
    RecordedFileCatalog catalog;
    catalog["front"] = {MakeFile("a", kBaseUnixMs), MakeFile("c", kBaseUnixMs + 120'000)};
    RecordingLibrary library(std::move(catalog));

    library.Apply(RecordedFileClosed{.camera_folder = "front", .file = MakeFile("b", kBaseUnixMs + 60'000)});
    library.Apply(RecordedFileClosed{.camera_folder = "front", .file = MakeFile("d", kBaseUnixMs + 180'000)});
    library.Apply(RecordedFileClosed{.camera_folder = "back", .file = MakeFile("x", kBaseUnixMs)});

    Expect((StemsOf(library, "front") == std::vector<std::string>{"a", "b", "c", "d"}), "시작 시각 순서에 넣음");
    Expect((StemsOf(library, "back") == std::vector<std::string>{"x"}), "새 폴더를 만듦");
    Expect(library.FindFile("back", "x") != nullptr && library.FindFile("back", "x")->fragments.size() == 1,
           "알림의 조각 목록을 담음");
}


// 삭제 알림으로 파일을 빼고, 마지막 파일이 빠진 폴더를 지우며, 없는 파일의 알림은 무시하는지 검사한다.
void TestApplyDeleted() {
    RecordedFileCatalog catalog;
    catalog["front"] = {MakeFile("a", kBaseUnixMs), MakeFile("b", kBaseUnixMs + 60'000)};
    catalog["back"] = {MakeFile("x", kBaseUnixMs)};
    RecordingLibrary library(std::move(catalog));

    library.Apply(RecordedFileDeleted{.camera_folder = "front", .stem = "a"});
    Expect((StemsOf(library, "front") == std::vector<std::string>{"b"}), "지운 파일을 뺌");
    Expect(library.FindFile("front", "a") == nullptr, "지운 파일은 찾을 수 없음");

    library.Apply(RecordedFileDeleted{.camera_folder = "back", .stem = "x"});
    Expect(library.FindFolder("back") == nullptr && !library.Folders().contains("back"), "마지막 파일이 빠진 폴더를 지움");

    library.Apply(RecordedFileDeleted{.camera_folder = "front", .stem = "missing"});
    library.Apply(RecordedFileDeleted{.camera_folder = "missing", .stem = "b"});
    Expect((StemsOf(library, "front") == std::vector<std::string>{"b"}), "없는 파일과 폴더의 삭제 알림은 무시");
}


// 폴더 이름과 파일 이름으로 바이트 단위로 찾는지 검사한다.
void TestFind() {
    RecordedFileCatalog catalog;
    catalog["\xEC\xA0\x95\xEB\xAC\xB8 1"] = {MakeFile("20261108T090000Z", kBaseUnixMs)};
    const RecordingLibrary library(std::move(catalog));

    Expect(library.FindFile("\xEC\xA0\x95\xEB\xAC\xB8 1", "20261108T090000Z") != nullptr, "한글·공백 폴더의 파일");
    Expect(library.FindFile("\xEC\xA0\x95\xEB\xAC\xB8", "20261108T090000Z") == nullptr, "폴더 이름이 다르면 없음");
    Expect(library.FindFile("\xEC\xA0\x95\xEB\xAC\xB8 1", "20261108T090000Z-1") == nullptr, "파일 이름이 다르면 없음");
    Expect(library.FindFile("..", "..") == nullptr, "경로처럼 보이는 이름은 없음");
}


}  // namespace


int main() {
    TestBuildFromRecovery();
    TestApplyClosed();
    TestApplyDeleted();
    TestFind();
    return vms::test::ExitCode();
}
