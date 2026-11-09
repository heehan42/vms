# recording-playback Specification

## Purpose
서버는 녹화 디렉터리에 남은 닫힌 녹화 파일을 HTTP로 제공한다. 브라우저는 카메라 폴더 하나의 녹화를 HLS 재생 목록으로 받아 재생한다.

## Requirements

### Requirement: 재생할 수 있는 녹화
서버는 닫힌 녹화 파일만 재생 대상으로 삼아야 한다(SHALL). 닫힌 녹화 파일은 녹화 스레드가 디스크와 동기화한 뒤 닫은 녹화 파일이다. 서버는 재생 대상을 이벤트 루프 스레드의 `RecordingLibrary`에 둔다. `RecordingLibrary`는 `camera_folder`별로 파일을 나눈다. `camera_folder`는 녹화 디렉터리 아래의 카메라 폴더 이름이다.

- 서버가 시작할 때 서버는 `RecoverRecordings()`가 남긴 파일을 `RecordingLibrary`에 넣는다. 등록하지 않은 카메라의 폴더도 넣는다.
- 녹화 중인 파일은 `RecordingLibrary`에 들어가지 않는다.
- 닫힌 파일이 없는 폴더는 `RecordingLibrary`에 없다.
- 이름이 UTF-8이 아닌 폴더는 `RecordingLibrary`에 없다.

#### Scenario: 시작할 때 남아 있던 녹화
- **WHEN** 녹화 디렉터리의 `old` 폴더에 닫힌 파일 두 개가 있고, `--camera` 없이 서버를 실행한다
- **THEN** `old` 폴더의 재생 목록에 두 파일이 모두 들어 있다

#### Scenario: 녹화 중인 파일
- **WHEN** 서버가 `cam` 카메라를 녹화하기 시작한다. 첫 파일이 닫히기 전에 클라이언트가 `GET /api/recordings/playlist.m3u8?camera=cam`을 요청한다
- **THEN** 서버는 `404 Not Found`로 응답한다

### Requirement: 재생할 수 있는 녹화의 갱신
서버는 녹화 스레드가 녹화 파일을 닫거나 지우면 `RecordingLibrary`를 고쳐야 한다(SHALL).

- 녹화 스레드가 녹화 파일을 닫으면 서버는 그 파일을 `RecordingLibrary`에 더한다.
- `ApplyRetention()`이 녹화 파일을 지우면 서버는 그 파일을 `RecordingLibrary`에서 뺀다. `ApplyRetention()`은 `--record-retention`초보다 오래된 파일을 지우는 함수다.
- `DeleteOldestSegment()`가 녹화 파일을 지우면 서버는 그 파일을 `RecordingLibrary`에서 뺀다. `DeleteOldestSegment()`는 디스크 여유 공간이 `--record-min-free`보다 작을 때 가장 오래된 파일을 지우는 함수다.

#### Scenario: 새로 닫힌 파일
- **WHEN** 서버가 `cam` 카메라를 녹화한다. `--record-segment`초가 지나 녹화 스레드가 녹화 파일 하나를 닫는다
- **THEN** 그 뒤에 요청한 `cam` 폴더의 재생 목록에 그 파일이 들어 있다

#### Scenario: `--record-retention`을 넘어 지운 파일
- **WHEN** `ApplyRetention()`이 `cam` 폴더의 가장 오래된 파일을 지운다
- **THEN** 그 뒤에 요청한 `cam` 폴더의 재생 목록에 그 파일이 없다. 그 파일의 녹화 파일 요청에 서버는 `404 Not Found`로 응답한다

### Requirement: 재생 목록 요청
서버는 `GET /api/recordings/playlist.m3u8` 요청에 폴더 하나의 재생 목록으로 응답해야 한다(SHALL). 쿼리 매개변수 `camera`의 값은 `camera_folder`다.

