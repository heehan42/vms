#include "playback/recording_playback_endpoints.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>

#include "playback/recording_library.h"
#include "test_support.h"

using vms::http::HttpEndpointResponse;
using vms::http::HttpFileResponse;
using vms::http::HttpGetResult;
using vms::http::HttpRequest;
using vms::http::HttpStatus;
using vms::playback::RecordingLibrary;
using vms::playback::RecordingPlaybackEndpoints;
using vms::recording::RecordedFile;
using vms::recording::RecordedFileCatalog;
using vms::recording::RecordedFragment;
using vms::test::Expect;


namespace {


// 2026-11-08 09:00:00 UTC의 유닉스 밀리초
constexpr std::int64_t kBaseUnixMs = 1794128400LL * 1000;


// 테스트의 녹화 디렉터리. 핸들러는 디스크를 읽지 않으므로 없는 경로여도 된다.
const std::filesystem::path kRecordingDirectory = "/var/vms/recordings";


// 조각 하나를 가진 닫힌 파일을 만든다.
RecordedFile MakeFile(std::string stem) {
    return RecordedFile{.stem = std::move(stem),
                        .start_unix_ms = kBaseUnixMs,
                        .duration = 90000,
                        .fragments = {RecordedFragment{.start_time = 0, .duration = 90000, .offset = 812, .size = 1000}}};
}


// front와 "정문 1" 폴더에 파일이 하나씩 있는 녹화 목록을 만든다.
RecordingLibrary MakeLibrary() {
    RecordedFileCatalog catalog;
    catalog["front"] = {MakeFile("20261108T090000Z")};
    catalog["\xEC\xA0\x95\xEB\xAC\xB8 1"] = {MakeFile("20261108T090000Z-1")};
    return RecordingLibrary(std::move(catalog));
}


// 요청 대상만 채운 GET 요청을 만든다.
HttpRequest MakeRequest(std::string target) {
    HttpRequest request;
    request.method = "GET";
    request.target = std::move(target);
    return request;
}


// 재생 목록 핸들러의 응답 상태가 기대값인지 확인한다.
void ExpectPlaylistStatus(const RecordingPlaybackEndpoints& endpoints, std::string target, HttpStatus status,
                          std::string_view description) {
    Expect(endpoints.HandlePlaylist(MakeRequest(std::move(target))).status == status, description);
}


// 녹화 파일 핸들러가 기대한 상태의 오류 응답을 돌려주는지 확인한다.
void ExpectFileError(const RecordingPlaybackEndpoints& endpoints, std::string target, HttpStatus status,
                     std::string_view description) {
    const HttpGetResult result = endpoints.HandleFile(MakeRequest(std::move(target)));
    const auto* response = std::get_if<HttpEndpointResponse>(&result);
    Expect(response != nullptr && response->status == status, description);
}


// 재생 목록 요청의 200 응답과 오류 응답을 검사한다.
void TestPlaylist() {
    const RecordingLibrary library = MakeLibrary();
    const RecordingPlaybackEndpoints endpoints(library, kRecordingDirectory);

    const HttpEndpointResponse response = endpoints.HandlePlaylist(MakeRequest("/api/recordings/playlist.m3u8?camera=front"));
    Expect(response.status == HttpStatus::kOk && response.content_type == "application/vnd.apple.mpegurl",
           "200과 Content-Type: application/vnd.apple.mpegurl");
    Expect(response.body.starts_with("#EXTM3U\n") &&
               response.body.contains("/api/recordings/file.mp4?camera=front&file=20261108T090000Z"),
           "front 폴더의 재생 목록 본문");

    const HttpEndpointResponse korean =
        endpoints.HandlePlaylist(MakeRequest("/api/recordings/playlist.m3u8?camera=%EC%A0%95%EB%AC%B8%201"));
    Expect(korean.status == HttpStatus::kOk && korean.body.contains("camera=%EC%A0%95%EB%AC%B8%201&file="),
           "인코딩된 한글 폴더 이름");

    ExpectPlaylistStatus(endpoints, "/api/recordings/playlist.m3u8", HttpStatus::kBadRequest, "camera 없음");
    ExpectPlaylistStatus(endpoints, "/api/recordings/playlist.m3u8?camera=front&camera=front", HttpStatus::kBadRequest,
                         "camera 중복");
    ExpectPlaylistStatus(endpoints, "/api/recordings/playlist.m3u8?camera=%zz", HttpStatus::kBadRequest,
                         "잘못된 퍼센트 인코딩");
    ExpectPlaylistStatus(endpoints, "/api/recordings/playlist.m3u8?camera=missing", HttpStatus::kNotFound, "없는 폴더");
    ExpectPlaylistStatus(endpoints, "/api/recordings/playlist.m3u8?camera=..", HttpStatus::kNotFound, "'..' 폴더");
}


// 녹화 파일 요청의 파일 응답과 오류 응답을 검사한다.
void TestFile() {
    const RecordingLibrary library = MakeLibrary();
    const RecordingPlaybackEndpoints endpoints(library, kRecordingDirectory);

    const HttpGetResult result =
        endpoints.HandleFile(MakeRequest("/api/recordings/file.mp4?camera=front&file=20261108T090000Z"));
    const auto* file = std::get_if<HttpFileResponse>(&result);
    Expect(file != nullptr && file->path == kRecordingDirectory / "front" / "20261108T090000Z.mp4" &&
               file->content_type == "video/mp4",
           "<녹화 디렉터리>/<폴더>/<파일>.mp4 경로와 video/mp4");

    const HttpGetResult korean = endpoints.HandleFile(
        MakeRequest("/api/recordings/file.mp4?camera=%EC%A0%95%EB%AC%B8%201&file=20261108T090000Z-1"));
    const auto* korean_file = std::get_if<HttpFileResponse>(&korean);
    Expect(korean_file != nullptr &&
               korean_file->path == kRecordingDirectory / "\xEC\xA0\x95\xEB\xAC\xB8 1" / "20261108T090000Z-1.mp4",
           "디코딩한 한글 폴더 이름의 경로");

    ExpectFileError(endpoints, "/api/recordings/file.mp4?camera=front", HttpStatus::kBadRequest, "file 없음");
    ExpectFileError(endpoints, "/api/recordings/file.mp4?file=20261108T090000Z", HttpStatus::kBadRequest, "camera 없음");
    ExpectFileError(endpoints, "/api/recordings/file.mp4?camera=front&file=a&file=b", HttpStatus::kBadRequest,
                    "file 중복");
    ExpectFileError(endpoints, "/api/recordings/file.mp4?camera=front&file=%G0", HttpStatus::kBadRequest,
                    "잘못된 퍼센트 인코딩");
    ExpectFileError(endpoints, "/api/recordings/file.mp4?camera=missing&file=20261108T090000Z", HttpStatus::kNotFound,
                    "없는 폴더");
    ExpectFileError(endpoints, "/api/recordings/file.mp4?camera=front&file=20261108T090100Z", HttpStatus::kNotFound,
                    "없는 파일");
    ExpectFileError(endpoints, "/api/recordings/file.mp4?camera=..&file=..%2F..%2Fetc%2Fpasswd", HttpStatus::kNotFound,
                    "경로처럼 보이는 값");
    ExpectFileError(endpoints, "/api/recordings/file.mp4?camera=front&file=..%2Ffront%2F20261108T090000Z",
                    HttpStatus::kNotFound, "'..'로 같은 파일을 가리키는 값");
}


}  // namespace


int main() {
    TestPlaylist();
    TestFile();
    return vms::test::ExitCode();
}
