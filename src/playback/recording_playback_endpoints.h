#pragma once

#include <filesystem>

#include "http/http_request.h"
#include "http/http_server.h"
#include "playback/recording_library.h"


namespace vms::playback {


// 녹화 재생에 쓰는 API 엔드포인트. GET /api/recordings/playlist.m3u8과 GET /api/recordings/file.mp4를 처리한다.
//
// 두 핸들러는 RecordingLibrary의 메모리 목록만 읽고 디스크를 읽지 않는다. 녹화 파일 본문은 HttpServer가 파일 읽기
// 스레드에서 읽는다. 녹화 목록은 참조만 하며 소유하지 않으므로, 목록은 이 객체보다 오래 살아 있어야 한다. 이 객체를
// 핸들러로 등록한 HttpServer는 이 객체보다 먼저 소멸해야 한다. 이벤트 루프 스레드에서만 호출한다.
class RecordingPlaybackEndpoints {
public:
    // 녹화 목록과 녹화 디렉터리로 엔드포인트를 만든다.
    //
    // library: 재생할 수 있는 녹화 파일 목록
    // recording_directory: 녹화 디렉터리. 녹화 파일 경로 <녹화 디렉터리>/<폴더 이름>/<파일 이름>.mp4를 만드는 데 쓴다.
    RecordingPlaybackEndpoints(const RecordingLibrary& library, std::filesystem::path recording_directory);


    // 재생 목록 요청 하나를 처리한다. HttpServer의 GET 핸들러로 등록해 쓴다.
    //
    // request: 파싱한 요청. 요청 대상의 camera 쿼리 매개변수가 폴더 이름이다.
    // 반환값: 처리 결과
    //   - 200, application/vnd.apple.mpegurl, 그 폴더의 HLS 재생 목록(BuildHlsPlaylist)
    //   - 400: camera가 없거나, 두 번 이상 오거나, 어느 매개변수에든 잘못된 퍼센트 인코딩이 있음(원인을 로그에 남김)
    //   - 404: 바이트 단위로 이름이 같고 재생할 녹화가 있는 폴더가 없음
    http::HttpEndpointResponse HandlePlaylist(const http::HttpRequest& request) const;


    // 녹화 파일 요청 하나를 처리한다. HttpServer의 GET 핸들러로 등록해 쓴다.
    //
    // request: 파싱한 요청. 요청 대상의 camera(폴더 이름)·file(확장자를 뺀 파일 이름) 쿼리 매개변수로 파일을 찾는다.
    // 반환값: 처리 결과
    //   - video/mp4 파일 응답(HttpFileResponse): 녹화 목록에 있는 파일. Range 요청은 HttpServer의 파일 응답 규칙을
    //     따르며, 그 사이에 파일이 지워져 열 수 없으면 HttpServer가 404로 응답한다.
    //   - 400: 두 매개변수 중 하나라도 없거나, 두 번 이상 오거나, 잘못된 퍼센트 인코딩이 있음(원인을 로그에 남김)
    //   - 404: 녹화 목록에 없는 파일(녹화 중인 파일, 지운 파일, 없는 이름). 디스크를 확인하지 않는다.
    //
    // 파일 경로는 요청 값이 아니라 녹화 목록에서 찾은 항목의 폴더 이름과 파일 이름으로 만든다. 그래서 ".." 같은 값은
    // 목록에서 찾지 못해 404가 되고, 녹화 디렉터리 밖의 파일을 가리키지 않는다.
    http::HttpGetResult HandleFile(const http::HttpRequest& request) const;


private:
    // 재생할 수 있는 녹화 파일 목록
    const RecordingLibrary& library_;


    // 녹화 디렉터리
    std::filesystem::path recording_directory_;
};


}  // namespace vms::playback