- 응답 상태는 `200 OK`다.
- 응답의 `Content-Type`은 `application/vnd.apple.mpegurl`이다.
- 쿼리 해석 규칙은 `POST /offer`의 쿼리 매개변수와 같다.
- `camera`가 없으면 서버는 `400 Bad Request`로 응답한다.
- `camera`가 두 번 이상 오면 서버는 `400 Bad Request`로 응답한다.
- `camera`의 퍼센트 인코딩이 잘못되었으면 서버는 `400 Bad Request`로 응답한다.
- `camera` 값과 바이트 단위로 같은 `camera_folder`가 `RecordingLibrary`에 없으면 서버는 `404 Not Found`로 응답한다.

#### Scenario: 녹화가 있는 폴더
- **WHEN** `front` 폴더에 닫힌 파일이 있다. 클라이언트가 `GET /api/recordings/playlist.m3u8?camera=front`를 요청한다
- **THEN** 서버는 `200 OK`와 `Content-Type: application/vnd.apple.mpegurl`로 응답한다. 응답 본문은 `#EXTM3U`로 시작한다

#### Scenario: 인코딩된 한글 폴더 이름
- **WHEN** `정문` 폴더에 닫힌 파일이 있다. 클라이언트가 `camera` 값을 `encodeURIComponent("정문")`으로 보낸다
- **THEN** 서버는 `정문` 폴더의 재생 목록으로 응답한다

#### Scenario: 없는 폴더
- **WHEN** 클라이언트가 `GET /api/recordings/playlist.m3u8?camera=missing`을 요청한다
- **THEN** 서버는 `404 Not Found`로 응답한다

#### Scenario: 잘못된 매개변수
- **WHEN** 클라이언트가 `camera` 없이 요청한다. 또는 `camera`를 두 번 넣어 요청한다. 또는 `camera=%zz`로 요청한다
- **THEN** 서버는 `400 Bad Request`로 응답한다

### Requirement: 재생 목록 머리
서버는 재생 목록을 HLS 버전 7의 VOD 재생 목록으로 만들어야 한다(SHALL).

- 첫 줄은 `#EXTM3U`다.
- 그 뒤에 `#EXT-X-VERSION:7`, `#EXT-X-PLAYLIST-TYPE:VOD`, `#EXT-X-INDEPENDENT-SEGMENTS`, `#EXT-X-TARGETDURATION:<초>`가 이 순서로 온다.
- `#EXT-X-TARGETDURATION`의 값은 재생 목록에 든 fragment의 `duration` 중 가장 긴 값이다. 이 값은 초 단위 정수로 올림한다. 올림한 값이 1보다 작으면 1을 쓴다. fragment는 녹화 파일 안의 `moof` 상자와 `mdat` 상자 한 쌍이다. `duration`은 fragment의 재생 시간이며 단위는 1/90000초다.
- 마지막 줄은 `#EXT-X-ENDLIST`다.
- 모든 줄은 LF(`\n`)로 끝난다.

#### Scenario: 1초 세그먼트만 있는 목록
- **WHEN** 폴더의 모든 fragment의 `duration`이 90000(1/90000초 단위)이다. 클라이언트가 그 폴더의 재생 목록을 받는다
- **THEN** 재생 목록에 `#EXT-X-TARGETDURATION:1`이 있다. 마지막 줄은 `#EXT-X-ENDLIST`다

#### Scenario: 길어진 fragment
- **WHEN** 패킷 손실 구간 때문에 `duration`이 1.5초(135000)로 늘어난 fragment가 폴더에 있다. 클라이언트가 그 폴더의 재생 목록을 받는다
- **THEN** 재생 목록에 `#EXT-X-TARGETDURATION:2`가 있다

### Requirement: 재생 목록의 파일 구간
서버는 폴더의 닫힌 파일을 `start_unix_ms` 순서로 재생 목록에 넣어야 한다(SHALL). `start_unix_ms`는 녹화 파일의 시작 시각이다. 단위는 밀리초이고 기준점은 1970-01-01T00:00:00Z다.

