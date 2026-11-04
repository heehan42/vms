# Spec Delta

## MODIFIED Requirements

### Requirement: 동영상 목록 조회
서버는 `GET /api/sources` 요청에 `200 OK`와 `Content-Type: application/json`으로 소스 목록을 응답해야 한다(SHALL). 본문은 `sources` 배열 하나를 가진 JSON 객체이며, 각 원소는 소스 종류 문자열 `type`과 소스 이름 문자열 `name`을 가진다. `type`은 `file`(동영상 파일) 또는 `camera`(카메라)다. `file` 원소는 프레임 수 정수 `frames`도 가지며, 다른 종류의 원소에는 `frames`가 없다. 원소 순서는 `file` 원소를 서버가 시작할 때 정한 동영상 목록 순서대로 먼저 두고, 그 뒤에 `camera` 원소를 `--camera`로 등록한 순서대로 둔다. 카메라 원소는 카메라의 현재 연결 상태와 관계없이 항상 목록에 있다. 소스 없이 실행했으면 `sources`는 빈 배열이다. 쿼리 문자열은 무시한다.

#### Scenario: 두 파일이 있는 디렉터리
- **WHEN** `b.h264`(300프레임)와 `a.h264`(150프레임)가 있는 디렉터리를 `--video-dir`로 지정해 실행하고 `GET /api/sources`를 요청한다
- **THEN** 서버는 `200 OK`와 `{"sources":[{"type":"file","name":"a.h264","frames":150},{"type":"file","name":"b.h264","frames":300}]}`에 해당하는 JSON을 응답한다

#### Scenario: 동영상 없이 실행
- **WHEN** `--video-dir`과 `--camera` 없이 실행하고 `GET /api/sources`를 요청한다
- **THEN** 서버는 `200 OK`와 `{"sources":[]}`에 해당하는 JSON을 응답한다

#### Scenario: 이전 목록 경로
- **WHEN** 웹 루트에 `api/videos` 파일이 없는 서버에 `GET /api/videos`를 요청한다
- **THEN** 서버는 `404 Not Found`로 응답한다

#### Scenario: 파일과 카메라
- **WHEN** `a.h264`(150프레임)가 있는 디렉터리와 `--camera front=... --camera a.h264=...`로 실행하고 `GET /api/sources`를 요청한다
- **THEN** 서버는 `{"sources":[{"type":"file","name":"a.h264","frames":150},{"type":"camera","name":"front"},{"type":"camera","name":"a.h264"}]}`에 해당하는 JSON을 응답한다

#### Scenario: 연결되지 않은 카메라
- **WHEN** 꺼져 있는 카메라를 `--camera front=...`로 등록해 실행하고 `GET /api/sources`를 요청한다
- **THEN** 응답에 `{"type":"camera","name":"front"}` 원소가 있다
