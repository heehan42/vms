# Spec Delta

## MODIFIED Requirements

### Requirement: 요청 오류 응답
서버는 처리할 수 없는 요청에 오류 상태 코드로 응답하고 연결을 닫아야 한다(SHALL). 요청 줄이나 헤더 형식이 잘못되었거나 헤더 영역이 8 KiB를 넘으면 `400 Bad Request`, 요청 경로가 지원하지 않는 메서드는 `405 Method Not Allowed`로 응답하며 `Allow`에 그 경로가 지원하는 메서드를 넣는다. 정적 파일 경로와 소스 목록 경로 `/api/sources`는 `GET`만, 시그널링 경로 `/offer`는 `POST`만 지원한다. 경로는 쿼리 문자열을 뺀 부분으로 판단하며, `/api/sources`는 웹 루트에 같은 이름의 파일이 있어도 소스 목록으로 응답한다.

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

#### Scenario: 동영상 목록 경로에 POST 요청
- **WHEN** 클라이언트가 `Content-Length: 0`으로 `POST /api/sources`를 요청한다
- **THEN** 서버는 `Allow: GET` 헤더를 포함한 `405 Method Not Allowed`로 응답한다
