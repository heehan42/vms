# Tasks

## 1. 다른 스레드에서 루프 작업 넘기기

- [x] 1.1 `src/core/event_loop.h/.cpp`에 `EventLoop::Post`를 추가한다(design 결정 2). `Create()`에서 eventfd와 잠금·루프 작업 목록을 담은 힙 구조체를 만들어 epoll에 등록하고, 클래스 주석에 "`Post`는 아무 스레드에서 호출할 수 있다"와 "`Post`를 부르는 스레드는 `EventLoop`보다 먼저 끝나야 한다"를 적는다
- [x] 1.2 `tests/core/event_loop_test.cpp`에서 확인한다(`ctest -R event_loop` 통과)
  - 다른 스레드에서 넘긴 루프 작업이 `Run()`을 호출한 스레드에서 실행됨(스레드 id 비교)
  - 한 스레드가 넘긴 여러 루프 작업이 넘긴 순서대로 실행됨
  - 루프 작업 안에서 넘긴 루프 작업도 실행됨
  - fd 이벤트도 타이머도 없이 기다리는 루프가 `Post`로 깨어남
  - `Create()`로 만든 루프를 이동한 뒤에도 `Post`가 동작함

## 2. Range 해석과 범위 계산

- [x] 2.1 `src/http/http_response.h/.cpp`에 `kPartialContent`(206)와 `kRangeNotSatisfiable`(416)을 추가하고, `tests/http/http_response_test.cpp`에서 숫자와 사유 문구를 확인한다(`ctest -R http_response` 통과)
- [x] 2.2 `src/http/http_request.h/.cpp`가 `Range` 헤더 값(한 번만 왔을 때)과 `If-Range` 헤더 유무를 보관하게 한다. `Range`가 여러 번 오면 값을 비우고 여러 번 왔음을 표시한다(design 결정 7). `tests/http/http_request_test.cpp`에서 확인한다(`ctest -R http_request` 통과)
- [x] 2.3 `src/http/byte_range.h/.cpp`에 `Range` 헤더 해석(`ByteRangeSpec`)과 파일 크기에 맞춘 범위 계산을 추가한다(design 결정 7). `CMakeLists.txt`에 소스를 추가한다
- [x] 2.4 `tests/http/byte_range_test.cpp`에서 명세 "범위 요청", "만족할 수 없는 범위", "범위 요청 무시"의 시나리오 값을 그대로 확인한다(`ctest -R byte_range` 통과)
  - `bytes=0-9`, `bytes=90-`, `bytes=95-200`, `bytes=-20`을 크기 100에서 계산
  - `bytes=100-`, `bytes=-0`, 크기 0의 `bytes=0-`은 만족할 수 없음
  - `bytes=0-9,20-29`, `bytes=abc`, `items=0-9`, `bytes=9-0`, `uint64`를 넘는 숫자는 범위 없음
  - `bytes=-200`을 크기 100에서 계산하면 0~99

## 3. 파일 읽기 작업 실행 함수

- [x] 3.1 `src/http/static_files.h/.cpp`의 `ResolveStaticFilePath`가 파일 시스템을 조회하지 않고 문자열 검사만 하도록 바꾼다(design 결정 6 "경로 확인"). 반환 오류는 `kBadRequest`, `kForbidden`만 남긴다. `ReadFileContent`는 쓰는 곳이 없어지면 지운다. `tests/http/static_files_test.cpp`를 바뀐 동작에 맞추고 없는 파일도 경로를 돌려주는지 확인한다(`ctest -R static_files` 통과)
- [x] 3.2 `src/http/file_reader.h/.cpp`에 파일 읽기 작업·결과 구조체와, 파일 읽기 작업 하나로 파일 읽기 결과를 만드는 실행 함수를 추가한다(design 결정 6의 1~5단계). `CMakeLists.txt`에 소스를 추가한다
- [x] 3.3 `tests/http/file_reader_test.cpp`에서 실행 함수를 직접 호출해 확인한다(`ctest -R file_reader` 통과)
  - 범위 없음: `200`, 본문이 파일 전체, `Accept-Ranges: bytes`, `Content-Type`
  - `bytes=0-9`: `206`, `Content-Range: bytes 0-9/100`, `Content-Length: 10`, 처음 10바이트
  - `bytes=100-`: `416`, `Content-Range: bytes */100`
  - 없는 경로와 디렉터리: `404`
  - FIFO: `open`에서 멈추지 않고 `404`
  - 읽기 권한 없는 파일: `500` (root로 실행 중이면 건너뜀)

## 4. 파일 읽기 스레드

