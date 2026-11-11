# vms

카메라 영상과 저장된 동영상을 관리하고 재생하는 VMS(Video Management System)입니다.
먼저 서버의 동영상 파일을 웹 사이트에서 WebRTC로 재생하는 기능부터 구현하고 있으며,
현재는 웹 페이지를 제공하는 HTTP 서버, 브라우저와 ICE 연결을 맺는 시그널링·ICE-lite, 그 연결 위에서
SRTP 키를 정하는 DTLS 핸드셰이크, 그리고 서버의 H.264 파일을 RTP로 패킷화해 SRTP로 보내는 송신까지 구현되어 있습니다.
브라우저 페이지에서 `--video-dir`로 지정한 디렉터리의 동영상을 골라 연결하면 그 영상이 반복 재생됩니다.
`--camera`로 등록한 RTSP 카메라(예: vms-cam 저장소의 `vms_camera_server`)를 고르면 카메라의 실시간 영상이
트랜스코딩 없이 재생됩니다. 서버는 등록한 카메라를 녹화하며, 녹화를 고르면 저장한 영상을 HLS로 다시 재생합니다.

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

시작 로그에는 ICE용 UDP 소켓마다 한 줄이 나옵니다. 한 줄에는 소켓의 주소와 포트, 그 소켓의 candidate 주소가 들어 있습니다.
candidate는 서버가 SDP answer에 적어 브라우저에 알리는 주소입니다. 브라우저는 이 주소로 연결 확인(STUN Binding 요청)을 보냅니다.

```text
ICE 대기 중: udp 172.27.209.81:50000, candidate 172.27.209.81
http://0.0.0.0:8080/ 에서 대기 중 (웹 루트: /home/.../web)
```

| 인자 | 기본값 | 의미 |
|---|---|---|
| `--port` | 8080 | HTTP 포트 |
| `--web-root` | `./web` | 정적 파일 디렉터리 |
| `--ice-port` | 50000 | ICE용 UDP 포트 |
| `--ice-address` | 자동 조회 | `<bind address>[=<announced address>[,<announced address>]...]` 형식의 ICE 주소. 아래 "ICE 주소 지정" 참고 |
| `--video-dir` | 없음 | 재생할 H.264 Annex-B 파일(`.h264`, `.264`)이 든 디렉터리. 없으면 연결 확인까지만 하고 영상은 보내지 않습니다 |
| `--video-fps` | 30 | 영상 재생 속도(1~120). 디렉터리의 모든 파일에 적용하므로 파일을 만들 때의 프레임 속도와 같게 지정합니다 |
| `--camera` | 없음 | `<이름>=rtsp://<호스트>[:<포트>][/<경로>]` 형식으로 RTSP 카메라를 등록합니다. 여러 번 쓸 수 있으며 쓴 순서가 목록 순서입니다. 아래 "카메라 연결" 참고 |
| `--record-dir` | `./recordings` | 카메라 녹화 디렉터리. 아래 "카메라 녹화" 참고 |
| `--record-segment` | 60 | 녹화 파일을 나누는 단위 시간(초, 10~3600) |
| `--record-retention` | 300 | 카메라마다 남길 녹화 분량(초, `--record-segment` 이상 604800 이하) |
| `--record-sync` | 10 | 녹화 파일을 디스크와 동기화하는 주기(초, 1 이상 `--record-segment` 이하) |
| `--record-min-free` | 1024 | 새 녹화 파일을 열기 전에 확보할 최소 여유 공간(MiB, 0~1048576) |

```bash
./build/vms_web_server --port 9000 --web-root /path/to/site --ice-port 50100 --ice-address 127.0.0.1
```

`Ctrl+C`로 종료합니다.

### ICE 주소 지정

`--ice-address` 값 하나는 UDP 소켓 하나를 뜻합니다. 값은 두 부분으로 이루어집니다.

- bind address: 서버가 UDP 소켓을 바인드하는 IPv4 주소입니다. 이 머신의 인터페이스에 있는 주소여야 합니다.
- announced address: 서버가 그 소켓의 candidate로 알리는 IPv4 주소입니다. 이 머신의 인터페이스에 없어도 됩니다.
  예를 들어 공유기의 public address(인터넷에서 쓰는 공인 IP)를 쓸 수 있습니다.

`=`가 없으면 서버는 bind address를 그대로 알립니다.
`=` 뒤에 announced address를 쉼표로 나열하면 서버는 bind address를 알리지 않습니다. 그래서 bind address도 알리려면 목록에 직접 넣습니다.
`--ice-address`는 여러 번 쓸 수 있습니다. candidate의 우선순위는 announced address를 쓴 순서대로 높습니다. 이 순서는 여러 `--ice-address`에 걸쳐 이어집니다.

```bash
./build/vms_web_server --ice-address 127.0.0.1=127.0.0.1,203.0.113.5
```

이 명령의 서버는 `127.0.0.1:50000`에 소켓 하나를 엽니다. 그리고 서버는 candidate 두 개를 알립니다.

```text
ICE 대기 중: udp 127.0.0.1:50000, candidate 127.0.0.1, 203.0.113.5
```

