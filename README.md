# vms

카메라 영상과 저장된 동영상을 관리하고 재생하는 VMS(Video Management System)입니다.
먼저 서버의 동영상 파일을 웹 사이트에서 WebRTC로 재생하는 기능부터 구현하고 있으며,
현재는 웹 페이지를 제공하는 HTTP 서버와, 브라우저와 ICE 연결을 맺는 시그널링·ICE-lite까지 구현되어 있습니다.
DTLS와 미디어 송신은 아직 없습니다.

## 필요한 도구

- CMake 3.28 이상
- C++23의 `<print>`, `std::expected`, `std::move_only_function`을 지원하는 컴파일러 (GCC 14 이상)
- OpenSSL 3.x 개발 패키지. Ubuntu에서는 다음과 같이 설치합니다.

  ```bash
  sudo apt install libssl-dev
  ```

- 통합 테스트용: `bash`, `curl`, `nc`(OpenBSD netcat)

## 빌드

```bash
cmake -B build
cmake --build build -j
```

## 실행

저장소 루트에서 실행하면 `./web` 디렉터리의 파일을 8080 포트로 제공하고,
ICE 연결 확인(STUN)을 UDP 50000 포트에서 받습니다.

```bash
./build/vms_web_server
```

시작 로그에 ICE가 사용하는 주소가 나옵니다. 이 주소가 SDP answer의 candidate로 브라우저에 전달됩니다.

```text
ICE 대기 중: udp 172.27.209.81:50000
http://0.0.0.0:8080/ 에서 대기 중 (웹 루트: /home/.../web)
```

| 인자 | 기본값 | 의미 |
|---|---|---|
| `--port` | 8080 | HTTP 포트 |
| `--web-root` | `./web` | 정적 파일 디렉터리 |
| `--ice-port` | 50000 | ICE용 UDP 포트 |
| `--ice-address` | 자동 조회 | candidate로 알리고 UDP 소켓을 바인드할 IPv4 주소. 여러 번 쓸 수 있으며, 쓴 순서대로 우선순위가 높습니다 |

`--ice-address`를 주지 않으면 켜져 있고 루프백이 아닌 인터페이스의 IPv4 주소를 모두 씁니다.
이 머신에 없는 주소(예: 공유기의 공인 IP)는 바인드할 수 없으므로 지금은 같은 PC나 같은 내부 네트워크의
브라우저만 연결할 수 있습니다.

```bash
./build/vms_web_server --port 9000 --web-root /path/to/site --ice-port 50100 --ice-address 127.0.0.1
```

`Ctrl+C`로 종료합니다.

## WSL2에서 실행하기

WSL2 기본 네트워크 모드(NAT)에서 확인한 결과 별도 설정 없이 동작합니다.

- HTTP: Windows 브라우저에서 `http://localhost:8080/`으로 접속합니다(WSL2의 localhost 포워딩).
- ICE(UDP): localhost 포워딩은 TCP만 전달하지만, Windows는 WSL의 `eth0` 주소(`172.x.x.x`)로 UDP를 직접 보낼 수 있습니다.
  자동 조회가 이 주소를 candidate로 알리므로 `--ice-address`를 지정할 필요가 없습니다.

`eth0` 주소는 WSL을 다시 시작할 때마다 바뀌지만, 서버를 다시 시작하면 새 주소로 조회합니다.
NAT 모드에서는 같은 PC의 Windows 브라우저만 접속할 수 있고, 다른 기기에서는 WSL에 닿지 않습니다.

## 연결 확인

1. 서버를 실행하고 Windows Chrome에서 `http://localhost:8080/`에 접속합니다.
   "app.js가 실행되었습니다." 문구가 보이면 페이지와 스크립트가 정상적으로 전달된 것입니다.
2. **연결** 버튼을 누르면 시그널링이 "answer 적용 완료"가 되고, ICE 상태가 `checking`을 거쳐 `connected`가 됩니다.
   서버 로그에는 `ICE 연결됨: ...`이 남습니다.
3. 아직 DTLS를 구현하지 않았으므로 연결 상태(`connectionState`)는 잠시 뒤 `failed`가 됩니다. 정상입니다.

연결이 되지 않으면 Chrome에서 `chrome://webrtc-internals`를 **먼저** 열어 둔 뒤 다시 연결해 봅니다.

- ICE candidate grid에 서버 candidate가 보이는지, candidate pair가 `succeeded`인지 확인합니다.
- Stats의 candidate-pair에서 `requestsSent`는 늘어나는데 `responsesReceived`가 0이면, UDP가 서버에 닿지 않았거나 서버가 요청을 거부한 것입니다.
  서버 로그의 `STUN 요청 무시 (...)`가 거부 원인을 알려 주며, 아무 기록이 없으면 패킷이 서버에 도착하지 않은 것입니다.

## 테스트

```bash
ctest --test-dir build --output-on-failure
```

`tests/`는 `src/`와 같은 디렉터리 구조를 따르며, 모듈별 단위 테스트(`tests/core/`, `tests/crypto/`, `tests/http/`, `tests/webrtc/`)와
서버를 실제로 띄워 요청을 보내는 통합 테스트(`tests/integration/`)가 함께 실행됩니다.
`tests/webrtc/data/chrome_offer.sdp`는 실제 Chrome이 만든 offer 견본입니다.

통합 테스트는 기본으로 HTTP 18080 포트와 ICE UDP 18090 포트를 사용합니다.
사용 중이면 `HTTP_SERVER_TEST_PORT`, `ICE_TEST_PORT` 환경 변수로 바꿀 수 있습니다.

```bash
HTTP_SERVER_TEST_PORT=28080 ICE_TEST_PORT=28090 ctest --test-dir build --output-on-failure
```
