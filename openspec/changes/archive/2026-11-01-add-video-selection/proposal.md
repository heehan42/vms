# Proposal

## Why

서버는 `--video-file`로 받은 파일 하나를 모든 세션에 보낸다. 그래서 프로젝트의 첫 목표인 "서버가 제공하는 동영상 파일을 웹 사이트에서 **선택**하고 WebRTC로 재생"에서 선택 부분이 빠져 있다. 이 change는 동영상 디렉터리의 파일 목록을 웹 페이지에 보여 주고, 사용자가 고른 파일을 그 연결에 보내 첫 목표를 끝낸다. 또한 이후 카메라 영상과 녹화 파일을 "소스를 골라 세션에 연결한다"는 같은 틀로 다룰 수 있게 한다.

## What Changes

- **BREAKING** `--video-file <파일>`을 없애고 `--video-dir <디렉터리>`로 바꾼다.
  - 서버는 시작할 때 디렉터리 바로 아래의 `.h264`·`.264` 일반 파일을 모두 읽어 동영상 목록을 만든다. 파일 이름이 동영상 ID이며, 목록은 이름순이다.
  - 검증에 실패한 파일(읽기 실패, 크기 초과, NAL 없음, 첫 프레임 조건 불만족, UTF-8이 아닌 이름)은 건너뛰고 경고를 남긴다. 쓸 수 있는 파일이 하나도 없으면 서버가 시작하지 않는다.
  - 읽는 파일 크기의 합계에 상한(256MiB)을 두고, 넘는 파일은 건너뛴다.
  - `--video-dir`을 주지 않으면 지금처럼 동영상 없이 실행한다.
  - `--video-fps`는 그대로이며, 모든 파일에 같은 값을 쓴다.
- 동영상 목록 API `GET /api/videos`를 추가한다. 응답은 `{"videos":[{"id":"...","frames":300}, ...]}` 형태의 JSON이다.
- HTTP 서버가 정적 파일 외의 GET 엔드포인트를 등록할 수 있게 한다.
- `POST /offer`가 쿼리 매개변수 `video=<ID>`를 받는다.
  - 값이 목록에 있으면 그 세션에 해당 동영상을 보낸다.
  - 목록에 없으면 `404 Not Found`, 퍼센트 인코딩이 잘못되었거나 `video`가 두 번 이상 오면 `400 Bad Request`로 응답한다.
  - `video`가 없으면 지금처럼 연결만 맺고 미디어는 보내지 않는다.
- 연결 확인 웹 페이지가 시작할 때 동영상 목록을 받아 `select` 요소의 `option` 요소로 보여 주고, 연결 버튼을 누르면 선택한 ID로 offer를 보낸다. 다른 영상을 보려면 다시 선택하고 연결 버튼을 누른다(연결을 새로 맺는다).
- README와 통합 테스트를 새 인자에 맞게 고친다.

범위 밖(이후 마일스톤):

- 연결을 유지한 채 영상 전환, 일시정지·탐색 같은 재생 제어
- 파일별 fps(SPS VUI 해석이나 MP4 컨테이너의 시간 정보)
- 서버 실행 중 디렉터리를 다시 읽어 목록 갱신, 하위 디렉터리
- 파일 SPS에 맞춘 answer 코덱 선택

## Capabilities

### New Capabilities

- `video-catalog`: 서버가 재생할 수 있는 동영상 목록을 HTTP로 알려 주는 동작. 이후 카메라와 녹화 파일도 같은 목록으로 제공된다.

### Modified Capabilities

- `video-file-source`: 파일 하나 대신 디렉터리의 파일들을 읽고, 잘못된 파일은 서버를 멈추지 않고 건너뛴다. `--video-file` 실행 설정을 `--video-dir`로 바꾼다.
- `http-server`: `/api/videos`를 GET 전용 엔드포인트로 두고, 메서드 오류 응답에 반영한다.
- `webrtc-signaling`: `POST /offer`의 `video` 쿼리 매개변수와 그 오류 응답, 웹 페이지에서 동영상을 고르는 `select` 요소를 추가한다.
- `rtp-video-streaming`: 송신 시작 조건을 "서버에 동영상 파일이 설정됨"에서 "세션에 동영상이 지정됨"으로 바꾼다.

## Impact

- 새 코드
  - `src/media/video_library.h/.cpp`: 디렉터리를 읽어 ID별 `H264File`을 소유하는 동영상 목록
  - `src/http/query_string.h/.cpp`: 요청 대상의 쿼리 매개변수 찾기와 퍼센트 디코딩
  - `src/core/json.h/.cpp`: JSON 문자열 이스케이프와 UTF-8 검사
  - 대응하는 `tests/`
- 바뀌는 코드
  - `src/http/http_server.h/.cpp`: GET 엔드포인트 표, `Allow` 판단
  - `src/webrtc/signaling.h/.cpp`: `video` 매개변수 처리, 목록 API 응답
  - `src/webrtc/ice_lite_agent.h/.cpp`: 에이전트 전체의 동영상 대신 세션별 동영상
  - `src/apps/web_server/main.cpp`: `--video-dir`
  - `web/index.html`, `web/app.js`, `README.md`, `tests/integration/http_server_test.sh`
- 의존성: 새 라이브러리는 없다.
- 호환성: `--video-file`로 실행하던 명령은 "알 수 없는 인자"로 시작하지 않는다. 파일 하나를 쓰려면 그 파일이 든 디렉터리를 `--video-dir`로 지정한다.
