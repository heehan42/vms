# http-server Specification

## Purpose

브라우저가 웹 페이지와 스크립트를 받을 수 있도록 HTTP 요청을 해석하고 웹 루트의 정적 파일 또는 오류로 응답한다. 이후 시그널링과 동영상 목록 같은 엔드포인트가 이 서버 위에 추가된다.

## Requirements

### Requirement: 실행 설정
서버는 명령행 인자로 수신 포트(`--port`)와 웹 루트 디렉터리(`--web-root`)를 받아야 한다(SHALL). 인자를 생략하면 포트 8080과 현재 디렉터리의 `web`을 사용하며, 모든 네트워크 인터페이스(0.0.0.0)에서 연결을 받아야 한다(SHALL).

#### Scenario: 기본값으로 실행
- **WHEN** 인자 없이 서버를 실행한다
- **THEN** 서버는 0.0.0.0:8080에서 연결을 받고 `./web`의 파일을 제공한다

#### Scenario: 포트와 웹 루트 지정
- **WHEN** `--port 9000 --web-root /srv/site`로 서버를 실행한다
- **THEN** 서버는 9000번 포트에서 연결을 받고 `/srv/site`의 파일을 제공한다

#### Scenario: 시작 실패
- **WHEN** 포트가 이미 사용 중이거나, 웹 루트가 디렉터리가 아니거나, 인자 형식이 잘못되었다
- **THEN** 서버는 원인을 표준 오류에 출력하고 0이 아닌 종료 코드로 끝난다

### Requirement: 정적 파일 제공
서버는 `GET` 요청의 경로를 웹 루트 기준 파일 경로로 해석해 파일 내용을 `200 OK`로 응답해야 한다(SHALL). 경로 `/`는 `/index.html`로 취급하고, 쿼리 문자열(`?` 이후)은 무시한다.

#### Scenario: 루트 경로 요청
- **WHEN** 브라우저가 `GET /`를 요청한다
- **THEN** 서버는 웹 루트의 `index.html` 내용을 `200 OK`로 응답한다

#### Scenario: 하위 파일 요청
- **WHEN** 브라우저가 `GET /app.js?v=1`을 요청한다
- **THEN** 서버는 웹 루트의 `app.js` 내용을 `200 OK`로 응답한다

### Requirement: 응답 헤더
서버는 모든 응답을 HTTP/1.1 형식으로 보내고 `Content-Length`와 `Connection: close`를 포함해야 한다(SHALL). 파일 응답의 `Content-Type`은 확장자로 정하며 `.html`, `.js`, `.css`, `.json`, `.svg`, `.png`, `.ico`를 구분하고, 그 밖의 확장자는 `application/octet-stream`으로 보내야 한다(SHALL).

#### Scenario: HTML 응답
- **WHEN** `index.html`을 응답한다
- **THEN** 응답에 `Content-Type: text/html; charset=utf-8`과 파일 크기와 같은 `Content-Length`가 포함된다

#### Scenario: 알 수 없는 확장자
- **WHEN** 확장자가 `.bin`인 파일을 응답한다
- **THEN** 응답의 `Content-Type`은 `application/octet-stream`이다

### Requirement: 연결 수명
서버는 응답 하나를 모두 보낸 뒤 연결을 닫아야 한다(SHALL). 여러 클라이언트의 연결을 동시에 받아들이며, 한 연결이 느리게 송수신하더라도 다른 연결의 처리를 막지 않아야 한다(SHALL).

#### Scenario: 응답 후 종료
- **WHEN** 서버가 응답을 끝까지 보낸다
- **THEN** 서버는 해당 연결을 닫는다

#### Scenario: 느린 클라이언트와 동시 요청
- **WHEN** 한 클라이언트가 요청 헤더를 끝내지 않은 채 연결을 유지하는 동안 다른 클라이언트가 `GET /`를 요청한다
- **THEN** 다른 클라이언트는 지연 없이 `200 OK` 응답을 받는다

### Requirement: 요청 오류 응답
서버는 처리할 수 없는 요청에 오류 상태 코드로 응답하고 연결을 닫아야 한다(SHALL). 요청 줄이나 헤더 형식이 잘못되었거나 헤더 영역이 8 KiB를 넘으면 `400 Bad Request`, 요청 경로가 지원하지 않는 메서드는 `405 Method Not Allowed`로 응답하며 `Allow`에 그 경로가 지원하는 메서드를 넣는다. 정적 파일 경로는 `GET`만, 시그널링 경로 `/offer`는 `POST`만 지원한다.

#### Scenario: 잘못된 요청 줄
- **WHEN** 클라이언트가 `HELLO\r\n\r\n`을 보낸다
- **THEN** 서버는 `400 Bad Request`로 응답한다

#### Scenario: 헤더 크기 초과
- **WHEN** 클라이언트가 빈 줄 없이 8 KiB를 넘는 헤더를 보낸다
- **THEN** 서버는 `400 Bad Request`로 응답한다

#### Scenario: 지원하지 않는 메서드
- **WHEN** 클라이언트가 `POST /`를 요청한다
- **THEN** 서버는 `Allow: GET` 헤더를 포함한 `405 Method Not Allowed`로 응답한다

