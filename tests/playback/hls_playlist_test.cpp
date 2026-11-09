#include "playback/hls_playlist.h"

#include <cstdint>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "test_support.h"

using vms::playback::BuildHlsPlaylist;
using vms::playback::BuildRecordingFileUrl;
using vms::recording::RecordedFile;
using vms::recording::RecordedFragment;
using vms::test::Expect;


namespace {


// 2026-11-08 09:00:00.000 UTC의 유닉스 밀리초
constexpr std::int64_t kNineOClockUnixMs = 1794128400LL * 1000;


// 2026-11-08 09:20:00.000 UTC의 유닉스 밀리초
constexpr std::int64_t kNineTwentyUnixMs = kNineOClockUnixMs + 20 * 60 * 1000;


// 녹화 파일 URL에 쓰는 front 폴더 첫 파일의 URL
constexpr std::string_view kFirstFileUrl = "/api/recordings/file.mp4?camera=front&file=20261108T090000Z";


// 폴더 front의 첫 파일. 조각 두 개는 specs의 "조각 두 개" 시나리오 값이다.
RecordedFile MakeFirstFile() {
    return RecordedFile{.stem = "20261108T090000Z",
                        .start_unix_ms = kNineOClockUnixMs,
                        .duration = 179910,
                        .fragments = {
                            RecordedFragment{.start_time = 0, .duration = 90000, .offset = 812, .size = 250000},
                            RecordedFragment{.start_time = 90000, .duration = 89910, .offset = 250812, .size = 248000},
                        }};
}


// 폴더 front의 두 번째 파일. 조각 길이를 지정한다.
//
// fragment_duration: 조각 하나의 길이(1/90000초 단위)
RecordedFile MakeSecondFile(std::uint32_t fragment_duration) {
    return RecordedFile{
        .stem = "20261108T092000Z",
        .start_unix_ms = kNineTwentyUnixMs,
        .duration = fragment_duration,
        .fragments = {RecordedFragment{.start_time = 0, .duration = fragment_duration, .offset = 812, .size = 1000}}};
}


// 재생 목록 텍스트를 LF로 나눈 줄 목록. 마지막 LF 뒤의 빈 문자열은 넣지 않는다.
std::vector<std::string> SplitLines(std::string_view text) {
    std::vector<std::string> lines;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        lines.emplace_back(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
    }
    return lines;
}


// "재생 목록 머리" 시나리오: 머리 줄, 목표 길이 1과 2, 마지막 줄, LF 줄 끝을 검사한다.
void TestHeader() {
    const std::vector<RecordedFile> one_second_files = {MakeSecondFile(90000)};
    const std::string playlist = BuildHlsPlaylist("front", one_second_files);
    const std::vector<std::string> lines = SplitLines(playlist);
    Expect(lines.size() >= 6 && lines[0] == "#EXTM3U" && lines[1] == "#EXT-X-VERSION:7" &&
               lines[2] == "#EXT-X-PLAYLIST-TYPE:VOD" && lines[3] == "#EXT-X-INDEPENDENT-SEGMENTS" &&
               lines[4] == "#EXT-X-TARGETDURATION:1",
           "머리 줄과 1초 조각의 목표 길이 1");
    Expect(!lines.empty() && lines.back() == "#EXT-X-ENDLIST", "마지막 줄은 #EXT-X-ENDLIST");
    Expect(playlist.ends_with("#EXT-X-ENDLIST\n") && !playlist.contains('\r'), "줄은 LF로 끝남");

    const std::vector<RecordedFile> long_fragment_files = {MakeFirstFile(), MakeSecondFile(135000)};
    Expect(BuildHlsPlaylist("front", long_fragment_files).contains("\n#EXT-X-TARGETDURATION:2\n"),
           "1.5초 조각이 있으면 목표 길이 2");

    const std::vector<RecordedFile> short_files = {MakeSecondFile(30000)};
    Expect(BuildHlsPlaylist("front", short_files).contains("\n#EXT-X-TARGETDURATION:1\n"), "1초보다 짧아도 목표 길이 1");
}


// "재생 목록의 파일 구간"과 "재생 목록의 세그먼트" 시나리오: 파일 두 개의 줄 순서를 그대로 비교한다.
void TestFilesAndSegments() {
    const std::vector<RecordedFile> files = {MakeFirstFile(), MakeSecondFile(90000)};
    const std::vector<std::string> lines = SplitLines(BuildHlsPlaylist("front", files));
    const std::string second_url = "/api/recordings/file.mp4?camera=front&file=20261108T092000Z";
    const std::vector<std::string> expected = {
        "#EXTM3U",
        "#EXT-X-VERSION:7",
        "#EXT-X-PLAYLIST-TYPE:VOD",
        "#EXT-X-INDEPENDENT-SEGMENTS",
        "#EXT-X-TARGETDURATION:1",
        "#EXT-X-MAP:URI=\"" + std::string(kFirstFileUrl) + "\",BYTERANGE=\"812@0\"",
        "#EXT-X-PROGRAM-DATE-TIME:2026-11-08T09:00:00.000Z",
        "#EXTINF:1.000,",
        "#EXT-X-BYTERANGE:250000@812",
        std::string(kFirstFileUrl),
        "#EXTINF:0.999,",
        "#EXT-X-BYTERANGE:248000@250812",
        std::string(kFirstFileUrl),
        "#EXT-X-DISCONTINUITY",
        "#EXT-X-MAP:URI=\"" + second_url + "\",BYTERANGE=\"812@0\"",
        "#EXT-X-PROGRAM-DATE-TIME:2026-11-08T09:20:00.000Z",
        "#EXTINF:1.000,",
        "#EXT-X-BYTERANGE:1000@812",
        second_url,
        "#EXT-X-ENDLIST",
    };
    Expect(lines == expected, "파일 두 개의 재생 목록 줄 순서");
    if (lines != expected) {
        for (const std::string& line : lines) {
            std::println(stderr, "  {}", line);
        }
    }

    const std::vector<RecordedFile> one_file = {MakeFirstFile()};
    Expect(!BuildHlsPlaylist("front", one_file).contains("#EXT-X-DISCONTINUITY"), "파일 하나면 불연속 표시 없음");
}


// 밀리초가 있는 시작 시각을 소수점 아래 세 자리로 쓰는지 검사한다.
void TestProgramDateTimeMilliseconds() {
    RecordedFile file = MakeSecondFile(90000);
    file.start_unix_ms = kNineOClockUnixMs + 1234;
    const std::vector<RecordedFile> files = {file};
    Expect(BuildHlsPlaylist("front", files).contains("\n#EXT-X-PROGRAM-DATE-TIME:2026-11-08T09:00:01.234Z\n"),
           "밀리초 시작 시각");
}


// "녹화 파일 URL" 시나리오: 한글·공백 폴더 이름과 번호 붙은 파일 이름을 검사한다.
void TestRecordingFileUrl() {
    Expect(BuildRecordingFileUrl("\xEC\xA0\x95\xEB\xAC\xB8 1", "20261108T090000Z-1") ==
               "/api/recordings/file.mp4?camera=%EC%A0%95%EB%AC%B8%201&file=20261108T090000Z-1",
           "한글과 공백이 든 폴더 이름의 URL");
    Expect(BuildRecordingFileUrl("a\"b", "x") == "/api/recordings/file.mp4?camera=a%22b&file=x",
           "큰따옴표는 인코딩해 #EXT-X-MAP 속성 값을 끝내지 않음");
}


}  // namespace


int main() {
    TestHeader();
    TestFilesAndSegments();
    TestProgramDateTimeMilliseconds();
    TestRecordingFileUrl();
    return vms::test::ExitCode();
}
