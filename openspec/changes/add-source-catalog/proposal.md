# Proposal

## Why

다음 change(`add-rtsp-camera-source`)에서 RTSP 카메라를 파일과 같은 목록에 넣어 웹 페이지에서 고르게 할 예정이다. 그런데 지금 목록과 offer 요청은 "동영상 = 파일 이름"을 전제로 한다. 그래서 카메라 ID가 파일 이름과 겹치면 구분할 수 없고, 프레임 수처럼 파일에만 있는 값이 필수 필드로 되어 있다. 카메라 기능을 넣기 전에, 소스 종류와 ID를 함께 쓰는 형식으로 목록과 요청을 바꿔 둔다. 이 change는 파일 소스만으로 검증한다.

## What Changes

- **BREAKING** 목록 API를 `GET /api/videos`에서 `GET /api/sources`로 바꾼다.
  - 응답은 `{"sources":[{"type":"file","id":"a.h264","frames":300}, ...]}`이다.
  - `type`은 소스 종류(`file` 또는 `camera`)이고, `frames`는 `file`에만 있다.
  - 이 change에는 카메라가 없으므로 실제 목록에는 `file`만 나온다.
  - `/api/videos`는 더 이상 엔드포인트가 아니다.
- **BREAKING** `POST /offer`의 소스 선택을 `video=<ID>`에서 `type=<종류>&id=<ID>`로 바꾼다.
  - 둘 다 없으면 지금처럼 미디어 없는 세션을 만든다.
  - 하나만 있거나, 퍼센트 인코딩이 잘못되었거나, 같은 이름이 두 번 오거나, `type`이 `file`·`camera`가 아니면 `400 Bad Request`로 응답한다.
  - 종류와 ID가 맞는 소스가 없으면 `404 Not Found`로 응답한다. 카메라는 아직 없으므로 `type=camera`는 항상 404다.
  - `video` 매개변수는 다른 이름의 매개변수처럼 무시한다.
- 웹 페이지가 `/api/sources`로 목록을 받고, `option`에 종류와 ID를 표시하며, 고른 소스의 `type`과 `id`로 offer를 보낸다.
- 시그널링 단위 테스트, 통합 테스트, README를 새 형식에 맞게 고친다.

범위 밖(`add-rtsp-camera-source`에서 다룸):

- `--camera` 인자, RTSP 수신, 카메라 소스 송신
- 카메라 연결 상태를 목록에 표시하는 것

## Capabilities

### New Capabilities

(없음)

### Modified Capabilities

- `video-catalog`: 목록 경로와 JSON 형식을 `/api/sources`, `{type, id, frames?}`로 바꾸고, ID 표기 규칙을 새 형식에 맞춘다.
- `webrtc-signaling`: offer의 소스 선택 매개변수를 `type`·`id`로 바꾸고 오류 규칙을 고친다. 웹 페이지가 새 목록과 매개변수를 쓰도록 바꾼다.
- `http-server`: GET 전용 엔드포인트 경로를 `/api/videos`에서 `/api/sources`로 바꾼다.

## Impact

- 바뀌는 코드
  - `src/webrtc/signaling.h/.cpp`: `type`·`id` 해석, 목록 JSON, 목록 핸들러 이름
  - `src/apps/web_server/main.cpp`: GET 엔드포인트 경로
  - `web/index.html`, `web/app.js`
  - `tests/webrtc/signaling_test.cpp`, `tests/integration/http_server_test.sh`, `README.md`
- 의존성: 새 라이브러리는 없다.
- 호환성
  - `/api/videos`를 요청하면 정적 파일 처리로 넘어가 `404`가 된다.
  - `?video=`로 offer를 보내는 기존 클라이언트는 오류 없이 미디어 없는 세션을 받는다. 웹 페이지는 같은 서버가 제공하므로 함께 바뀐다.