- 서버는 파일마다 첫 세그먼트 앞에 `#EXT-X-MAP`을 둔다. `#EXT-X-MAP`의 내용은 "재생 목록의 init segment" 요구사항이 정한다.
- 서버는 `#EXT-X-MAP` 바로 뒤에 `#EXT-X-PROGRAM-DATE-TIME`을 둔다.
- `#EXT-X-PROGRAM-DATE-TIME`의 값은 `start_unix_ms`를 `YYYY-MM-DDTHH:MM:SS.sssZ`(UTC) 형식으로 쓴 것이다.
- 두 번째 파일부터 서버는 `#EXT-X-MAP` 앞에 `#EXT-X-DISCONTINUITY`를 둔다.

#### Scenario: 파일 두 개
- **WHEN** `front` 폴더에 파일 `20261108T090000Z`와 `20261108T092000Z`가 있다. 두 파일의 `start_unix_ms`는 2026-11-08T09:00:00.000Z와 2026-11-08T09:20:00.000Z다. 두 파일의 첫 fragment `offset`은 812다
- **THEN** 재생 목록에는 다음 줄이 이 순서로 나온다. `#EXT-X-MAP:URI="/api/recordings/file.mp4?camera=front&file=20261108T090000Z",BYTERANGE="812@0"`, `#EXT-X-PROGRAM-DATE-TIME:2026-11-08T09:00:00.000Z`, 첫 파일의 세그먼트, `#EXT-X-DISCONTINUITY`, 두 번째 파일의 `#EXT-X-MAP`, `#EXT-X-PROGRAM-DATE-TIME:2026-11-08T09:20:00.000Z`, 두 번째 파일의 세그먼트

#### Scenario: 녹화가 없던 구간
- **WHEN** 위 두 파일 사이(09:01:00~09:20:00)에 녹화가 없다
- **THEN** 재생 목록에는 그 구간을 나타내는 줄이 없다. 첫 파일의 마지막 세그먼트 바로 다음에 `#EXT-X-DISCONTINUITY`가 온다. 그다음에 두 번째 파일의 `#EXT-X-MAP`이 온다

### Requirement: 재생 목록의 init segment
서버는 파일마다 그 파일의 init segment를 가리키는 `#EXT-X-MAP`을 써야 한다(SHALL). init segment는 녹화 파일 맨 앞의 `ftyp` 상자와 `moov` 상자다.

- `#EXT-X-MAP`의 `URI`는 그 파일의 녹화 파일 URL이다.
- `#EXT-X-MAP`의 `BYTERANGE`는 `<init segment 크기>@0`이다.
- init segment의 크기는 그 파일 첫 fragment의 `offset`과 같다. `offset`은 녹화 파일 안에서 fragment가 시작하는 위치(바이트, 녹화 파일 시작 기준)다.

#### Scenario: init segment 범위
- **WHEN** `front` 폴더 파일 `20261108T090000Z`의 첫 fragment `offset`이 812다
- **THEN** 재생 목록에 `#EXT-X-MAP:URI="/api/recordings/file.mp4?camera=front&file=20261108T090000Z",BYTERANGE="812@0"`이 있다

### Requirement: 재생 목록의 세그먼트
서버는 녹화 파일의 fragment 하나를 HLS 세그먼트 하나로 재생 목록에 넣어야 한다(SHALL).

- 서버는 세그먼트마다 세 줄을 쓴다. 세 줄은 `#EXTINF:<초>,`, `#EXT-X-BYTERANGE:<size>@<offset>`, 녹화 파일 URL이다. `size`는 fragment의 크기(바이트)다. `offset`은 녹화 파일 안에서 fragment가 시작하는 위치(바이트, 녹화 파일 시작 기준)다.
- `#EXTINF`의 초는 fragment의 `duration`을 90000으로 나눈 값이다. 이 값은 소수점 아래 세 자리로 반올림한다.
- 세그먼트 순서는 녹화 파일 안의 fragment 순서다.

