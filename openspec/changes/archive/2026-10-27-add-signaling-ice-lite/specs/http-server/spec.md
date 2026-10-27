# Spec Delta

## MODIFIED Requirements

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

## ADDED Requirements

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
