# video-catalog Specification

## Purpose
서버는 재생할 수 있는 소스의 목록을 HTTP로 알려 준다. 소스는 동영상 파일, 카메라, 카메라 폴더 하나의 녹화다. 웹 페이지는 이 목록으로 사용자에게 고를 소스를 보여 준다. 웹 페이지는 고른 소스의 이름으로 재생을 요청한다.

## Requirements

### Requirement: 동영상 목록 조회
서버는 `GET /api/sources` 요청에 소스 목록으로 응답해야 한다(SHALL).

- 응답 상태는 `200 OK`다.
- 응답의 `Content-Type`은 `application/json`이다.
- 응답 본문은 JSON 객체다. 이 객체는 `sources` 배열 하나를 가진다.
- `sources`의 원소는 소스 종류 문자열 `type`과 소스 이름 문자열 `name`을 가진다.
- `type`은 `file`, `camera`, `recording` 중 하나다. `file`은 동영상 파일이다. `camera`는 카메라다. `recording`은 `camera_folder` 하나의 녹화다. `camera_folder`는 녹화 디렉터리 아래의 카메라 폴더 이름이다.
- `file` 원소는 프레임 수 정수 `frames`도 가진다. 다른 종류의 원소에는 `frames`가 없다.
- 원소는 다음 순서로 온다.
  1. `file` 원소. 순서는 서버가 시작할 때 정한 동영상 목록 순서다.
  2. `camera` 원소. 순서는 `--camera`로 등록한 순서다.
  3. `recording` 원소. 순서는 `camera_folder`의 바이트 순서다.
- `camera` 원소는 카메라의 현재 연결 상태와 관계없이 항상 목록에 있다.
- 서버는 재생할 수 있는 닫힌 녹화 파일이 있는 `camera_folder`마다 `recording` 원소를 하나 만든다. `recording` 원소의 `name`은 `camera_folder`다.
- 소스 없이 실행했으면 `sources`는 빈 배열이다.
- 서버는 쿼리 문자열을 무시한다.

#### Scenario: 두 파일이 있는 디렉터리
- **WHEN** `b.h264`(300프레임)와 `a.h264`(150프레임)가 있는 디렉터리를 `--video-dir`로 지정해 서버를 실행한다. 클라이언트가 `GET /api/sources`를 요청한다
- **THEN** 서버는 `200 OK`로 응답한다. 응답 본문은 `{"sources":[{"type":"file","name":"a.h264","frames":150},{"type":"file","name":"b.h264","frames":300}]}`에 해당하는 JSON이다

#### Scenario: 동영상 없이 실행
- **WHEN** 녹화 디렉터리가 없는 위치에서 `--video-dir`과 `--camera` 없이 서버를 실행한다. 클라이언트가 `GET /api/sources`를 요청한다
- **THEN** 서버는 `200 OK`로 응답한다. 응답 본문은 `{"sources":[]}`에 해당하는 JSON이다

#### Scenario: 이전 목록 경로
- **WHEN** 웹 루트에 `api/videos` 파일이 없다. 클라이언트가 `GET /api/videos`를 요청한다
- **THEN** 서버는 `404 Not Found`로 응답한다

#### Scenario: 파일과 카메라
- **WHEN** `a.h264`(150프레임)가 있는 디렉터리와 `--camera front=... --camera a.h264=...`로 서버를 실행한다. 클라이언트가 `GET /api/sources`를 요청한다
- **THEN** 응답 본문은 `{"sources":[{"type":"file","name":"a.h264","frames":150},{"type":"camera","name":"front"},{"type":"camera","name":"a.h264"}]}`에 해당하는 JSON이다

#### Scenario: 연결되지 않은 카메라
- **WHEN** 꺼져 있는 카메라를 `--camera front=...`로 등록해 서버를 실행한다. 클라이언트가 `GET /api/sources`를 요청한다
- **THEN** 응답에 `{"type":"camera","name":"front"}` 원소가 있다

#### Scenario: 녹화가 있는 폴더
- **WHEN** 녹화 디렉터리의 `front` 폴더와 `back` 폴더에 닫힌 파일이 있다. `empty` 폴더에는 파일이 없다. `--camera front=...`로 서버를 실행한다. 클라이언트가 `GET /api/sources`를 요청한다
- **THEN** 응답 본문은 `{"sources":[{"type":"camera","name":"front"},{"type":"recording","name":"back"},{"type":"recording","name":"front"}]}`에 해당하는 JSON이다

#### Scenario: 첫 파일이 닫힌 뒤
- **WHEN** 녹화 디렉터리가 없는 상태에서 `--camera cam=...`로 서버를 실행한다. 카메라가 연결된다. 녹화 스레드가 첫 녹화 파일을 닫는다. 그 뒤에 클라이언트가 `GET /api/sources`를 요청한다
- **THEN** 응답에 `{"type":"recording","name":"cam"}` 원소가 있다. 첫 파일이 닫히기 전의 응답에는 이 원소가 없다

### Requirement: 동영상 이름 표기
서버는 소스 이름을 JSON 문자열 규칙대로 이스케이프해 응답해야 한다(SHALL).

- 서버는 `"`와 `\` 앞에 백슬래시를 붙인다.
- 서버는 U+0020 미만의 제어 문자를 `\u00XX`로 바꾼다.
- 서버는 그 밖의 UTF-8 문자를 그대로 쓴다.
- 클라이언트가 `file` 원소나 `camera` 원소의 `type`과 `name`을 그대로 `POST /offer`의 `type`과 `name` 값으로 쓰면, 그 값은 같은 소스를 가리켜야 한다.
- 클라이언트가 `recording` 원소의 `name`을 그대로 재생 목록 요청의 `camera` 값으로 쓰면, 그 값은 같은 `camera_folder`를 가리켜야 한다.

#### Scenario: 한글과 따옴표가 든 이름
- **WHEN** 이름이 `시험 "1".h264`인 파일이 목록에 있다
- **THEN** 응답의 해당 원소는 `"type":"file","name":"시험 \"1\".h264"`다. 브라우저가 `JSON.parse`로 읽은 `name`은 원래 파일 이름과 같다

#### Scenario: 녹화 폴더 이름
- **WHEN** 녹화 디렉터리에 닫힌 파일이 있는 `정문 "1"` 폴더가 있다. 브라우저가 응답에서 그 원소의 `name`을 읽는다. 브라우저가 그 `name`을 `encodeURIComponent`로 인코딩해 `GET /api/recordings/playlist.m3u8?camera=...`를 요청한다
- **THEN** 서버는 `정문 "1"` 폴더의 재생 목록으로 응답한다
