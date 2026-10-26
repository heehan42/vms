# Tasks

## 1. 빌드 구성

- [x] 1.1 루트 `CMakeLists.txt`에 C++23(`CMAKE_CXX_EXTENSIONS OFF`), `-fno-exceptions -Wall -Wextra -Wpedantic`, `server_core` 정적 라이브러리, `vms_web_server` 실행 파일, CTest 활성화를 구성하고, 빈 `main`으로 `cmake -B build && cmake --build build`가 경고 없이 성공하는지 확인한다

## 2. 이벤트 루프 기반

- [x] 2.1 `src/core/unique_fd.h`에 fd 소유 RAII 래퍼(이동 전용, 소멸 시 `close`)를 구현하고, 이동 후 원본이 fd를 닫지 않는지 단위 테스트로 확인한다
- [x] 2.2 `src/core/event_loop.h/.cpp`에 epoll 기반 루프(fd별 핸들러 등록·변경·해제, 핸들러 처리 후 지연 해제, 종료 요청)를 구현하고, `eventfd`나 `pipe`로 핸들러 호출과 핸들러 안에서의 해제가 안전한지 단위 테스트로 확인한다
- [x] 2.3 `signalfd`로 `SIGINT`/`SIGTERM`을 받아 루프 종료를 요청하는 설정 함수를 구현하고, 테스트 프로세스가 자신에게 `SIGTERM`을 보내 루프가 종료되는지 확인한다

## 3. HTTP 처리 구성 요소

- [x] 3.1 `src/http/http_response.h/.cpp`에 `HttpStatus` 열거형과 응답 바이트 생성(상태 줄, `Content-Type`, `Content-Length`, `Connection: close`, 추가 헤더)을 구현하고, 생성 결과 문자열을 단위 테스트로 확인한다
- [x] 3.2 `src/http/http_request.h/.cpp`에 헤더 경계 탐지(8 KiB 한도)와 요청 줄·헤더 문법 해석을 `std::expected<HttpRequest, HttpStatus>`로 구현하고, 정상 요청·잘못된 요청 줄·잘못된 헤더·한도 초과·경계 미도착을 단위 테스트로 확인한다
- [x] 3.3 `src/http/mime_type.h/.cpp`에 명세의 확장자별 `Content-Type` 판정을 구현하고, 각 확장자와 알 수 없는 확장자를 단위 테스트로 확인한다
- [x] 3.4 `src/http/static_files.h/.cpp`에 쿼리 제거, `/` → `/index.html`, `..` 구성 요소 차단, 일반 파일 확인과 읽기를 `std::error_code` 오버로드로만 구현하고, 임시 디렉터리에서 정상 파일·쿼리가 붙은 경로·`/`로 시작하지 않는 경로·상위 경로·없는 파일·디렉터리를 단위 테스트로 확인한다

## 4. 서버 조립과 웹 페이지

- [x] 4.1 `src/http/http_server.h/.cpp`에 리스닝 소켓(0.0.0.0, `SO_REUSEADDR`, 논블로킹)과 연결 상태 머신(요청 수신 → 응답 송신 → 종료, `MSG_NOSIGNAL`), `GET` 이외 메서드의 `405`(`Allow: GET`), 요청별 로그를 구현하고, 빌드가 성공하는지 확인한다
- [x] 4.2 `src/apps/web_server/main.cpp`에 `--port`, `--web-root` 인자 해석(기본값 8080, `./web`), 웹 루트 검증, 시그널 설정, 서버 실행과 종료 코드 처리를 구현하고, 잘못된 인자·없는 웹 루트·사용 중인 포트에서 오류 메시지와 0이 아닌 종료 코드가 나오는지 수동으로 확인한다
- [x] 4.3 `web/index.html`과 `web/app.js`를 추가해 스크립트 실행 시 화면 문구가 바뀌도록 하고, `curl http://localhost:8080/`과 `/app.js`가 올바른 `Content-Type`으로 응답하는지 확인한다
- [x] 4.4 `README.md`에 의존 도구, 빌드, 실행, 테스트 명령을 정리하고, 문서의 명령을 그대로 실행해 동작하는지 확인한다

## 5. 통합 검증

- [x] 5.1 `tests/integration/http_server_test.sh`를 작성해 임시 웹 루트와 포트로 서버를 띄우고 명세 시나리오(200, 쿼리 무시, 헤더, 400 두 종류, 403, 404, 405, 느린 연결 중 동시 요청, `SIGTERM` 후 종료 코드 0)를 검사하도록 CTest에 등록한 뒤, `ctest --test-dir build --output-on-failure`가 모두 통과하는지 확인한다
- [x] 5.2 WSL2에서 기본 설정으로 서버를 실행하고 Windows 브라우저에서 `http://localhost:8080/`에 접속해 `app.js` 실행 문구가 표시되는지, Ctrl+C로 정상 종료되는지 확인한다
- [x] 5.3 `tests/integration/http_server_test.sh`에 읽기 권한이 없는 파일을 요청하면 `500 Internal Server Error`로 응답하는 시나리오를 추가하고, `ctest --test-dir build --output-on-failure`가 모두 통과하는지 확인한다
