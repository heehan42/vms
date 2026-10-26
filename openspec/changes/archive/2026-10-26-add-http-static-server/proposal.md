# Proposal

## Why

WebRTC로 동영상을 재생하려면 브라우저가 먼저 서버에서 웹 페이지와 스크립트를 받아야 하고, 이후 시그널링(SDP 교환)도 같은 HTTP 서버를 통해 이루어진다. 아직 소스 코드와 빌드 구성이 없으므로, 첫 마일스톤(M1)으로 Windows 브라우저에서 WSL2의 C++ 서버에 접속해 페이지가 열리는 최소 경로를 만든다.

## What Changes

- CMake 기반 C++23 빌드 구성을 추가한다(`-fno-exceptions` 적용).
- 단일 스레드 이벤트 루프(epoll) 위에서 동작하는 HTTP/1.1 서버 실행 파일을 추가한다. 이 이벤트 루프는 이후 마일스톤에서 UDP 소켓(ICE/DTLS/RTP)과 타이머도 함께 처리하도록 확장할 기반이다.
- 지정한 웹 루트 디렉터리의 정적 파일을 `GET` 요청으로 제공한다. `/`는 `index.html`로 연결한다.
- 잘못된 요청, 지원하지 않는 메서드, 없는 파일, 웹 루트 밖 경로 접근에 대해 적절한 오류 응답을 보낸다.
- 브라우저에서 서버 동작을 확인할 수 있는 최소 웹 페이지(`index.html`, `app.js`)를 추가한다.
- 포트와 웹 루트를 명령행 인자로 지정할 수 있게 하고, `SIGINT`/`SIGTERM`을 받으면 정상 종료한다.

범위 밖(이후 마일스톤):

- 시그널링 엔드포인트(SDP offer/answer), WebSocket
- 동영상 목록 API, HTTPS, keep-alive 연결 재사용, 요청 본문 처리, 퍼센트 인코딩 해석

## Capabilities

### New Capabilities

- `http-server`: 브라우저의 HTTP 요청을 받아 해석하고, 웹 루트의 정적 파일 또는 오류로 응답하는 서버 동작. 이후 시그널링·동영상 목록 엔드포인트가 이 위에 추가된다.

### Modified Capabilities

(없음)

## Impact

- 새 코드: `CMakeLists.txt`, `src/`(이벤트 루프, HTTP 서버, 정적 파일 처리, 진입점), `web/`(정적 페이지), `tests/`
- 의존성: 외부 라이브러리 없음(Linux epoll, C++23 표준 라이브러리만 사용)
- 실행 환경: 서버는 WSL2에서 실행하고, Windows 브라우저에서 `http://localhost:<port>/`로 접속한다
