#include "playback/source_list.h"

#include <stdlib.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "media/camera_list.h"
#include "media/video_library.h"
#include "playback/recording_library.h"
#include "test_support.h"

using vms::http::HttpStatus;
using vms::media::CameraList;
using vms::media::VideoLibrary;
using vms::playback::BuildSourceListResponse;
using vms::playback::RecordingLibrary;
using vms::recording::RecordedFile;
using vms::recording::RecordedFileCatalog;
using vms::recording::RecordedFileClosed;
using vms::recording::RecordedFragment;
using vms::test::Expect;

namespace fs = std::filesystem;
using namespace std::string_view_literals;


namespace {


// SPS, PPS, IDR 슬라이스로 이루어진 프레임 하나짜리 H.264 Annex-B 바이트. 0 바이트가 있으므로 sv 리터럴로
// 길이를 정한다.
constexpr std::string_view kSingleFrameVideo =
    "\x00\x00\x01\x67\x42\xC0\x1F\xDA"
    "\x00\x00\x01\x68\xCE\x3C\x80"
    "\x00\x00\x01\x65\x88\x84\x21\x10"sv;


// first_mb_in_slice가 0인 일반 슬라이스 하나로 이루어진 프레임의 Annex-B 바이트
constexpr std::string_view kNextFrame = "\x00\x00\x01\x41\x9A\x02\x03"sv;


// "시험 \"1\".h264"의 UTF-8 바이트. JSON 이스케이프 검사에 쓴다.
constexpr std::string_view kKoreanVideoId = "\xEC\x8B\x9C\xED\x97\x98 \"1\".h264";


// 2026-11-08 09:00:00 UTC의 유닉스 밀리초
constexpr std::int64_t kBaseUnixMs = 1794128400LL * 1000;


// 조각 하나를 가진 닫힌 파일을 만든다.
RecordedFile MakeFile() {
    return RecordedFile{.stem = "20261108T090000Z",
                        .start_unix_ms = kBaseUnixMs,
                        .duration = 90000,
                        .fragments = {RecordedFragment{.start_time = 0, .duration = 90000, .offset = 812, .size = 1000}}};
}


// 동영상 세 개(a.h264 2프레임, b.h264 1프레임, kKoreanVideoId 1프레임)가 있는 동영상 목록을 만든다.
//
// 반환값: 만든 목록. 임시 디렉터리를 만들거나 읽지 못하면 빈 optional
std::optional<VideoLibrary> LoadThreeVideos() {
    std::error_code error;
    std::string root_template = (fs::temp_directory_path(error) / "source_list_test.XXXXXX").string();
    if (error || ::mkdtemp(root_template.data()) == nullptr) {
        return std::nullopt;
    }
    const fs::path video_dir = root_template;
    std::ofstream(video_dir / "b.h264", std::ios::binary) << kSingleFrameVideo;
    std::ofstream(video_dir / "a.h264", std::ios::binary) << kSingleFrameVideo << kNextFrame;
    std::ofstream(video_dir / kKoreanVideoId, std::ios::binary) << kSingleFrameVideo;
    auto loaded = VideoLibrary::LoadDirectory(video_dir);
    fs::remove_all(video_dir, error);
    if (!loaded) {
        return std::nullopt;
    }
    return std::move(loaded->library);
}


// 파일과 카메라 원소의 순서, 종류, 프레임 수, 이스케이프를 검사한다.
void TestFilesAndCameras() {
    const std::optional<VideoLibrary> videos = LoadThreeVideos();
    if (!videos) {
        Expect(false, "동영상 목록 준비");
        return;
    }
    CameraList cameras;
    cameras.Add("front", "rtsp://127.0.0.1:8554/cam");
    cameras.Add("a.h264", "rtsp://127.0.0.1:9554/cam");
    const RecordingLibrary recordings;

    const auto response = BuildSourceListResponse(*videos, cameras, recordings);
    Expect(response.status == HttpStatus::kOk, "목록 200");
    Expect(response.content_type == "application/json", "목록 Content-Type");
    Expect(response.body == "{\"sources\":["
                            "{\"type\":\"file\",\"name\":\"a.h264\",\"frames\":2},"
                            "{\"type\":\"file\",\"name\":\"b.h264\",\"frames\":1},"
                            "{\"type\":\"file\",\"name\":\"\xEC\x8B\x9C\xED\x97\x98 \\\"1\\\".h264\",\"frames\":1},"
                            "{\"type\":\"camera\",\"name\":\"front\"},"
                            "{\"type\":\"camera\",\"name\":\"a.h264\"}"
                            "]}",
           "파일은 이름의 바이트 순서로 먼저, 카메라는 등록 순서로 뒤에 두고 카메라 원소에는 frames가 없음");
}


// "녹화가 있는 폴더" 시나리오: 녹화 원소를 카메라 뒤에 폴더 이름 순서로 두고, 파일 없는 폴더는 빼는지 검사한다.
void TestRecordingFolders() {
    const VideoLibrary videos;
    CameraList cameras;
    cameras.Add("front", "rtsp://127.0.0.1:8554/cam");
    RecordedFileCatalog catalog;
    catalog["front"] = {MakeFile()};
    catalog["back"] = {MakeFile()};
    catalog["empty"] = {};
    RecordingLibrary recordings(std::move(catalog));

    Expect(BuildSourceListResponse(videos, cameras, recordings).body ==
               "{\"sources\":["
               "{\"type\":\"camera\",\"name\":\"front\"},"
               "{\"type\":\"recording\",\"name\":\"back\"},"
               "{\"type\":\"recording\",\"name\":\"front\"}"
               "]}",
           "카메라 뒤에 폴더 이름 순서의 녹화 원소, 파일 없는 폴더는 없음");

    // "첫 파일이 닫힌 뒤" 시나리오: 닫기 알림을 반영한 뒤 응답에 새 폴더가 생긴다.
    recordings.Apply(RecordedFileClosed{.camera_folder = "cam", .file = MakeFile()});
    Expect(BuildSourceListResponse(videos, cameras, recordings).body.contains("{\"type\":\"recording\",\"name\":\"cam\"}"),
           "닫기 알림 뒤 녹화 원소가 생김");
}


// "녹화 폴더 이름" 시나리오: 따옴표가 든 한글 폴더 이름을 JSON 문자열로 이스케이프하는지 검사한다.
void TestRecordingFolderName() {
    const VideoLibrary videos;
    const CameraList cameras;
    RecordedFileCatalog catalog;
    catalog["\xEC\xA0\x95\xEB\xAC\xB8 \"1\""] = {MakeFile()};
    const RecordingLibrary recordings(std::move(catalog));

    Expect(BuildSourceListResponse(videos, cameras, recordings).body ==
               "{\"sources\":[{\"type\":\"recording\",\"name\":\"\xEC\xA0\x95\xEB\xAC\xB8 \\\"1\\\"\"}]}",
           "녹화 폴더 이름의 JSON 이스케이프");
}


// 소스 없이 실행하면 빈 배열인지 검사한다.
void TestEmpty() {
    const VideoLibrary videos;
    const CameraList cameras;
    const RecordingLibrary recordings;
    Expect(BuildSourceListResponse(videos, cameras, recordings).body == "{\"sources\":[]}", "소스 없이 실행하면 빈 배열");
}


}  // namespace


int main() {
    TestFilesAndCameras();
    TestRecordingFolders();
    TestRecordingFolderName();
    TestEmpty();
    return vms::test::ExitCode();
}
