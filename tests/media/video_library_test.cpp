#include "media/video_library.h"

#include <stdlib.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "test_support.h"

using vms::media::VideoLibrary;
using vms::media::VideoLibraryLimits;
using vms::test::Expect;

namespace fs = std::filesystem;
using namespace std::string_view_literals;


namespace {


// SPS, PPS, IDR 슬라이스로 이루어진 첫 프레임의 Annex-B 바이트. 0 바이트가 있으므로 sv 리터럴로 길이를 정한다.
constexpr std::string_view kFirstFrame =
    "\x00\x00\x01\x67\x42\xC0\x1F\xDA"
    "\x00\x00\x01\x68\xCE\x3C\x80"
    "\x00\x00\x01\x65\x88\x84\x21\x10"sv;


// kFirstFrame 중 SPS와 PPS가 차지하는 앞부분의 길이
constexpr std::size_t kParameterSetsSize = 15;


// first_mb_in_slice가 0인 일반 슬라이스 하나로 이루어진 프레임의 Annex-B 바이트
constexpr std::string_view kNextFrame = "\x00\x00\x01\x41\x9A\x02\x03"sv;


// 지정한 프레임 수의 H.264 Annex-B 파일을 만든다.
//
// path: 만들 파일 경로
// frame_count: 프레임 수. 1 이상이어야 한다.
// 반환값: 쓴 파일의 크기(바이트)
std::uintmax_t WriteVideo(const fs::path& path, int frame_count) {
    std::string bytes(kFirstFrame);
    for (int index = 1; index < frame_count; ++index) {
        bytes.append(kNextFrame);
    }
    std::ofstream(path, std::ios::binary) << bytes;
    return bytes.size();
}


// 파일을 만들고 내용을 쓴다.
//
// path: 만들 파일 경로
// content: 쓸 내용
void WriteText(const fs::path& path, std::string_view content) { std::ofstream(path) << content; }


// 테스트마다 쓰는 빈 하위 디렉터리를 만든다.
//
// root: 임시 루트 디렉터리
// name: 하위 디렉터리 이름
// 반환값: 만든 디렉터리 경로
fs::path MakeCaseDirectory(const fs::path& root, std::string_view name) {
    const fs::path directory = root / name;
    std::error_code error;
    fs::create_directories(directory, error);
    return directory;
}


// 경고 목록에 두 문구를 모두 담은 경고가 있는지 확인한다.
//
// warnings: 경고 목록
// first, second: 찾을 문구
// 반환값: 한 경고에 두 문구가 모두 있으면 true
bool HasWarning(const std::vector<std::string>& warnings, std::string_view first, std::string_view second) {
    for (const std::string& warning : warnings) {
        if (warning.find(first) != std::string::npos && warning.find(second) != std::string::npos) {
            return true;
        }
    }
    return false;
}


// 확장자로 대상을 고르고, 디렉터리를 무시하며, 이름의 바이트 순서로 정렬하는지 검사한다.
void TestSelectsAndSortsFiles(const fs::path& root) {
    const fs::path directory = MakeCaseDirectory(root, "select");
    WriteVideo(directory / "b.h264", 3);
    WriteVideo(directory / "A.264", 1);
    WriteVideo(directory / "c.mp4", 1);
    WriteVideo(directory / "upper.H264", 2);
    fs::create_directory(directory / "sub");
    fs::create_directory(directory / "dir.h264");

    const auto loaded = VideoLibrary::LoadDirectory(directory);
    if (!loaded) {
        Expect(false, "정상 디렉터리를 읽어야 한다");
        return;
    }
    const auto& entries = loaded->library.Entries();
    Expect(entries.size() == 3, "대상 파일은 .h264, .264, .H264 세 개");
    if (entries.size() == 3) {
        Expect(entries[0].id == "A.264" && entries[1].id == "b.h264" && entries[2].id == "upper.H264",
               "이름의 바이트 순서(대문자가 앞)");
        Expect(entries[0].file.Frames().size() == 1 && entries[1].file.Frames().size() == 3,
               "항목마다 자기 파일의 프레임");
    }
    Expect(loaded->warnings.empty(), "디렉터리와 다른 확장자는 경고 없이 무시");
}


// 잘못된 파일을 건너뛰고 경고를 남기는지 검사한다.
void TestSkipsInvalidFiles(const fs::path& root) {
    const fs::path directory = MakeCaseDirectory(root, "skip");
    WriteVideo(directory / "ok.h264", 2);
    WriteText(directory / "bad.h264", "this is not an h264 stream\n");
    // 첫 프레임에 IDR이 없도록 SPS, PPS 뒤에 일반 슬라이스를 쓴다.
    std::ofstream(directory / "no_idr.h264", std::ios::binary) << kFirstFrame.substr(0, kParameterSetsSize)
                                                               << kNextFrame;
    fs::create_symlink(directory / "missing_target.h264", directory / "gone.h264");
    WriteVideo(directory / "\xBD\xC3.h264", 1);

    const auto loaded = VideoLibrary::LoadDirectory(directory);
    if (!loaded) {
        Expect(false, "쓸 수 있는 파일이 하나라도 있으면 성공해야 한다");
        return;
    }
    const auto& entries = loaded->library.Entries();
    Expect(entries.size() == 1 && entries[0].id == "ok.h264", "목록에는 ok.h264만 남는다");
    Expect(loaded->warnings.size() == 4, "건너뛴 파일마다 경고 하나");
    Expect(HasWarning(loaded->warnings, "bad.h264", "NAL"), "NAL이 없는 파일 경고");
    Expect(HasWarning(loaded->warnings, "no_idr.h264", "IDR"), "첫 프레임 조건 경고");
    Expect(HasWarning(loaded->warnings, "gone.h264", "열 수 없음"), "끊어진 심볼릭 링크 경고");
    Expect(HasWarning(loaded->warnings, ".h264", "UTF-8"), "UTF-8이 아닌 이름 경고");
}


// 합계 상한을 넘게 만드는 파일부터 건너뛰는지 검사한다.
void TestTotalSizeLimit(const fs::path& root) {
    const fs::path directory = MakeCaseDirectory(root, "limit");
    const std::uintmax_t size = WriteVideo(directory / "1.h264", 2);
    WriteVideo(directory / "2.h264", 2);
    WriteVideo(directory / "3.h264", 2);

    const auto loaded = VideoLibrary::LoadDirectory(directory, VideoLibraryLimits{.max_total_bytes = size * 2 + 1});
    if (!loaded) {
        Expect(false, "상한 안의 파일로 시작해야 한다");
        return;
    }
    const auto& entries = loaded->library.Entries();
    Expect(entries.size() == 2 && entries[1].id == "2.h264", "앞의 두 파일만 넣는다");
    Expect(loaded->warnings.size() == 1 && HasWarning(loaded->warnings, "3.h264", "합계"), "세 번째 파일 경고");
}


// 목록을 만들 수 없는 경우 오류로 끝나는지 검사한다.
void TestLoadErrors(const fs::path& root) {
    const fs::path empty = MakeCaseDirectory(root, "empty");
    WriteVideo(empty / "note.txt", 1);
    Expect(!VideoLibrary::LoadDirectory(empty), "대상 파일이 없으면 오류");

    const fs::path all_bad = MakeCaseDirectory(root, "all_bad");
    WriteText(all_bad / "bad.h264", "text");
    const auto bad_result = VideoLibrary::LoadDirectory(all_bad);
    Expect(!bad_result && bad_result.error().find("쓸 수 있는 동영상이 없음") != std::string::npos,
           "모두 잘못된 파일이면 오류");

    WriteText(root / "plain.txt", "file");
    Expect(!VideoLibrary::LoadDirectory(root / "plain.txt"), "일반 파일은 디렉터리가 아님");
    Expect(!VideoLibrary::LoadDirectory(root / "missing"), "없는 디렉터리");
}


// ID로 항목을 찾는지 검사한다.
void TestFind(const fs::path& root) {
    const fs::path directory = MakeCaseDirectory(root, "find");
    WriteVideo(directory / "\xEC\x8B\x9C\xED\x97\x98.h264", 1);
    WriteVideo(directory / "a.h264", 1);

    const auto loaded = VideoLibrary::LoadDirectory(directory);
    if (!loaded) {
        Expect(false, "찾기 테스트 디렉터리를 읽어야 한다");
        return;
    }
    const VideoLibrary& library = loaded->library;
    const auto* found = library.Find("\xEC\x8B\x9C\xED\x97\x98.h264");
    Expect(found != nullptr && found == &library.Entries()[1], "한글 ID로 찾음");
    Expect(library.Find("a.h264") == &library.Entries()[0], "ASCII ID로 찾음");
    Expect(library.Find("A.h264") == nullptr, "대소문자가 다르면 없음");
    Expect(library.Find("../find/a.h264") == nullptr, "경로처럼 보이는 값은 없음");
    Expect(VideoLibrary().Find("a.h264") == nullptr && VideoLibrary().Entries().empty(), "빈 목록");
}


}  // namespace


int main() {
    std::error_code error;
    std::string root_template = (fs::temp_directory_path(error) / "video_library_test.XXXXXX").string();
    if (error || ::mkdtemp(root_template.data()) == nullptr) {
        Expect(false, "임시 디렉터리 생성");
        return vms::test::ExitCode();
    }
    const fs::path root = root_template;

    TestSelectsAndSortsFiles(root);
    TestSkipsInvalidFiles(root);
    TestTotalSizeLimit(root);
    TestLoadErrors(root);
    TestFind(root);

    fs::remove_all(root, error);
    return vms::test::ExitCode();
}