`--ice-address`를 주지 않으면 서버는 켜져 있고 루프백이 아닌 인터페이스의 IPv4 주소를 모두 bind address로 씁니다.
이때 서버는 각 주소를 그대로 알립니다.

다음 경우 서버는 원인과 사용법을 출력합니다. 그다음 서버는 종료 코드 1로 끝납니다.

- `=` 뒤에 주소가 없거나, 쉼표 사이가 비어 있습니다.
- bind address나 announced address가 IPv4 형식이 아닙니다.
- 같은 bind address를 두 번 지정했습니다.
- 같은 announced address가 두 번 나옵니다. 여러 `--ice-address`에 걸쳐 검사합니다.

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
파일 이름을 동영상 이름으로 씁니다. 시작 로그에 동영상마다 이름과 프레임 수, 마지막에 동영상 수와 fps가 나옵니다.

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

### 카메라 연결

카메라 서버 `vms_camera_server`(`~/vms-cam`)를 카메라 없이 테스트 패턴으로 띄웁니다. 빌드 방법은 vms-cam의 README를 봅니다.

```bash
~/vms-cam/build/vms_camera_server --test-source
```

다른 터미널에서 그 RTSP 주소를 카메라 이름 `cam`으로 등록해 서버를 실행합니다.

```bash
./build/vms_web_server --camera cam=rtsp://127.0.0.1:8554/cam
```

시작 로그에 등록한 카메라가 나오고, 연결되면 해상도와 payload type이 남습니다.

```text
카메라 등록: cam, rtsp://127.0.0.1:8554/cam (주소 127.0.0.1:8554)
카메라 목록: 1대
...
카메라 연결 시도: cam, rtsp://127.0.0.1:8554/cam (127.0.0.1:8554)
카메라 연결 성공: cam, 해상도 640x480, payload type 96, 세션 timeout 60초
```

웹 페이지의 **소스** 목록에서 "카메라 · cam"을 골라 연결하면 카메라 영상이 표시됩니다. 파일과 카메라를 함께 쓸 수도 있습니다.

```bash
./build/vms_web_server --video-dir videos --camera cam=rtsp://127.0.0.1:8554/cam
```

- `--camera` 값은 첫 `=`에서 이름과 URL로 나눕니다. `=`가 없거나, 이름이 비었거나, 카메라끼리 이름이 겹치거나, 이름을
  녹화 폴더 이름으로 쓸 수 없거나(`/` 포함, `.`, `..`, 255바이트 초과), URL에 사용자 정보(`user:pass@`, 인증 미지원)가
  있거나, 호스트를 IPv4 주소로 해석할 수 없으면 원인을 출력하고 시작하지 않습니다.
  호스트 이름은 시작할 때 한 번만 해석합니다. 카메라 이름은 파일 동영상 이름과 겹쳐도 됩니다(종류로 구분).
- 서버는 등록한 카메라마다 RTSP 연결 하나를 상시 유지하고, 같은 카메라를 보는 모든 탭이 그 연결을 함께 씁니다.
  제어는 TCP, 영상(RTP)은 UDP로 받습니다. TCP interleaved, 인증, RTSPS는 지원하지 않습니다.
- 시작할 때 카메라가 꺼져 있어도 서버는 시작합니다. 제어 연결이 닫히거나, 요청에 5초 동안 응답이 없거나, 3초 동안
  RTP가 오지 않으면 끊긴 것으로 보고(`카메라 끊김: ...`) 1, 2, 4, 8, 10, 10, ...초 간격으로 다시 연결합니다.
  연결에 성공하면 간격은 다시 1초부터 셉니다.
- 브라우저는 연결 뒤 카메라의 다음 키프레임(vms-cam 기본 1초 간격)부터 받으므로 첫 화면이 최대 1초 늦게 뜹니다.
  패킷이 빠지면 다음 키프레임까지 화면이 멈췄다가 이어집니다.
- 카메라가 끊겨도 브라우저 연결은 유지한 채 기다립니다(`카메라 대기 시작: ...`). 30초 안에 카메라가 다시 연결되면
  같은 탭에서 다음 키프레임부터 영상이 이어집니다(`카메라 대기 끝: ...`). 30초가 지나면 서버가 DTLS close_notify를 보내고
  세션을 제거하므로(`카메라 대기 시간 초과: ...`), 페이지의 연결 상태가 `connected`가 아닌 값으로 바뀝니다.
- `Ctrl+C`로 서버를 끝내면 연결된 카메라에 `TEARDOWN`을 보내고 종료합니다.

### 카메라 녹화

서버는 등록한 카메라를 실행되는 동안 항상 녹화합니다. 카메라가 없으면 녹화 디렉터리를 만들지 않습니다.

```text
recordings/                       # --record-dir
  cam/                            # 카메라 이름
    20261104T100105Z.mp4          # 그 파일 첫 프레임의 시각(UTC, 초 단위)
    20261104T100105Z.idx          # 같은 이름의 색인 파일
    20261104T100200Z.mp4
    20261104T100200Z.idx
```

