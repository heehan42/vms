# vms

카메라 영상과 저장된 동영상을 관리하고 재생하는 VMS(Video Management System)입니다.
먼저 서버의 동영상 파일을 웹 사이트에서 WebRTC로 재생하는 기능부터 구현하고 있으며,
현재는 웹 페이지를 제공하는 HTTP 서버, 브라우저와 ICE 연결을 맺는 시그널링·ICE-lite, 그 연결 위에서
SRTP 키를 정하는 DTLS 핸드셰이크, 그리고 서버의 H.264 파일을 RTP로 패킷화해 SRTP로 보내는 송신까지 구현되어 있습니다.
브라우저 페이지에서 `--video-dir`로 지정한 디렉터리의 동영상을 골라 연결하면 그 영상이 반복 재생됩니다.

## 필요한 도구

- CMake 3.28 이상
- C++23의 `<print>`, `std::expected`, `std::move_only_function`을 지원하는 컴파일러 (GCC 14 이상)
- OpenSSL 3.2 이상 개발 패키지(`libssl`, `libcrypto`). DTLS 데이터그램 처리에 3.2부터 들어간 datagram pair BIO를 씁니다.
  Ubuntu 24.04의 기본 패키지는 3.0이므로 빌드할 수 없습니다. 버전이 낮으면 `cmake -B build` 단계에서 오류가 납니다.
  Ubuntu에서는 다음과 같이 설치합니다.

  ```bash
  sudo apt install libssl-dev
  ```

- 통합 테스트용: `bash`, `curl`, `nc`(OpenBSD netcat)
- 테스트 영상 생성용: `ffmpeg`. 개발 도구로만 쓰며 빌드와 실행에는 필요 없습니다.

  ```bash
  sudo apt install ffmpeg
  ```

## 빌드

```bash
cmake -B build
cmake --build build -j
```

## 실행

저장소 루트에서 실행하면 `./web` 디렉터리의 파일을 8080 포트로 제공하고,
ICE 연결 확인(STUN)과 DTLS를 UDP 50000 포트에서 받습니다.

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
| `--video-dir` | 없음 | 재생할 H.264 Annex-B 파일(`.h264`, `.264`)이 든 디렉터리. 없으면 연결 확인까지만 하고 영상은 보내지 않습니다 |
| `--video-fps` | 30 | 영상 재생 속도(1~120). 디렉터리의 모든 파일에 적용하므로 파일을 만들 때의 프레임 속도와 같게 지정합니다 |

`--ice-address`를 주지 않으면 켜져 있고 루프백이 아닌 인터페이스의 IPv4 주소를 모두 씁니다.
이 머신에 없는 주소(예: 공유기의 공인 IP)는 바인드할 수 없으므로 지금은 같은 PC나 같은 내부 네트워크의
브라우저만 연결할 수 있습니다.

```bash
./build/vms_web_server --port 9000 --web-root /path/to/site --ice-port 50100 --ice-address 127.0.0.1
```

`Ctrl+C`로 종료합니다.

### 테스트 영상 만들기

ffmpeg의 내장 테스트 패턴으로 10초 길이의 720p 30fps H.264 파일 두 개를 `videos` 디렉터리에 만듭니다.
영상 파일은 저장소에 커밋하지 않습니다.

```bash
mkdir -p videos
```

```bash
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -t 10 -c:v libx264 -profile:v baseline -level 3.1 -pix_fmt yuv420p -g 30 -bf 0 -x264-params repeat-headers=1 -f h264 videos/test_pattern.h264
```

```bash
ffmpeg -f lavfi -i smptebars=size=1280x720:rate=30 -t 10 -c:v libx264 -profile:v baseline -level 3.1 -pix_fmt yuv420p -g 30 -bf 0 -x264-params repeat-headers=1 -f h264 videos/color_bars.h264
```

두 번째 명령은 입력만 `smptebars`(정지한 컬러 바)로 바꾼 것입니다. 옵션의 의미는 다음과 같습니다.

| 옵션 | 의미 |
|---|---|
| `-f lavfi -i testsrc2=size=1280x720:rate=30` | 내장 테스트 패턴 입력(움직이는 숫자와 색 막대) |
| `-t 10` | 10초 |
| `-c:v libx264 -profile:v baseline -level 3.1` | H.264 Baseline 프로파일, 레벨 3.1. answer가 고르는 `42e01f`(Constrained Baseline 3.1)와 맞춥니다 |
| `-pix_fmt yuv420p` | 브라우저가 디코딩하는 색 형식 |
| `-g 30 -bf 0` | 1초마다 키프레임, B-프레임 없음. 패킷이 손실되어도 1초 안에 화면이 회복됩니다 |
| `-x264-params repeat-headers=1` | 키프레임마다 SPS·PPS 반복 |
| `-f h264` | 컨테이너 없는 Annex-B 원시 스트림 출력 |

만든 디렉터리를 지정해 서버를 실행합니다.

```bash
./build/vms_web_server --video-dir videos --video-fps 30
```

서버는 시작할 때 디렉터리 바로 아래의 `.h264`·`.264` 파일을 이름순으로 모두 읽어 동영상 목록을 만들고,
파일 이름을 동영상 ID로 씁니다. 시작 로그에 동영상마다 ID와 프레임 수, 마지막에 동영상 수와 fps가 나옵니다.

```text
동영상 로드: color_bars.h264, 프레임 300
동영상 로드: test_pattern.h264, 프레임 300
동영상 목록: 2개, 30fps (videos)
```

- 파일을 읽을 수 없거나, 시작 코드가 없거나, 첫 프레임에 SPS·PPS·IDR이 모두 있지 않거나, 이름이 UTF-8이
  아닌 파일은 `경고: ...`를 남기고 건너뜁니다. 파일 하나는 64MiB, 목록 전체는 256MiB까지 읽습니다.