#### Scenario: fragment 두 개
- **WHEN** `front` 폴더 파일 `20261108T090000Z`에 fragment 두 개가 있다. 두 fragment의 (`duration`, `offset`, `size`)는 (90000, 812, 250000)과 (89910, 250812, 248000)이다
- **THEN** 재생 목록에는 다음 줄이 이 순서로 나온다. `#EXTINF:1.000,`, `#EXT-X-BYTERANGE:250000@812`, `/api/recordings/file.mp4?camera=front&file=20261108T090000Z`, `#EXTINF:0.999,`, `#EXT-X-BYTERANGE:248000@250812`, 같은 URL

### Requirement: 녹화 파일 URL
서버는 재생 목록에 넣는 녹화 파일 URL을 `/api/recordings/file.mp4?camera=<camera_folder>&file=<stem>` 형식으로 만들어야 한다(SHALL). `stem`은 녹화 파일 이름에서 확장자 `.mp4`를 뺀 부분이다.

- 서버는 `camera_folder`와 `stem`을 퍼센트 인코딩한다.
- 서버는 UTF-8 바이트 중 영문자, 숫자, `-`, `.`, `_`, `~`를 그대로 둔다.
- 서버는 나머지 바이트를 대문자 16진수 `%XX`로 바꾼다.

#### Scenario: 한글과 공백이 든 폴더 이름
- **WHEN** `camera_folder`가 `정문 1`이다. `stem`이 `20261108T090000Z-1`이다
- **THEN** URL은 `/api/recordings/file.mp4?camera=%EC%A0%95%EB%AC%B8%201&file=20261108T090000Z-1`이다

### Requirement: 녹화 파일 요청
서버는 `GET /api/recordings/file.mp4` 요청이 가리키는 녹화 파일이 `RecordingLibrary`에 있으면, 그 파일을 파일 응답으로 보내야 한다(SHALL). 쿼리 매개변수 `camera`의 값은 `camera_folder`다. 쿼리 매개변수 `file`의 값은 `stem`이다.

- 응답의 `Content-Type`은 `video/mp4`다.
- `Range` 요청은 HTTP 서버의 파일 응답 규칙을 따른다.

#### Scenario: fragment 하나의 범위
- **WHEN** 재생 목록에 세그먼트 `#EXT-X-BYTERANGE:250000@812`가 있다. 클라이언트가 `Range: bytes=812-250811`로 그 녹화 파일을 요청한다
- **THEN** 서버는 `206 Partial Content`와 `Content-Type: video/mp4`로 응답한다. 응답 본문은 파일의 812번째 바이트부터 250000바이트다

### Requirement: 녹화 파일 요청의 오류 응답
서버는 `GET /api/recordings/file.mp4` 요청이 잘못되었거나 `RecordingLibrary`에 없는 파일을 가리키면 오류로 응답해야 한다(SHALL).

- 두 매개변수 중 하나라도 없으면 서버는 `400 Bad Request`로 응답한다.
- 매개변수가 두 번 이상 오면 서버는 `400 Bad Request`로 응답한다.
- 퍼센트 인코딩이 잘못되었으면 서버는 `400 Bad Request`로 응답한다.
- 파일이 `RecordingLibrary`에 없으면 서버는 디스크를 확인하지 않는다. 서버는 `404 Not Found`로 응답한다. 녹화 중인 파일, 지운 파일, 없는 이름이 이 경우에 해당한다.

#### Scenario: 녹화 중인 파일
- **WHEN** 클라이언트가 지금 녹화 중인 파일의 `stem`으로 요청한다
- **THEN** 서버는 `404 Not Found`로 응답한다

#### Scenario: 경로처럼 보이는 값
- **WHEN** 클라이언트가 `camera=..&file=..%2F..%2Fetc%2Fpasswd`로 요청한다
- **THEN** 서버는 `404 Not Found`로 응답한다. 서버는 녹화 디렉터리 밖의 파일을 읽지 않는다