시작 로그에 녹화 설정이 나오고, 녹화를 시작하거나 멈출 때 카메라 이름과 원인이 남습니다.

```text
녹화 설정: 디렉터리 /home/.../recordings, 단위 60초, 보관 300초, 동기화 10초, 최소 여유 공간 1024MiB
...
녹화 시작: cam
녹화 중단: cam (카메라 연결 끊김)
```

- 녹화 파일은 fragmented MP4입니다. 맨 앞에 SPS·PPS와 해상도를 담은 초기화 구간(`ftyp`, `moov`)이 있고, 그 뒤에
  GOP 하나(키프레임부터 다음 키프레임 직전까지)를 담은 조각(`moof`, `mdat`)이 이어집니다. 오디오는 기록하지 않습니다.
- 파일은 유닉스 시각을 `--record-segment`초로 나눈 단위 구간마다 나눕니다. 새 구간에 속한 첫 키프레임에서 새 파일을
  엽니다. 한 번 닫은 파일은 다시 열지 않으며, 같은 이름이 있으면 `-1`, `-2`를 붙입니다.
- 파일 첫 키프레임을 받은 순간의 서버 시계를 그 파일의 시작 시각으로 삼고, 이후 프레임은 카메라 RTP 타임스탬프
  차이만큼 더합니다.
- 카메라가 끊기면 받은 프레임까지 쓰고 파일을 닫으며, 다시 연결된 뒤 첫 키프레임부터 새 파일에 씁니다. 꺼져 있던
  구간은 어느 파일에도 없습니다.
- 패킷이 빠지면 파일을 닫지 않고 다음 키프레임까지의 프레임을 버립니다. 버리기 직전 프레임을 그 키프레임까지 늘려
  쓰므로, 그 구간은 마지막 화면이 멈춘 채로 재생됩니다(`녹화 프레임 버림: cam (패킷 손실)`, `녹화 재개: cam`).
- 디스크 쓰기는 별도 녹화 스레드가 합니다. 기록을 기다리는 영상이 64MiB 이상일 때 키프레임이 오면 그 GOP부터
  버리고, 패킷 손실과 같이 앞 화면을 멈춘 채로 이어 씁니다(`녹화 프레임 버림: cam (녹화 작업이 밀림)`).
- `--record-sync`초마다, 그리고 파일을 닫을 때 녹화 파일, 색인 파일 순서로 디스크와 동기화합니다. `Ctrl+C`·SIGTERM으로
  끝내면 남은 영상을 모두 쓰고 파일을 닫은 뒤 종료합니다.
- 색인 파일에는 조각마다 시작 시각·길이, 녹화 파일 안 위치·크기, 조각 CRC32, 레코드 CRC32가 들어 있습니다.

보관과 삭제 규칙은 다음과 같습니다.

- **보관 분량:** 파일을 닫을 때마다 그 카메라의 닫힌 파일 길이 합이 `--record-retention`초를 넘으면 가장 오래된
  파일부터 `.mp4`·`.idx`를 함께 지웁니다(`보관 분량 초과로 삭제: ...`). 꺼져 있던 시간은 분량에 넣지 않으므로,
  카메라가 오래 꺼져 있어도 그 전 녹화는 남습니다.
- **비상 삭제:** 새 파일을 열기 전에 녹화 디렉터리가 있는 파일 시스템의 여유 공간이 `--record-min-free`MiB보다 적으면,
  모든 카메라 폴더(지금 등록하지 않은 카메라의 폴더 포함)에서 가장 오래된 단위 구간의 파일을 함께 지웁니다
  (`비상 삭제: 단위 구간 ..., 파일 N개`). 여유 공간이 기준 이상이 되거나 지울 닫힌 파일이 없을 때까지 반복합니다.
  쓰는 중인 파일은 지우지 않습니다. 쓰기가 공간 부족으로 실패하면 같은 삭제를 한 뒤 한 번 다시 씁니다.

서버를 시작할 때 녹화 디렉터리가 있으면 카메라 등록 여부와 관계없이 남은 녹화를 검사합니다. 색인 레코드의 CRC와
조각의 CRC32가 맞는 데까지만 남기고 그 뒤는 잘라 내며, 유효한 조각이 없는 파일과 짝이 없는 파일은 지웁니다.
이름 규칙에 맞지 않는 파일은 건드리지 않습니다.

```text
녹화 복구: cam, 남은 파일 5개, 잘라 낸 데이터 없음
녹화 복구: cam, 남은 파일 5개, 잘라 낸 파일 1개, 지운 파일 0개, 실패 0개
```

닫힌 녹화 파일은 `ffprobe`·`ffplay`로 확인할 수 있습니다.

```bash
ffprobe -v error -show_entries stream=codec_name,width,height:format=duration -of default=nw=1 recordings/cam/20261104T100105Z.mp4
```

```bash
ffplay recordings/cam/20261104T100105Z.mp4
```

### 녹화 재생