- 쓸 수 있는 파일이 하나도 없거나 디렉터리가 아니면 원인을 출력하고 시작하지 않습니다.
- fps는 모든 파일에 같은 값을 쓰므로, 같은 fps로 만든 파일만 디렉터리에 둡니다. 다르면 빠르거나 느리게 재생됩니다.
- 디렉터리는 시작할 때 한 번만 읽습니다. 파일을 추가하거나 지웠으면 서버를 다시 시작합니다.

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
   **동영상** 선택 목록에는 서버의 동영상이 이름순으로 나오고 첫 동영상이 선택되어 있습니다.
   `--video-dir` 없이 실행했으면 "동영상 없음"이 표시되며, 연결은 영상 없이 맺어집니다.
2. 동영상을 고르고 **연결** 버튼을 누르면 시그널링이 "answer 적용 완료"가 되고, ICE 상태가 `checking`을 거쳐 `connected`가 됩니다.
   서버 로그에는 `ICE 연결됨: ...`이 남습니다.
3. 이어서 DTLS 핸드셰이크가 끝나면 연결 상태(`connectionState`)가 `connected`가 되고,
   페이지에 DTLS 상태 `connected`와 SRTP 암호 `SRTP_AES128_CM_HMAC_SHA1_80`이 표시됩니다.
   서버 로그에는 `DTLS 연결됨: ..., SRTP 프로파일 SRTP_AES128_CM_SHA1_80`이 남습니다.
4. `chrome://webrtc-internals`의 transport 통계에서도 확인할 수 있습니다.

   | 항목 | 기대값 | 의미 |
   |---|---|---|
   | `dtlsState` | `connected` | DTLS 핸드셰이크 완료 |
   | `dtlsRole` | `client` | 서버가 answer에서 `a=setup:passive`로 DTLS 서버를 맡으므로 브라우저는 클라이언트 |
   | `srtpCipher` | `SRTP_AES128_CM_HMAC_SHA1_80` | 협상한 SRTP 프로파일 |

5. 동영상을 골랐다면 페이지의 영상 영역에 그 동영상이 표시되고, 파일 끝에서 처음부터 반복됩니다.
   서버 로그에는 `송신 시작: 세션 ..., 동영상 <ID>, SSRC ..., payload type ...`이 남습니다.
   다른 동영상을 보려면 목록에서 고른 뒤 **연결** 버튼을 다시 누릅니다. 새 연결을 맺어 처음부터 재생합니다.
   `chrome://webrtc-internals`의 inbound-rtp(video) 통계에서 다음을 확인합니다.

   | 항목 | 기대값 | 의미 |
   |---|---|---|
   | `framesPerSecond` | 약 30 | `--video-fps`대로 프레임이 도착해 디코딩됨 |
   | `packetsReceived` | 계속 증가 | SRTP 인증을 통과한 패킷 수 |
   | `framesDecoded` | 계속 증가 | 디코딩한 프레임 수 |
   | `packetsLost` | 0 근처 | 순번이 빠진 패킷 수 |

   탭을 닫거나 연결 버튼을 다시 누르면, 브라우저의 close_notify를 받거나 세션이 만료되는 시점에
   서버 로그에 `송신 중지: ...`가 남고 그 세션으로는 더 보내지 않습니다.

서버는 offer의 `a=fingerprint:sha-256` 값과 핸드셰이크에서 받은 브라우저 인증서를 비교하므로,
offer에 `sha-256` fingerprint가 없거나 `a=setup`이 `actpass`·`active`가 아니면 `400`으로 거부합니다.

### 세션 만료

ICE 세션은 만든 뒤, 또는 마지막 유효 Binding 요청 뒤 30초 동안 유효한 요청이 없으면 그 DTLS 상태와 함께 제거됩니다.
연결된 브라우저는 몇 초마다 동의 확인(consent) 요청을 보내므로 연결을 유지하는 동안에는 만료되지 않습니다.
탭을 닫거나 연결 버튼을 다시 눌러 이전 연결이 닫히면 약 30초 뒤 서버 로그에 `ICE 세션 제거: ...`가 남습니다.

연결이 되지 않으면 Chrome에서 `chrome://webrtc-internals`를 **먼저** 열어 둔 뒤 다시 연결해 봅니다.

- ICE candidate grid에 서버 candidate가 보이는지, candidate pair가 `succeeded`인지 확인합니다.
- Stats의 candidate-pair에서 `requestsSent`는 늘어나는데 `responsesReceived`가 0이면, UDP가 서버에 닿지 않았거나 서버가 요청을 거부한 것입니다.
  서버 로그의 `STUN 요청 무시 (...)`가 거부 원인을 알려 주며, 아무 기록이 없으면 패킷이 서버에 도착하지 않은 것입니다.

## 테스트

```bash
ctest --test-dir build --output-on-failure
```

`tests/`는 `src/`와 같은 디렉터리 구조를 따르며, 모듈별 단위 테스트(`tests/core/`, `tests/crypto/`, `tests/http/`, `tests/media/`, `tests/webrtc/`)와
서버를 실제로 띄워 요청을 보내는 통합 테스트(`tests/integration/`)가 함께 실행됩니다.
`tests/webrtc/data/chrome_offer.sdp`는 실제 Chrome이 만든 offer 견본입니다.

통합 테스트는 기본으로 HTTP 18080 포트와 ICE UDP 18090 포트를 사용합니다.
사용 중이면 `HTTP_SERVER_TEST_PORT`, `ICE_TEST_PORT` 환경 변수로 바꿀 수 있습니다.

```bash
HTTP_SERVER_TEST_PORT=28080 ICE_TEST_PORT=28090 ctest --test-dir build --output-on-failure
```
