#include "playback/recording_playback_endpoints.h"

#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>

#include "http/query_string.h"
#include "playback/hls_playlist.h"
#include "recording/recording_file_name.h"


namespace vms::playback {


namespace {


// 재생 목록 응답의 Content-Type
constexpr std::string_view kPlaylistContentType = "application/vnd.apple.mpegurl";


// 녹화 파일 응답의 Content-Type
constexpr std::string_view kRecordingFileContentType = "video/mp4";


// 꼭 있어야 하는 쿼리 매개변수 하나를 찾는다.
//
// target: 요청 대상 원문
// name: 매개변수 이름
// 반환값: 퍼센트 디코딩한 값. 없거나, 두 번 이상 오거나, 잘못된 퍼센트 인코딩이 있으면 빈 optional(원인을 로그에 남김)
std::optional<std::string> FindRequiredParameter(std::string_view target, std::string_view name) {
    auto value = http::FindQueryParameter(target, name);
    if (!value) {
        std::println(stderr, "녹화 재생 요청 거부: {} 매개변수 해석 실패 ({}): {}", name, target,
                     value.error() == http::QueryParameterError::kDuplicate ? "두 번 이상 옴" : "잘못된 퍼센트 인코딩");
        return std::nullopt;
    }
    if (!*value) {
        std::println(stderr, "녹화 재생 요청 거부: {} 매개변수 없음 ({})", name, target);
        return std::nullopt;
    }
    return **std::move(value);
}


}  // namespace


RecordingPlaybackEndpoints::RecordingPlaybackEndpoints(const RecordingLibrary& library,
                                                       std::filesystem::path recording_directory)
    : library_(library), recording_directory_(std::move(recording_directory)) {}


http::HttpEndpointResponse RecordingPlaybackEndpoints::HandlePlaylist(const http::HttpRequest& request) const {
    const std::optional<std::string> camera_folder = FindRequiredParameter(request.target, "camera");
    if (!camera_folder) {
        return http::HttpEndpointResponse::Error(http::HttpStatus::kBadRequest);
    }

    const auto* files = library_.FindFolder(*camera_folder);
    if (files == nullptr) {
        return http::HttpEndpointResponse::Error(http::HttpStatus::kNotFound);
    }
    return http::HttpEndpointResponse{.status = http::HttpStatus::kOk,
                                      .content_type = std::string(kPlaylistContentType),
                                      .body = BuildHlsPlaylist(*camera_folder, *files)};
}


http::HttpGetResult RecordingPlaybackEndpoints::HandleFile(const http::HttpRequest& request) const {
    // 두 매개변수를 모두 해석한 뒤 판정해, camera가 없을 때도 file의 해석 오류를 같은 400으로 본다.
    const std::optional<std::string> camera_folder = FindRequiredParameter(request.target, "camera");
    const std::optional<std::string> stem = FindRequiredParameter(request.target, "file");
    if (!camera_folder || !stem) {
        return http::HttpEndpointResponse::Error(http::HttpStatus::kBadRequest);
    }

    if (library_.FindFile(*camera_folder, *stem) == nullptr) {
        return http::HttpEndpointResponse::Error(http::HttpStatus::kNotFound);
    }
    std::filesystem::path path = recording_directory_ / *camera_folder / *stem;
    path += recording::kRecordingVideoExtension;
    return http::HttpFileResponse{.path = std::move(path), .content_type = std::string(kRecordingFileContentType)};
}


}  // namespace vms::playback