#### Scenario: 시그널링 경로에 GET 요청
- **WHEN** 클라이언트가 `GET /offer`를 요청한다
- **THEN** 서버는 `Allow: POST` 헤더를 포함한 `405 Method Not Allowed`로 응답한다

### Requirement: 요청 본문 수신
서버는 `Content-Length` 헤더가 있는 요청의 본문을 그 길이만큼 모두 받은 뒤 처리해야 한다(SHALL). 본문은 64 KiB까지 허용한다. `Content-Length`가 없는 요청은 본문이 없는 것으로 취급한다.

#### Scenario: 여러 번에 나눠 도착한 본문
- **WHEN** 클라이언트가 `Content-Length: 3000`인 `POST /offer` 요청의 헤더와 본문을 여러 TCP 세그먼트로 나눠 천천히 보낸다
- **THEN** 서버는 본문 3000바이트를 모두 받은 뒤 그 본문으로 요청을 처리한다

#### Scenario: 헤더와 함께 도착한 본문
- **WHEN** 클라이언트가 헤더와 본문 전체를 한 번에 보낸다
- **THEN** 서버는 헤더 뒤의 바이트를 본문으로 사용해 요청을 처리한다

### Requirement: 요청 본문 오류 응답
서버는 본문을 받을 수 없는 요청에 오류로 응답해야 한다(SHALL). `Content-Length` 값이 음이 아닌 10진수가 아니거나 서로 다른 값으로 여러 번 오면 `400 Bad Request`, 64 KiB를 넘으면 `413 Content Too Large`, `Transfer-Encoding` 헤더가 있으면 `501 Not Implemented`, 본문이 필요한 경로에 `Content-Length`가 없으면 `411 Length Required`로 응답한다.

#### Scenario: 잘못된 Content-Length
- **WHEN** 클라이언트가 `Content-Length: abc`인 요청을 보낸다
- **THEN** 서버는 `400 Bad Request`로 응답한다

#### Scenario: 서로 다른 Content-Length
- **WHEN** 클라이언트가 `Content-Length: 10`과 `Content-Length: 20`을 함께 담은 요청을 보낸다
- **THEN** 서버는 `400 Bad Request`로 응답한다

#### Scenario: 본문 크기 초과
- **WHEN** 클라이언트가 `Content-Length: 100000`인 `POST /offer` 요청을 보낸다
- **THEN** 서버는 본문을 기다리지 않고 `413 Content Too Large`로 응답한다

#### Scenario: chunked 전송
- **WHEN** 클라이언트가 `Transfer-Encoding: chunked`인 `POST /offer` 요청을 보낸다
- **THEN** 서버는 `501 Not Implemented`로 응답한다

#### Scenario: 길이 없는 POST
- **WHEN** 클라이언트가 `Content-Length` 없이 `POST /offer`를 요청한다
- **THEN** 서버는 `411 Length Required`로 응답한다

### Requirement: 파일 접근 제한
서버는 웹 루트 안의 일반 파일만 제공해야 한다(MUST). 경로에 `..` 구성 요소가 있으면 `403 Forbidden`, 파일이 없거나 디렉터리인 경우는 `404 Not Found`로 응답한다. 경로의 퍼센트 인코딩은 해석하지 않고 문자 그대로 취급한다.

#### Scenario: 상위 디렉터리 접근
- **WHEN** 클라이언트가 `GET /../etc/passwd`를 요청한다
- **THEN** 서버는 `403 Forbidden`으로 응답하고 웹 루트 밖의 파일 내용을 보내지 않는다

#### Scenario: 없는 파일
- **WHEN** 클라이언트가 존재하지 않는 `GET /missing.js`를 요청한다
- **THEN** 서버는 `404 Not Found`로 응답한다

### Requirement: 파일 읽기 실패 응답
서버는 경로 확인을 통과한 파일을 읽지 못하면(읽기 권한 없음, 확인 직후 삭제 등) `500 Internal Server Error`로 응답하고 원인을 로그에 남겨야 한다(SHALL).

#### Scenario: 읽기 권한이 없는 파일
- **WHEN** 웹 루트에 읽기 권한이 없는 일반 파일이 있고 클라이언트가 그 파일을 요청한다
- **THEN** 서버는 `500 Internal Server Error`로 응답한다

### Requirement: 정상 종료
서버는 `SIGINT` 또는 `SIGTERM`을 받으면 새 연결을 받지 않고 열린 소켓을 모두 닫은 뒤 종료 코드 0으로 끝나야 한다(SHALL).

#### Scenario: Ctrl+C로 종료
- **WHEN** 서버 실행 중 터미널에서 Ctrl+C를 누른다
- **THEN** 서버는 종료 로그를 출력하고 종료 코드 0으로 끝난다

### Requirement: 동작 확인용 웹 페이지
웹 루트의 기본 페이지는 브라우저에서 HTML과 별도 스크립트 파일이 모두 정상적으로 전달되었는지 확인할 수 있어야 한다(SHALL).

#### Scenario: Windows 브라우저에서 접속
- **WHEN** WSL2에서 기본 설정으로 서버를 실행하고 Windows 브라우저로 `http://localhost:8080/`에 접속한다
- **THEN** 페이지가 표시되고, `app.js`가 실행되었음을 나타내는 문구가 화면에 나타난다
