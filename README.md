# vms

카메라 영상과 저장된 동영상을 관리하고 재생하는 VMS(Video Management System)입니다.
먼저 서버의 동영상 파일을 웹 사이트에서 WebRTC로 재생하는 기능부터 구현하고 있으며,
현재는 웹 페이지를 제공하는 HTTP 서버까지 구현되어 있습니다.

## 필요한 도구

- CMake 3.28 이상
- C++23의 `<print>`, `std::expected`, `std::move_only_function`을 지원하는 컴파일러 (GCC 14 이상)
- 통합 테스트용: `bash`, `curl`, `nc`(OpenBSD netcat)

## 빌드

```bash
cmake -B build
cmake --build build -j
```

## 실행

저장소 루트에서 실행하면 `./web` 디렉터리의 파일을 8080 포트로 제공합니다.

```bash
./build/vms_web_server
```

포트와 웹 루트는 인자로 바꿀 수 있습니다.

```bash
./build/vms_web_server --port 9000 --web-root /path/to/site
```

브라우저에서 `http://localhost:8080/`에 접속해 "app.js가 실행되었습니다." 문구가 보이면
페이지와 스크립트가 모두 정상적으로 전달된 것입니다. `Ctrl+C`로 종료합니다.

WSL2에서 서버를 실행하는 경우에도 Windows 브라우저에서 같은 주소로 접속합니다.

## 테스트

```bash
ctest --test-dir build --output-on-failure
```

`tests/`는 `src/`와 같은 디렉터리 구조를 따르며, 모듈별 단위 테스트(`tests/core/`, `tests/http/`)와
서버를 실제로 띄워 요청을 보내는 통합 테스트(`tests/integration/`)가 함께 실행됩니다.
통합 테스트는 기본으로 18080 포트를 사용하며, 사용 중이면 `HTTP_SERVER_TEST_PORT` 환경 변수로 바꿀 수 있습니다.