- [x] 4.1 `src/http/file_reader.h/.cpp`에 `FileReader`를 추가한다(design 결정 3). 생성자는 `EventLoop&`, 결과를 받을 함수, 선택 인자인 실행 함수를 받는다. 소멸자는 남은 작업을 버리고 실행 중인 작업을 기다려 join한다
- [x] 4.2 `tests/http/file_reader_test.cpp`에 `FileReader` 시험을 더한다(`ctest -R file_reader` 통과)
  - 넣은 작업의 결과가 루프 스레드에서, 넣은 순서대로 전달됨
  - 실행 함수가 멈춰 있는 동안 루프의 타이머가 제때 실행됨
  - 실행 중인 작업이 있을 때 소멸하면 그 작업이 끝난 뒤 반환되고, 큐에 남은 작업은 실행되지 않음

## 5. HTTP 서버의 파일 응답

- [x] 5.1 `src/http/http_server.h`에 `HttpFileResponse`와 `HttpGetResult`를 추가하고 `HttpGetHandler`의 반환형을 바꾼다(design 결정 4). 기존 핸들러(`main.cpp`의 `/api/sources`, 테스트 핸들러)가 고치지 않고 빌드되는지 확인한다(`cmake --build build` 성공)
- [x] 5.2 `src/http/http_server.cpp`에 연결 번호, `kWaitingForFile` 상태, 수명 표시, `FileReader` 멤버를 추가하고, 정적 파일과 `HttpFileResponse`를 파일 읽기 스레드로 보내도록 바꾼다(design 결정 5). 요청 로그는 결과가 도착했을 때 남긴다. `HttpServer::Start`가 선택 인자로 실행 함수를 받아 `FileReader`에 넘긴다
- [x] 5.3 `tests/http/http_endpoint_test.cpp`에서 소켓 요청으로 확인한다(`ctest -R http_endpoint` 통과)
  - 정적 파일 `GET`에 `Range: bytes=0-9`로 `206`과 `Content-Range`, 범위 없이 `200`과 `Accept-Ranges: bytes`
  - `Range: bytes=0-9`와 `If-Range`를 함께 보내면 `200`
  - GET 핸들러가 `HttpFileResponse`를 돌려주면 그 파일 내용과 핸들러가 정한 `Content-Type`으로 응답
  - 실행 함수가 테스트가 풀어 줄 때까지 멈춰 있는 동안, 다른 연결의 GET 엔드포인트 요청이 응답을 받음(명세 "파일 읽기가 늦어지는 동안의 엔드포인트 요청")
  - 실행 함수가 멈춰 있는 동안 클라이언트가 RST로 연결을 끊으면(`SO_LINGER` 0 후 `close()`) 서버가 그 연결을 닫고, 나중에 도착한 결과는 버림(같은 fd 번호로 새 연결을 맺어도 그 연결에 이전 결과가 가지 않음)
  - 실행 함수가 멈춰 있는 동안 서버를 소멸시키면, 실행 함수를 풀어 준 뒤 소멸이 끝나고 이후 루프를 돌려도 오류가 없음

## 6. 통합 시험과 문서

- [x] 6.1 `tests/integration/http_server_test.sh`에 `curl -r`(또는 `-H 'Range: ...'`) 시나리오를 더한다: `206`과 받은 바이트 수, `bytes=<크기>-`의 `416`, 여러 범위의 `200`, 응답 헤더의 `Accept-Ranges: bytes`. `ctest -R http_server`가 통과하는지 확인한다
- [x] 6.2 README에 정적 파일의 범위 요청 확인 방법(`curl -r 0-9 -i http://localhost:8080/app.js`)과 결과(`206 Partial Content`)를 적는다. 적은 명령을 실행 중인 서버에 그대로 실행해 결과가 같은지 확인한다
- [x] 6.3 전체 테스트(`ctest --test-dir build`)가 통과하고, 브라우저에서 기본 페이지와 WebRTC 파일·카메라 재생이 이전처럼 동작하는지 확인한다. 확인 결과를 작업 메모로 남긴다
  - 작업 메모(2026-11-08): `ctest --test-dir build` 45개 모두 통과. 테스트 영상(`test_pattern.h264`)과 `vms_camera_server --test-source`를 카메라 `cam`으로 등록한 서버에서 `index.html`·`app.js`가 `200`과 파일 크기만큼의 본문으로 응답하고, `/api/sources`가 파일과 카메라를 돌려주며, 파일·카메라 `POST /offer`가 `200`으로 응답함을 curl로 확인했다. 이어서 같은 구성(`videos/test_pattern.h264`·`videos/color_bars.h264`, 카메라 `cam`)으로 실행한 서버에 Windows Chrome으로 `http://localhost:8080/`에 접속해, 기본 페이지가 표시되고 동영상 파일과 카메라 `cam`의 WebRTC 재생이 이전처럼 동작함을 사용자가 확인했다.

## Workflow follow-up

- 구현을 마치면 `/openspec-archive-change`로 아카이브하고 `http-server` 명세에 반영한다.
- 다음 change `add-recording-playback`에서 녹화 파일 경로 핸들러가 `HttpFileResponse`를, 녹화 스레드가 `EventLoop::Post`를 쓴다.