웹 페이지에서 "녹화 · <폴더>"를 고르고 연결 버튼을 누르면 그 카메라 폴더의 녹화를 처음부터 재생합니다. `<폴더>`는
녹화 디렉터리 아래의 카메라 폴더 이름이며, 지금 `--camera`로 등록하지 않은 카메라의 폴더도 목록에 나옵니다. 녹화를
재생하는 동안에는 영상 아래에 재생 막대가 나타나 다른 위치로 옮길 수 있고, "녹화 시각" 항목에 지금 보이는 화면이
녹화된 시각(브라우저의 지역 시각)이 표시됩니다. "파일"이나 "카메라"를 고르면 WebRTC 재생으로 돌아가고 재생 막대가
사라집니다.

웹 페이지는 서버가 만든 HLS 재생 목록을 [hls.js](https://github.com/video-dev/hls.js) 1.7.4로 재생합니다. hls.js 배포
파일(`hls.min.js`)과 라이선스 파일(Apache License 2.0)은 `web/vendor/hls.js-1.7.4/`에 있고, 서버가 웹 루트의 정적
파일로 제공하므로 인터넷에 연결되지 않은 네트워크에서도 재생할 수 있습니다. hls.js는 Media Source Extensions가 있는
브라우저에서만 동작하며, Windows Chrome을 확인 대상으로 합니다.

재생할 수 있는 범위는 다음과 같습니다.

- **닫힌 파일만 재생합니다.** 녹화 스레드가 다 쓰고 디스크와 동기화해 닫은 파일만 재생 목록에 들어갑니다. 지금
  쓰고 있는 파일은 단위 구간이 끝나 닫힐 때까지 재생할 수 없으므로, 최근 최대 `--record-segment`초(기본 60초)의
  녹화는 재생되지 않습니다. 카메라를 등록한 뒤 첫 파일이 닫히기 전에는 목록에 "녹화 · <폴더>"가 없습니다.
- **목록은 연결할 때의 상태입니다.** 서버는 연결 버튼을 누를 때 그 순간의 닫힌 파일로 재생 목록을 만듭니다. 그 뒤에
  닫힌 파일을 보려면 연결 버튼을 다시 누릅니다. 소스 목록에 새 폴더가 나타나게 하려면 페이지를 새로 엽니다.
- **꺼졌던 구간은 건너뜁니다.** 카메라가 꺼져 있던 구간은 어느 파일에도 없으므로, 재생은 앞 파일 끝에서 멈추지 않고
  다음 파일로 바로 이어지며 "녹화 시각"이 다음 파일의 시작 시각으로 바뀝니다.
- **재생 중 지워진 파일에서 멈춥니다.** 재생하는 동안 보관 분량 삭제나 비상 삭제로 아직 받지 않은 부분의 파일이
  지워지면, 그 부분을 요청할 때 `404`를 받아 "녹화 재생" 항목에 오류(`networkError fragLoadError` 등)가 표시되고
  재생이 멈춥니다. 연결 버튼을 다시 누르면 남은 파일로 새 재생 목록을 받습니다. 기본 보관 분량(300초)에서 오래된
  부분을 보고 있으면 일어날 수 있습니다.
- **보관 분량이 길면 재생 목록이 커집니다.** 재생 목록에는 녹화 파일의 조각(GOP, vms-cam 기본값으로 약 1초)마다
  약 110바이트의 세 줄이 들어갑니다. 기본 보관 분량 300초면 폴더 하나의 재생 목록이 약 33KB이고, 최대값
  604800초(7일)면 약 66MB입니다. 서버는 재생 목록 요청을 받을 때마다 이벤트 루프 스레드에서 이 문자열을 만들므로,
  보관 분량이 길수록 그동안 다른 요청과 영상 송신이 늦어집니다.

재생에 쓰는 요청은 다음 두 가지입니다. `<폴더>`와 `<파일>` 값은 퍼센트 인코딩합니다(브라우저의
`encodeURIComponent`).

| 요청 | 응답 |
|---|---|
| `GET /api/recordings/playlist.m3u8?camera=<폴더>` | `200`, `Content-Type: application/vnd.apple.mpegurl`, 그 폴더의 닫힌 파일 전체를 시작 시각 순서로 이은 HLS VOD 재생 목록 |
| `GET /api/recordings/file.mp4?camera=<폴더>&file=<파일>` | 녹화 파일 `<녹화 디렉터리>/<폴더>/<파일>.mp4`를 `Content-Type: video/mp4`로 응답. `Range` 요청은 정적 파일과 같은 규칙입니다 |
| 매개변수가 없음, 같은 매개변수가 두 번 옴, 퍼센트 인코딩 오류 | `400` |
| 재생할 녹화가 없는 폴더, 재생 목록에 없는 파일(쓰고 있는 파일, 지운 파일, 없는 이름) | `404` |

`--camera cam=...`으로 녹화하는 서버에서 첫 파일이 닫힌 뒤 다음 명령으로 확인합니다. 소스 목록에 `recording` 원소가
생깁니다.

```bash
curl -s http://localhost:8080/api/sources
```

```json
{"sources":[{"type":"camera","name":"cam"},{"type":"recording","name":"cam"}]}
```

재생 목록은 조각 하나를 세그먼트 하나로 하고, 세그먼트를 녹화 파일의 바이트 범위로 가리킵니다. 파일마다
초기화 구간(`#EXT-X-MAP`)과 파일 시작 시각(`#EXT-X-PROGRAM-DATE-TIME`, UTC)이 있고, 파일이 바뀌는 곳에
`#EXT-X-DISCONTINUITY`가 있습니다.

```bash
curl -s 'http://localhost:8080/api/recordings/playlist.m3u8?camera=cam' | head -n 10
```

```text
#EXTM3U
#EXT-X-VERSION:7
#EXT-X-PLAYLIST-TYPE:VOD
#EXT-X-INDEPENDENT-SEGMENTS
#EXT-X-TARGETDURATION:1
#EXT-X-MAP:URI="/api/recordings/file.mp4?camera=cam&file=20261108T144333Z",BYTERANGE="665@0"
#EXT-X-PROGRAM-DATE-TIME:2026-11-08T14:43:33.224Z
#EXTINF:1.000,
#EXT-X-BYTERANGE:327156@665
/api/recordings/file.mp4?camera=cam&file=20261108T144333Z
```

`#EXT-X-BYTERANGE:<크기>@<시작 위치>`는 녹화 파일의 `<시작 위치>`번째 바이트부터 `<크기>`바이트입니다. 재생기는 이
값을 `Range` 요청으로 바꿉니다. 위 첫 세그먼트(665번째 바이트부터 327156바이트, 끝 위치 665 + 327156 - 1 = 327820)를
직접 요청하면 `206`과 그 크기의 본문을 받습니다. 파일 이름은 재생 목록에 나온 값으로 바꿉니다.

```bash
curl -s -D - -o /dev/null -r 665-327820 'http://localhost:8080/api/recordings/file.mp4?camera=cam&file=20261108T144333Z'
```

```text
HTTP/1.1 206 Partial Content
Content-Type: video/mp4
Content-Length: 327156
Connection: close
Accept-Ranges: bytes
Content-Range: bytes 665-327820/1809284
```

서버는 `HEAD` 요청을 지원하지 않으므로(`405`) 응답 헤더는 위처럼 `GET`의 `-D -`로 봅니다. 재생 목록 전체는
`ffplay`로도 재생할 수 있습니다.

```bash
ffplay 'http://localhost:8080/api/recordings/playlist.m3u8?camera=cam'
```

### 소스 목록과 선택 API

웹 페이지는 `GET /api/sources`로 소스 목록을 받고, 고른 소스의 종류(`type`)와 이름(`name`)을 `POST /offer`의
쿼리 매개변수로 보냅니다. 목록의 `type`은 `file`(동영상 파일), `camera`(카메라), `recording`(카메라 폴더 하나의
녹화) 중 하나입니다. 목록에는 `file` 원소를 이름순으로 먼저, `camera` 원소를 `--camera`로 등록한 순서대로 그 뒤에,
`recording` 원소를 폴더 이름순으로 마지막에 둡니다. 카메라는 연결 상태와 관계없이 항상 목록에 있습니다.
`frames`(프레임 수)는 `file` 원소에만 있습니다. `POST /offer`에 쓸 수 있는 `type`은 `file`과 `camera`이며,
`recording` 원소의 `name`은 [녹화 재생](#녹화-재생)의 재생 목록 요청에 `camera` 값으로 씁니다.

```bash
curl -s http://localhost:8080/api/sources
```

```json
{"sources":[{"type":"file","name":"color_bars.h264","frames":300},{"type":"file","name":"test_pattern.h264","frames":300},{"type":"camera","name":"cam"},{"type":"recording","name":"cam"}]}
```

| `POST /offer` 요청 대상 | 결과 |
|---|---|
| `/offer?type=file&name=color_bars.h264` | 그 파일을 첫 프레임부터 보내는 세션 |
| `/offer?type=camera&name=cam` | 그 카메라의 영상을 다음 키프레임부터 보내는 세션 |
| `/offer` | 영상 없는 세션. 이전 형식의 `video` 매개변수도 무시하므로 같은 결과입니다 |
| `type`·`name` 중 하나만 있음, 같은 매개변수가 두 번 옴, 퍼센트 인코딩 오류, `type`이 `file`·`camera`가 아님 | `400` |
| 종류와 이름이 맞는 소스가 없음 | `404` |

이름에 한글이나 `&` 같은 문자가 있으면 값을 퍼센트 인코딩합니다(브라우저의 `encodeURIComponent`).

### 정적 파일의 범위 요청

서버는 파일 응답에서 `Range` 헤더의 범위 하나(`bytes=a-b`, `bytes=a-`, `bytes=-n`)를 처리합니다.
서버를 실행한 상태에서 다음 명령으로 `app.js`의 처음 10바이트(0번부터 9번까지)를 요청합니다.

```bash
curl -r 0-9 -i http://localhost:8080/app.js
```

응답은 `206 Partial Content`이고, `Content-Range`의 `/` 뒤 숫자는 `app.js` 전체 크기(바이트)입니다.
본문은 파일의 처음 10바이트라서 한글 글자 중간에서 끊길 수 있습니다.

```text
HTTP/1.1 206 Partial Content
Content-Type: text/javascript; charset=utf-8
Content-Length: 10
Connection: close
Accept-Ranges: bytes
Content-Range: bytes 0-9/6746
```

| 요청 | 응답 |
|---|---|
| 범위 하나이고 시작 위치가 파일 크기보다 작음 | `206`과 그 범위의 바이트. 끝 위치가 파일 끝을 넘으면 파일 끝까지 |
| 시작 위치가 파일 크기 이상, `bytes=-0` | `416 Range Not Satisfiable`, `Content-Range: bytes */<파일 크기>` |
| 범위가 둘 이상, 형식 오류, `bytes`가 아닌 단위, `If-Range`가 함께 옴, `Range`가 여러 번 옴 | `Range`를 무시하고 파일 전체를 `200` |

파일 내용을 담은 `200`과 `206` 응답에는 `Accept-Ranges: bytes`가 들어 있습니다.

## WSL2에서 실행하기

WSL2의 네트워크 방식은 두 가지입니다. 방식마다 접속할 수 있는 브라우저가 다릅니다.

| 방식 | 같은 PC의 Windows 브라우저 | 다른 기기의 브라우저 |
|---|---|---|
| NAT mode(기본값) | 접속 가능 | 접속 불가 |
| mirrored mode | `--ice-address 127.0.0.1`을 더하면 접속 가능 | 접속 가능 |

### NAT mode

NAT mode는 WSL2의 기본 네트워크 방식입니다. WSL2는 Windows 안의 가상 네트워크에 있습니다.
WSL2의 `eth0` 주소는 `172.x.x.x` 같은 주소입니다. NAT mode에서는 별도 설정 없이 서버를 실행합니다.

- HTTP: Windows 브라우저에서 `http://localhost:8080/`으로 접속합니다. WSL2의 localhost 포워딩이 이 TCP 연결을 WSL2로 전달합니다.
- ICE(UDP): localhost 포워딩은 TCP만 전달합니다. 대신 Windows는 WSL2의 `eth0` 주소로 UDP를 직접 보낼 수 있습니다.
  `--ice-address`의 자동 조회가 이 주소를 candidate로 알립니다. 그래서 `--ice-address`를 지정할 필요가 없습니다.

`eth0` 주소는 WSL2를 다시 시작할 때마다 바뀝니다. 서버를 다시 시작하면 자동 조회가 새 주소를 찾습니다.
NAT mode에서는 Windows 밖의 기기가 `eth0` 주소로 패킷을 보낼 수 없습니다. 그래서 다른 기기의 브라우저는 접속할 수 없습니다.

### mirrored mode

mirrored mode에서는 WSL2가 Windows의 네트워크 인터페이스를 함께 씁니다. 그래서 서버는 Windows의 LAN 주소를 bind address로 쓸 수 있습니다.
다른 기기의 브라우저로 접속하려면 mirrored mode가 필요합니다.

1. `%UserProfile%\.wslconfig`의 `[wsl2]` 절에 다음 줄을 씁니다.

   ```ini
   [wsl2]
   networkingMode=mirrored
   ```

2. PowerShell에서 `wsl --shutdown`을 실행해 WSL2를 다시 시작합니다.
3. WSL2 셸에서 `wslinfo --networking-mode`를 실행합니다. 결과가 `mirrored`이면 설정이 적용된 것입니다.

mirrored mode에서 Windows가 자기 LAN 주소로 보낸 패킷은 기본 설정에서 WSL2로 전달되지 않습니다.
그래서 같은 PC의 Windows 브라우저는 LAN 주소 candidate로 서버에 연결할 수 없습니다.
반면 Windows의 `localhost`로 보낸 TCP와 UDP는 WSL2로 전달됩니다. 그래서 같은 PC의 Windows 브라우저를 위해 `--ice-address 127.0.0.1`을 더합니다.

```bash
./build/vms_web_server --ice-address 192.168.219.105 --ice-address 127.0.0.1
```

`192.168.219.105`는 개발 PC의 LAN 주소 예입니다. `ip -4 addr`로 실제 주소를 확인합니다.
같은 PC의 Windows 브라우저는 `http://localhost:8080/`으로 접속합니다. 같은 Wi-Fi의 기기는 `http://192.168.219.105:8080/`으로 접속합니다.
다른 기기에서 접속이 되지 않으면 아래 "외부 네트워크에서 접속하기"의 Hyper-V 방화벽 규칙을 먼저 더합니다.

`.wslconfig`의 `[experimental]` 절에 `hostAddressLoopback=true`를 쓰는 방법도 있습니다.
이 설정은 Windows가 자기 LAN 주소로 보낸 패킷을 WSL2로 전달합니다. 이 설정은 실험 기능이므로 WSL 버전에 따라 동작이 바뀔 수 있습니다.
이 프로젝트는 이 설정을 시험하지 않았습니다.

## 외부 네트워크에서 접속하기

이 절은 LTE 휴대폰처럼 집 밖의 브라우저가 집의 개발 PC에 접속하는 절차입니다.
개발 PC는 WSL2를 [mirrored mode](#mirrored-mode)로 실행한다고 가정합니다.

> **경고:** 서버에는 인증과 HTTPS가 없습니다. 공유기에 port forwarding 규칙을 켜면 인터넷의 누구나 실시간 영상과 녹화를 볼 수 있습니다.
> 그래서 외부 데모를 할 때만 port forwarding 규칙을 켭니다. 데모가 끝나면 규칙을 끕니다.

이 절의 예는 다음 주소를 씁니다.

| 주소 | 예 | 뜻 |
|---|---|---|
| LAN 주소 | `192.168.219.105` | 공유기가 개발 PC에 준 private address(내부망에서만 쓰는 주소) |
| public address | `203.0.113.5` | 통신사가 공유기에 준 공인 IP. `curl -4 ifconfig.me`로 확인합니다 |

### 1. CGNAT 확인

CGNAT는 통신사가 공유기 바깥에 한 번 더 두는 NAT입니다. CGNAT 환경에서는 공유기에 port forwarding 규칙을 넣어도 외부 패킷이 공유기에 도착하지 않습니다.
그래서 먼저 회선이 CGNAT인지 확인합니다. 확인 방법은 두 가지입니다.

- 공유기 관리 화면의 WAN 주소와 `curl -4 ifconfig.me`의 결과를 비교합니다. 두 주소가 다르면 CGNAT입니다.
- Windows 명령 프롬프트에서 `tracert -d 8.8.8.8`을 실행합니다. 두 번째 hop의 주소가 `100.64.0.0/10` 범위이면 CGNAT입니다.

CGNAT이면 이 절차로는 외부에서 접속할 수 없습니다.

### 2. Hyper-V 방화벽 규칙

Hyper-V 방화벽은 WSL2로 들어오는 연결을 거르는 Windows 방화벽입니다. 관리자 권한 PowerShell에서 HTTP 포트와 ICE 포트의 규칙을 더합니다.
`{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}`은 Microsoft가 공개한 WSL의 VM 식별자입니다.

```powershell
New-NetFirewallHyperVRule -Name "vms-http" -DisplayName "vms HTTP" -Direction Inbound -VMCreatorId "{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}" -Protocol TCP -LocalPorts 8080
New-NetFirewallHyperVRule -Name "vms-ice" -DisplayName "vms ICE" -Direction Inbound -VMCreatorId "{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}" -Protocol UDP -LocalPorts 50000
```

이 규칙은 출발지와 관계없이 두 포트의 연결을 허용합니다. 외부 기기의 접근은 공유기의 port forwarding 규칙이 정합니다.

### 3. 공유기 설정

공유기 관리 화면에서 두 가지를 설정합니다.

1. DHCP 고정 할당: 개발 PC의 MAC 주소에 LAN 주소 `192.168.219.105`를 지정합니다.
   그러면 port forwarding 규칙과 `--ice-address`의 LAN 주소가 계속 맞습니다.
2. port forwarding 규칙: 다음 두 규칙을 더합니다. TCP 규칙은 UDP 패킷에 적용되지 않으므로 두 규칙이 모두 필요합니다.

   | 프로토콜 | 외부 포트 | 내부 주소 | 내부 포트 |
   |---|---|---|---|
   | TCP | 8080 | `192.168.219.105` | 8080 |
   | UDP | 50000 | `192.168.219.105` | 50000 |

### 4. 서버 실행

```bash
./build/vms_web_server --ice-address 192.168.219.105=192.168.219.105,203.0.113.5 --ice-address 127.0.0.1
```

이 명령의 서버는 [bind address](#ice-주소-지정)마다 UDP 소켓을 하나씩, 모두 두 개 엽니다. 그리고 서버는 candidate 세 개를 이 순서로 알립니다.

1. `192.168.219.105`: 같은 Wi-Fi의 기기가 연결합니다.
2. `203.0.113.5`: 외부 기기가 연결합니다. 공유기는 이 주소로 온 패킷을 `192.168.219.105:50000`의 소켓으로 전달합니다.
3. `127.0.0.1`: 같은 PC의 Windows 브라우저가 연결합니다.

LAN 주소를 public address보다 앞에 둡니다. 일부 공유기는 내부 기기가 public address로 보낸 패킷을 내부로 되돌려 보내지 않습니다(NAT hairpinning 미지원).
그래도 같은 Wi-Fi의 기기는 앞의 LAN 주소 candidate로 연결합니다.

시작 로그는 다음과 같습니다.

```text
ICE 대기 중: udp 192.168.219.105:50000, candidate 192.168.219.105, 203.0.113.5
ICE 대기 중: udp 127.0.0.1:50000, candidate 127.0.0.1
```

외부 브라우저에는 LAN 주소와 `127.0.0.1` candidate도 전달됩니다. 외부 브라우저는 이 주소로 연결하지 못하고 public address로 연결합니다.
LAN 주소를 외부에 알리고 싶지 않으면 public address만 [announced address](#ice-주소-지정)로 지정합니다.

### 5. 외부 브라우저에서 접속

외부 브라우저에서 `http://203.0.113.5:8080/`으로 접속합니다.
연결되면 서버 로그의 `ICE 연결됨` 줄에 휴대폰의 통신사 쪽 주소가 남습니다.

### public address가 바뀌었을 때

가정용 public address는 공유기를 다시 켜거나 시간이 오래 지나면 바뀔 수 있습니다.
public address가 바뀌어도 새 주소로 웹 페이지는 열립니다. 하지만 answer의 candidate는 예전 주소를 가리킵니다. 그래서 WebRTC 연결만 실패합니다.

이 증상이 보이면 `curl -4 ifconfig.me`의 결과를 시작 로그의 candidate 주소와 비교합니다.
두 주소가 다르면 새 주소를 `--ice-address`에 넣어 서버를 다시 실행합니다.

## 연결 확인

1. 서버를 실행하고 Windows Chrome에서 `http://localhost:8080/`에 접속합니다.
   "app.js가 실행되었습니다." 문구가 보이면 페이지와 스크립트가 정상적으로 전달된 것입니다.
   **소스** 선택 목록에는 서버의 동영상이 "파일 · <이름>" 형식으로 이름순으로, 그 뒤에 카메라가 "카메라 · <이름>" 형식으로
   나오고 첫 항목이 선택되어 있습니다. `--video-dir`과 `--camera` 없이 실행했으면 "소스 없음"이 표시되며, 연결은 영상 없이 맺어집니다.
2. 소스를 고르고 **연결** 버튼을 누르면 시그널링이 "answer 적용 완료"가 되고, ICE 상태가 `checking`을 거쳐 `connected`가 됩니다.
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

5. 소스를 골랐다면 페이지의 영상 영역에 그 동영상이 표시되고, 파일 끝에서 처음부터 반복됩니다.
   서버 로그에는 `송신 시작: 세션 ..., 소스 file <이름>, SSRC ..., payload type ...`이 남습니다(카메라는 `소스 camera <이름>`).
   다른 소스를 보려면 목록에서 고른 뒤 **연결** 버튼을 다시 누릅니다. 새 연결을 맺어 처음부터 재생합니다.
   `chrome://webrtc-internals`의 inbound-rtp(video) 통계에서 다음을 확인합니다.

   | 항목 | 기대값 | 의미 |
   |---|---|---|
   | `framesPerSecond` | 약 30 | 파일은 `--video-fps`대로, 카메라는 카메라의 fps대로 프레임이 도착해 디코딩됨 |
   | `packetsReceived` | 계속 증가 | SRTP 인증을 통과한 패킷 수 |
   | `framesDecoded` | 계속 증가 | 디코딩한 프레임 수 |
   | `packetsLost` | 0 근처 | 순번이 빠진 패킷 수 |

   탭을 닫거나 연결 버튼을 다시 누르면, 브라우저의 close_notify를 받거나 세션이 만료되는 시점에
   서버 로그에 `송신 중지: ...`가 남고 그 세션으로는 더 보내지 않습니다.
6. 연결 뒤 몇 초 안에 서버 로그에 `RTCP 수신 시작: 세션 ..., 브라우저 ...: RR(보낸 쪽 SSRC ..., 대상 SSRC ..., ...)`이 한 번 남습니다.
   RR의 대상 SSRC는 `송신 시작` 로그와 answer의 `a=ssrc` 값과 같습니다. 브라우저가 서버 영상에 대해 보낸 수신 보고를 받았다는 뜻입니다.
   Chrome은 RR을 SDES 없이 보내기도 하고 PSFB(PT 206)를 함께 보내기도 하므로, 요약의 종류별 개수에 `SDES`가 없거나 `PSFB`가 있어도 정상입니다.
   이후 RTCP는 개수만 세며, `RTCP 버림: ...`은 남지 않아야 합니다.
   탭을 닫아 세션이 제거되면(아래 "세션 만료") `ICE 세션 제거` 직전에 `RTCP 수신 요약: ...`이 남고,
   그 안의 `버림: 인증 실패 0, 재생 0, 길이 부족 0, 형식 오류 0`처럼 실패가 모두 0이어야 합니다.

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

`tests/`는 `src/`와 같은 디렉터리 구조를 따르며, 모듈별 단위 테스트(`tests/core/`, `tests/crypto/`, `tests/http/`, `tests/media/`, `tests/rtsp/`, `tests/webrtc/`)와
서버를 실제로 띄워 요청을 보내는 통합 테스트(`tests/integration/`)가 함께 실행됩니다.
`tests/webrtc/data/chrome_offer.sdp`는 실제 Chrome이 만든 offer 견본입니다.

통합 테스트는 기본으로 HTTP 18080 포트와 ICE UDP 18090 포트를 사용합니다.
사용 중이면 `HTTP_SERVER_TEST_PORT`, `ICE_TEST_PORT` 환경 변수로 바꿀 수 있습니다.

```bash
HTTP_SERVER_TEST_PORT=28080 ICE_TEST_PORT=28090 ctest --test-dir build --output-on-failure
```
