#pragma once

#include "http/http_server.h"
#include "media/camera_list.h"
#include "media/video_library.h"
#include "playback/recording_library.h"


namespace vms::playback {


// GET /api/sources 응답(재생할 수 있는 소스 목록 JSON)을 만든다.
//
// video_library: 동영상 파일 목록. 동영상 없이 실행하면 빈 목록이다.
// camera_list: --camera로 등록한 카메라 목록. 카메라 없이 실행하면 빈 목록이다.
// recording_library: 재생할 수 있는 녹화가 있는 카메라 폴더 목록
// 반환값: 200, application/json, {"sources":[...]}. 원소 순서는 다음과 같다.
//   1. {"type":"file","name":"...","frames":N}: 동영상 목록 순서
//   2. {"type":"camera","name":"..."}: 등록 순서. 카메라의 연결 상태와 관계없이 모두 넣는다.
//   3. {"type":"recording","name":"<폴더 이름>"}: 폴더 이름의 바이트 순서. 닫힌 파일이 있는 폴더만 넣는다.
//   소스가 없으면 빈 배열이다. 이름은 JSON 문자열 규칙대로 이스케이프한다.
//
// 이벤트 루프 스레드에서 호출한다. recording_library는 녹화 알림으로 바뀌므로 요청마다 새로 만든다.
http::HttpEndpointResponse BuildSourceListResponse(const media::VideoLibrary& video_library,
                                                   const media::CameraList& camera_list,
                                                   const RecordingLibrary& recording_library);


}  // namespace vms::playback
