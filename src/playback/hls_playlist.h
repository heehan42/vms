#pragma once

#include <span>
#include <string>
#include <string_view>

#include "recording/recorded_file.h"


namespace vms::playback {


// 녹화 파일 요청 경로. 쿼리 매개변수 camera(폴더 이름)와 file(확장자를 뺀 파일 이름)로 파일을 가리킨다.
inline constexpr std::string_view kRecordingFilePath = "/api/recordings/file.mp4";


// 재생 목록 요청 경로. 쿼리 매개변수 camera(폴더 이름)로 폴더를 가리킨다.
inline constexpr std::string_view kRecordingPlaylistPath = "/api/recordings/playlist.m3u8";


// 녹화 파일 하나를 가리키는 URL을 만든다.
//
// camera_folder: 카메라 폴더 이름
// stem: 확장자를 뺀 녹화 파일 이름
// 반환값: "/api/recordings/file.mp4?camera=<폴더 이름>&file=<파일 이름>". 두 값은 http::EncodeQueryComponent로
//         퍼센트 인코딩한다. 그래서 결과에 큰따옴표와 줄바꿈이 없다.
std::string BuildRecordingFileUrl(std::string_view camera_folder, std::string_view stem);


// 폴더 하나의 닫힌 녹화 파일을 이은 HLS 버전 7 VOD 재생 목록 텍스트를 만든다.
//
// camera_folder: 카메라 폴더 이름. 녹화 파일 URL에 쓴다.
// files: 시작 시각 순서의 닫힌 파일. 파일마다 조각 목록이 하나 이상 있어야 한다.
// 반환값: 줄마다 LF로 끝나는 재생 목록 텍스트
//
// 조각 하나가 세그먼트 하나이며, 세그먼트는 녹화 파일의 바이트 범위(#EXT-X-BYTERANGE)로 가리킨다. 파일마다 첫
// 세그먼트 앞에 초기화 구간(#EXT-X-MAP, 0바이트부터 첫 조각 시작 위치까지)과 파일 시작 시각
// (#EXT-X-PROGRAM-DATE-TIME)을 두고, 두 번째 파일부터 그 앞에 #EXT-X-DISCONTINUITY를 둔다. 같은 입력에는 항상 같은
// 텍스트를 만든다(부동소수점을 쓰지 않는다). 디스크를 읽지 않는다.
std::string BuildHlsPlaylist(std::string_view camera_folder, std::span<const recording::RecordedFile> files);


}  // namespace vms::playback
