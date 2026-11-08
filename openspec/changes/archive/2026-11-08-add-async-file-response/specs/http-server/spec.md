# Spec Delta

## MODIFIED Requirements

### Requirement: 응답 헤더
서버는 모든 응답을 HTTP/1.1 형식으로 보내고 `Content-Length`와 `Connection: close`를 포함해야 한다(SHALL). 파일 응답의 `Content-Type`은 확장자로 정하며 `.html`, `.js`, `.css`, `.json`, `.svg`, `.png`, `.ico`를 구분하고, 그 밖의 확장자는 `application/octet-stream`으로 보내야 한다(SHALL). 파일 내용을 담은 응답(`200 OK`, `206 Partial Content`)에는 `Accept-Ranges: bytes`를 포함해야 한다(SHALL).

#### Scenario: HTML 응답
- **WHEN** `index.html`을 응답한다
- **THEN** 응답에 `Content-Type: text/html; charset=utf-8`과 파일 크기와 같은 `Content-Length`가 포함된다

#### Scenario: 알 수 없는 확장자
- **WHEN** 확장자가 `.bin`인 파일을 응답한다
- **THEN** 응답의 `Content-Type`은 `application/octet-stream`이다

#### Scenario: 범위 요청 지원 표시
- **WHEN** 클라이언트가 `GET /app.js`를 요청한다
- **THEN** `200 OK` 응답에 `Accept-Ranges: bytes`가 포함된다

### Requirement: 연결 수명
서버는 응답 하나를 모두 보낸 뒤 연결을 닫아야 한다(SHALL). 여러 클라이언트의 연결을 동시에 받아들이며, 한 연결이 느리게 송수신하더라도 다른 연결의 처리를 막지 않아야 한다(SHALL). 파일 응답을 위해 파일을 열고 읽는 동안에도 다른 연결의 요청 수신과 응답 송신, 엔드포인트 처리를 멈추지 않아야 한다(SHALL).

#### Scenario: 응답 후 종료
- **WHEN** 서버가 응답을 끝까지 보낸다
- **THEN** 서버는 해당 연결을 닫는다

#### Scenario: 느린 클라이언트와 동시 요청
- **WHEN** 한 클라이언트가 요청 헤더를 끝내지 않은 채 연결을 유지하는 동안 다른 클라이언트가 `GET /`를 요청한다
- **THEN** 다른 클라이언트는 지연 없이 `200 OK` 응답을 받는다

#### Scenario: 파일 읽기가 늦어지는 동안의 엔드포인트 요청
- **WHEN** 서버가 한 클라이언트에 보낼 파일을 디스크에서 읽느라 오래 걸리는 동안 다른 클라이언트가 `GET /api/sources`를 요청한다
- **THEN** 다른 클라이언트는 파일 읽기가 끝나기를 기다리지 않고 소스 목록 응답을 받는다

### Requirement: 파일 읽기 실패 응답
서버는 경로 확인을 통과한 파일을 열거나 읽지 못하면 원인을 로그에 남기고 오류로 응답해야 한다(SHALL). 파일이 경로 확인 뒤 삭제되어 열 때 없으면 `404 Not Found`, 그 밖의 원인(읽기 권한 없음, 읽기 오류 등)이면 `500 Internal Server Error`로 응답한다.

#### Scenario: 읽기 권한이 없는 파일
- **WHEN** 웹 루트에 읽기 권한이 없는 일반 파일이 있고 클라이언트가 그 파일을 요청한다
- **THEN** 서버는 `500 Internal Server Error`로 응답한다

#### Scenario: 경로 확인 뒤 삭제된 파일
- **WHEN** 서버가 요청 경로를 확인한 뒤 파일을 열기 전에 그 파일이 삭제된다
- **THEN** 서버는 `404 Not Found`로 응답한다

## ADDED Requirements

### Requirement: 범위 요청
서버는 파일 응답 경로로 온 `GET` 요청에 `Range` 헤더가 범위 하나(`bytes=a-b`, `bytes=a-`, `bytes=-n`)를 담고 있고 그 시작 위치가 파일 크기보다 작으면, 그 범위의 바이트만 `206 Partial Content`로 응답해야 한다(SHALL). 응답에는 `Content-Range: bytes <시작>-<끝>/<파일 크기>`를 넣는다. 끝 위치가 파일 끝을 넘으면 파일 끝까지로 줄인다. `bytes=-n`은 파일 마지막 n바이트다. 위치는 0부터 세는 바이트 단위이며 끝 위치를 포함한다.

#### Scenario: 처음 10바이트
- **WHEN** 크기가 100바이트인 파일에 `Range: bytes=0-9`로 요청한다
- **THEN** 서버는 `206 Partial Content`, `Content-Range: bytes 0-9/100`, `Content-Length: 10`과 파일의 처음 10바이트로 응답한다

#### Scenario: 끝이 없는 범위
- **WHEN** 크기가 100바이트인 파일에 `Range: bytes=90-`로 요청한다
- **THEN** 서버는 `Content-Range: bytes 90-99/100`과 마지막 10바이트로 응답한다

#### Scenario: 파일 끝을 넘는 끝 위치
- **WHEN** 크기가 100바이트인 파일에 `Range: bytes=95-200`으로 요청한다
- **THEN** 서버는 `Content-Range: bytes 95-99/100`과 마지막 5바이트로 응답한다

#### Scenario: 마지막 n바이트
- **WHEN** 크기가 100바이트인 파일에 `Range: bytes=-20`으로 요청한다
- **THEN** 서버는 `Content-Range: bytes 80-99/100`과 마지막 20바이트로 응답한다

### Requirement: 만족할 수 없는 범위
서버는 범위 하나를 담은 `Range` 요청의 범위가 파일 안에 한 바이트도 없으면(시작 위치가 파일 크기 이상이거나 `bytes=-0`) `416 Range Not Satisfiable`로 응답해야 한다(SHALL). 응답에는 `Content-Range: bytes */<파일 크기>`를 넣는다.

#### Scenario: 파일 크기를 넘는 시작 위치
- **WHEN** 크기가 100바이트인 파일에 `Range: bytes=100-`로 요청한다
- **THEN** 서버는 `Content-Range: bytes */100`을 포함한 `416 Range Not Satisfiable`로 응답한다

#### Scenario: 빈 파일
- **WHEN** 크기가 0바이트인 파일에 `Range: bytes=0-`로 요청한다
- **THEN** 서버는 `Content-Range: bytes */0`을 포함한 `416 Range Not Satisfiable`로 응답한다

### Requirement: 범위 요청 무시
서버는 `Range` 헤더가 `bytes` 단위가 아니거나, 형식이 잘못되었거나, 범위를 둘 이상 담고 있거나, 요청에 `If-Range` 헤더가 함께 있거나, `Range` 헤더가 여러 번 오면 `Range`를 무시하고 파일 전체를 `200 OK`로 응답해야 한다(SHALL).

#### Scenario: 여러 범위
- **WHEN** 클라이언트가 `Range: bytes=0-9,20-29`로 파일을 요청한다
- **THEN** 서버는 파일 전체를 `200 OK`로 응답한다

#### Scenario: 잘못된 형식
- **WHEN** 클라이언트가 `Range: bytes=abc`로 파일을 요청한다
- **THEN** 서버는 파일 전체를 `200 OK`로 응답한다

#### Scenario: If-Range와 함께 온 범위
- **WHEN** 클라이언트가 `Range: bytes=0-9`와 `If-Range: "x"`로 파일을 요청한다
- **THEN** 서버는 파일 전체를 `200 OK`로 응답한다
