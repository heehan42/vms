# Proposal

## Why

서버는 등록한 카메라의 영상을 [녹화 파일](background.md#녹화-파일)로 저장한다(`camera-recording`). 하지만 저장한 영상을 웹 사이트에서 볼 방법이 없다. 지금은 사용자가 `ffplay`로 녹화 파일을 하나씩 직접 열어야 한다. 프로젝트 목표의 다음 단계는 저장한 카메라 영상을 웹 사이트에서 다시 재생하는 것이다.

녹화 파일 형식과 [색인 파일](background.md#색인-파일)은 HLS로 재생하는 것을 전제로 정했다.

- 색인 파일의 [`RecordingIndexRecord`](background.md#recordingindexrecord)는 [fragment](background.md#fragment)마다 위치와 재생 시간을 담는다.
- HTTP 서버는 파일 일부를 [`Range` 요청](background.md#range-요청)으로 보낼 수 있다(`http-server`).

이 change는 이 두 기능 위에 재생 목록과 웹 재생을 더한다.

## What Changes

### 서버의 녹화 목록

- 서버는 [이벤트 루프 스레드](background.md#이벤트-루프-스레드)의 [`RecordingLibrary`](background.md#recordinglibrary)에 닫힌 파일 목록을 둔다.
- 목록은 [`camera_folder`](background.md#camera_folder)별로 나뉜다.
- 목록의 [`RecordedFile`](background.md#recordedfile)은 `fragments`를 채운 값이다.
- 서버가 시작할 때 `main.cpp`는 [`RecoverRecordings()`](background.md#recoverrecordings)의 결과로 `RecordingLibrary`를 채운다. 등록하지 않은 카메라의 폴더도 `RecordingLibrary`에 들어간다.
- [녹화 스레드](background.md#녹화-스레드)는 파일을 닫거나 지울 때 [`RecordingCatalogEvent`](background.md#recordingcatalogevent)를 [`RecordingCatalogListener`](background.md#recordingcataloglistener)로 알린다.
- 이벤트 루프 스레드는 `RecordingLibrary::Apply()`로 그 이벤트를 `RecordingLibrary`에 반영한다.
- 녹화 중인 파일은 `RecordingLibrary`에 들어가지 않는다.

### 소스 목록

- `GET /api/sources`의 응답에 `type`이 `recording`인 원소를 더한다.
- 서버는 `RecordingLibrary`에 있는 `camera_folder`마다 `recording` 원소를 하나 만든다.
- `recording` 원소의 `name`은 `camera_folder`다.
- `recording` 원소는 `camera` 원소 뒤에 온다. `recording` 원소끼리는 `camera_folder`의 바이트 순서로 정렬한다.

### 재생 목록 요청

`GET /api/recordings/playlist.m3u8?camera=<camera_folder>`는 HLS [VOD 재생 목록](background.md#vod-재생-목록)으로 응답한다.

- 재생 목록은 그 폴더의 닫힌 파일을 모두 담는다. 파일 순서는 `start_unix_ms` 오름차순이다.
- fragment 하나가 HLS [세그먼트](background.md#세그먼트) 하나다.
- 재생 목록은 세그먼트마다 녹화 파일 URL과 바이트 범위(`EXT-X-BYTERANGE`)를 쓴다. 바이트 범위는 녹화 파일 안에서 fragment가 차지하는 범위다.
- 재생 목록은 파일마다 [init segment](background.md#init-segment) 위치(`EXT-X-MAP`)와 파일 시작 시각(`EXT-X-PROGRAM-DATE-TIME`)을 쓴다.
- 재생 목록은 파일이 바뀌는 곳에 `EXT-X-DISCONTINUITY`를 쓴다.
- 카메라가 꺼져 녹화가 없던 구간은 재생 목록에 들어가지 않는다. 그래서 재생 위치는 그 구간을 건너뛰어 다음 파일로 이어진다.

### 녹화 파일 요청

`GET /api/recordings/file.mp4?camera=<camera_folder>&file=<stem>`은 녹화 파일을 `video/mp4` 파일 응답으로 보낸다. [`stem`](background.md#stem)은 확장자를 뺀 파일 이름이다.

- 서버는 `RecordingLibrary`에 있는 파일만 보낸다.
- `Range` 요청은 기존 파일 응답 규칙을 따른다.
- `RecordingLibrary`에 없는 파일의 요청에는 `404 Not Found`로 응답한다.

### 웹 페이지

- 웹 페이지는 고른 소스의 `type`으로 재생 방식을 정한다.
- `type`이 `file`이나 `camera`이면 웹 페이지는 지금처럼 WebRTC로 재생한다.
- `type`이 `recording`이면 웹 페이지는 재생 목록을 [hls.js](background.md#hlsjs)로 재생한다.
- 녹화를 재생하는 동안 웹 페이지는 `<video>` 요소의 재생 막대를 보인다.
- 녹화를 재생하는 동안 웹 페이지는 화면에 보이는 프레임의 녹화 시각을 표시한다. 녹화 시각은 UTC 시각을 브라우저의 지역 시각으로 바꾼 값이다.
- 세그먼트 요청이 실패하면 웹 페이지는 오류를 표시한다. 그다음 웹 페이지는 재생을 멈춘다. 예를 들어 `--record-retention`초가 지나 파일이 지워지면 세그먼트 요청이 실패한다.

### 배포 파일과 문서

- 서버는 hls.js 배포 파일과 라이선스 파일을 웹 루트에서 직접 제공한다. hls.js 버전은 하나로 고정한다.
- README에 녹화 재생 방법을 적는다.

### 범위 밖

- 녹화 중인 파일의 재생
- 녹화가 진행되면서 항목이 늘어나는 재생 목록(`EXT-X-PLAYLIST-TYPE:EVENT`)
- 녹화 구간 목록 API와 웹 타임라인
- 녹화가 없던 구간을 검은 화면으로 보여 주는 `EXT-X-GAP`
- 시간 범위를 지정한 재생 목록
- 여러 fragment를 HLS 세그먼트 하나로 묶는 기능
- HTTP keep-alive
- 재생 중에 재생 목록을 다시 받아 지워진 파일을 피하는 기능

## Capabilities

### New Capabilities

- `recording-playback`: 닫힌 파일을 HTTP로 재생하게 하는 동작이다. 이 capability는 다음을 다룬다.
  - 재생할 수 있는 녹화의 범위와 그 범위가 바뀌는 시점
  - HLS 재생 목록의 내용
  - 녹화 파일 응답
  - 웹 페이지의 녹화 재생

### Modified Capabilities

- `video-catalog`
  - "동영상 목록 조회"에 `recording` 원소를 더한다.
  - "동영상 이름 표기"를 고친다. `type`과 `name`을 그대로 `POST /offer`에 쓸 수 있는 종류는 `file`과 `camera`뿐이다. `recording` 원소의 `name`은 재생 목록 요청의 `camera` 값으로 쓴다.
- `webrtc-signaling`
  - "연결 상태 확인 웹 페이지"에 `recording`의 종류 이름 "녹화"를 더한다.
  - 같은 요구사항에 분기를 더한다. 사용자가 `recording` 소스를 고르면 웹 페이지는 offer를 보내지 않는다. 대신 웹 페이지는 녹화 재생을 시작한다.

## Impact

### 새 코드

- `src/playback/`: `RecordingLibrary`, [`BuildHlsPlaylist()`](background.md#buildhlsplaylist), [`RecordingPlaybackEndpoints`](background.md#recordingplaybackendpoints), [`BuildSourceListResponse()`](background.md#buildsourcelistresponse)
- `src/recording/recording_catalog_event.h`: `RecordingCatalogEvent`와 `RecordingCatalogListener`
- `tests/playback/`: 단위 테스트
- `tests/integration/`: 재생 목록 요청과 녹화 파일 요청의 시나리오
- `web/vendor/hls.js-<버전>/`: hls.js 배포 파일과 라이선스 파일

### 바뀌는 코드

- `src/recording/recorded_file.h`: `RecordedFile`에 `fragments`를 더한다.
- `src/recording/recording_recovery.*`: `RecoverRecordings()`가 `fragments`를 채운다.
- `src/recording/recording_file_store.*`, `src/recording/recording_writer.*`: 생성자가 `RecordingCatalogListener`를 받는다. 녹화 스레드는 파일을 닫거나 지울 때 그 함수를 부른다.
- `src/webrtc/signaling.*`: `Signaling::HandleSourceList()`를 지운다. 소스 목록 JSON은 `src/playback/`의 `BuildSourceListResponse()`가 만든다.
- `src/apps/web_server/main.cpp`
  - `EventLoop`를 `RecordingWriter`보다 먼저 선언한다.
  - `RecordingCatalogListener`가 이벤트를 [`EventLoop::Post()`](background.md#eventlooppost)로 넘긴다.
  - 새 엔드포인트를 등록한다.
- `web/index.html`, `web/app.js`: 종류별 재생 분기, hls.js 재생, 녹화 시각 표시, 오류 표시
- `README.md`, `CMakeLists.txt`

### 의존성과 호환성

- 브라우저 쪽에 hls.js(Apache License 2.0)가 더해진다. 서버의 빌드 의존성은 바뀌지 않는다.
- `/api/sources` 응답에 새 종류의 원소가 생긴다.
- `type=recording`인 `POST /offer`에는 지금처럼 `400 Bad Request`로 응답한다. 서버는 `recording`을 알 수 없는 종류로 처리한다.