#### Scenario: 매개변수 하나만 있음
- **WHEN** 클라이언트가 `GET /api/recordings/file.mp4?camera=front`를 요청한다
- **THEN** 서버는 `400 Bad Request`로 응답한다

### Requirement: 웹 페이지의 녹화 재생
사용자가 `type`이 `recording`인 소스를 고르고 연결 버튼을 누르면, 웹 페이지는 그 폴더의 재생 목록을 hls.js로 `<video>` 요소에 재생해야 한다(SHALL).

- 재생을 시작하기 전에 웹 페이지는 진행 중인 WebRTC 연결을 끝낸다.
- 재생을 시작하기 전에 웹 페이지는 진행 중인 녹화 재생을 끝낸다.
- 재생 목록 URL의 `camera` 값은 소스 이름을 `encodeURIComponent`로 인코딩한 값이다.
- 사용자가 `file`이나 `camera` 소스를 고르면 웹 페이지는 녹화 재생을 끝낸다. 웹 페이지는 재생 막대를 숨긴다.

#### Scenario: 녹화가 없던 구간 건너뛰기
- **WHEN** 두 파일 사이에 카메라가 꺼졌던 구간이 있다. 웹 페이지가 두 파일을 이어서 재생한다
- **THEN** 재생은 첫 파일 끝에서 멈추지 않는다. 재생은 두 번째 파일로 넘어간다. 표시되는 녹화 시각이 두 번째 파일의 `start_unix_ms`로 바뀐다


#### Scenario: 실시간 영상으로 바꾸기
- **WHEN** 웹 페이지가 녹화를 재생하고 있다. 사용자가 "카메라 · cam"을 골라 연결한다
- **THEN** 녹화 재생이 끝난다. 재생 막대가 사라진다. 웹 페이지는 WebRTC 연결로 받은 실시간 영상을 표시한다

### Requirement: 녹화 재생 화면
웹 페이지는 녹화를 재생하는 동안 재생 막대와 녹화 시각을 표시해야 한다(SHALL).

- 웹 페이지는 `<video>` 요소의 재생 막대(`controls`)를 보인다.
- 웹 페이지는 화면에 보이는 프레임의 녹화 시각을 표시한다. 녹화 시각은 `hls.playingDate`를 브라우저의 지역 시각으로 바꾼 값이다.
- hls.js가 `fatal`이 참인 `Hls.Events.ERROR`를 알리면 웹 페이지는 원인을 표시한다. 그다음 웹 페이지는 재생을 멈춘다.

#### Scenario: 녹화 재생
- **WHEN** 서버에 닫힌 녹화 파일이 있다. 사용자가 페이지에서 "녹화 · cam"을 골라 연결한다
- **THEN** `<video>` 요소에 녹화 영상이 처음부터 재생된다. 사용자는 재생 막대로 재생 위치를 옮길 수 있다. 화면에 그 위치의 녹화 시각이 표시된다

#### Scenario: 재생 중 지워진 파일
- **WHEN** 웹 페이지가 녹화를 재생하고 있다. 아직 받지 않은 세그먼트의 파일을 `ApplyRetention()`이 지운다
- **THEN** 웹 페이지가 그 세그먼트를 요청할 때 오류를 표시한다. 웹 페이지는 재생을 멈춘다

### Requirement: hls.js 제공
서버는 버전을 고정한 hls.js 배포 파일을 웹 루트의 정적 파일로 제공해야 한다(SHALL). 서버는 hls.js의 라이선스 파일도 제공한다. 웹 페이지는 외부 서버가 아니라 이 서버의 hls.js 파일을 불러온다.

#### Scenario: 외부 네트워크 없이 재생
- **WHEN** 브라우저가 인터넷에 연결되지 않은 네트워크에 있다. 사용자가 페이지를 열고 녹화를 재생한다
- **THEN** 페이지가 hls.js를 불러온다. 녹화가 재생된다
