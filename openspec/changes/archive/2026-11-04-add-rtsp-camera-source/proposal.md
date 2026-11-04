# Proposal

## Why

지금 서버는 미리 저장한 H.264 파일만 보낼 수 있다. 프로젝트의 다음 목표는 카메라의 실시간 영상을 웹 사이트에서 보는 것이다. `~/vms-cam`의 `vms_camera_server`는 V4L2 카메라 영상을 H.264 Constrained Baseline(B 프레임 없음, 키프레임마다 SPS/PPS)으로 인코딩해 RTSP로 제공한다. 이 프로파일은 브라우저와 협상하는 `42e01f`에 그대로 맞으므로, 트랜스코딩 없이 RTP를 받아 다시 보내면 된다. 앞선 change(`add-source-catalog`)에서 목록과 offer가 이미 "종류 + 이름"로 소스를 가리키게 되어 있어, 카메라를 `camera` 종류로 추가할 자리가 있다.

## What Changes

- `--camera <이름>=<rtsp URL>` 명령행 인자를 추가한다.
  - 여러 번 쓸 수 있고, 쓴 순서가 목록 순서다.
  - 이름이 비었거나 카메라끼리 겹치거나 UTF-8이 아니면 시작하지 않는다.
  - URL은 `rtsp://<호스트>[:<포트>]/<경로>` 형식이다. 사용자 정보(`user:pass@`)가 있거나 호스트를 IPv4 주소로 해석할 수 없으면 시작하지 않는다.
- 서버는 등록한 카메라마다 RTSP 클라이언트로 상시 연결한다.
  - RTSP 제어는 TCP로 한다(`OPTIONS` → `DESCRIBE` → `SETUP` → `PLAY`, 주기적인 `OPTIONS`로 세션 유지).
  - RTP와 RTCP는 UDP로 받는다. SETUP의 `client_port`로 연속된 짝수·홀수 포트 쌍을 알린다.
  - 받은 RTP(단일 NAL, STAP-A, FU-A)를 H.264 프레임으로 재조립한다.
  - 제어 연결이 닫히거나 3초 동안 RTP가 없거나 요청에 응답이 없으면 끊긴 것으로 보고, 1초부터 최대 10초까지 늘어나는 간격으로 다시 연결한다.
  - 시작할 때 카메라에 연결되지 않아도 서버는 시작한다.
- `GET /api/sources`에 카메라를 `{"type":"camera","name":"<이름>"}`로, 파일 뒤에 등록 순서대로 넣는다.
- `POST /offer?type=camera&name=<이름>`로 카메라를 세션에 지정할 수 있다.
  - DTLS가 끝나면 세션은 카메라의 다음 키프레임(SPS·PPS·IDR)부터 받는다.
  - 프레임이 도착하는 즉시 보내고, 카메라 타임스탬프의 간격을 그대로 유지한다.
  - 카메라 RTP가 빠지면 그 세션은 다음 키프레임까지 보내지 않는다.
- 카메라가 끊겨도 WebRTC 세션은 유지하고 기다린다.
  - 30초 안에 카메라가 다시 연결되면 다음 키프레임부터 이어서 보낸다. 순번은 이어 가고, 타임스탬프는 끊긴 시간만큼 건너뛴다.
  - 30초가 지나면 서버가 DTLS close_notify를 보내고 세션을 제거한다.
- 파일 소스의 송신 동작(첫 프레임부터, `--video-fps` 간격, 반복 재생)은 바뀌지 않는다.
- README에 vms-cam과 함께 실행하는 방법을 적는다.

범위 밖:

- RTSP over TCP(interleaved), 멀티캐스트, 인증, RTSPS
- RTCP RR 송신, 지터 버퍼, 순서가 바뀐 패킷의 재정렬, 브라우저 PLI에 따른 키프레임 요청
- 카메라 연결 상태를 목록이나 페이지에 표시하는 것
- 카메라 녹화
- 설정 파일로 카메라 등록. 나중에 명령행 인자를 대체할 수 있다.

## Capabilities

### New Capabilities

- `rtsp-camera-source`: 명령행으로 등록한 RTSP 카메라에 연결해 H.264 프레임을 받아 오는 동작. 연결 절차, UDP 수신, 재조립, 끊김 감지, 재연결을 다룬다.

### Modified Capabilities

- `video-catalog`: 목록에 카메라 원소를 등록 순서대로 넣는다.
- `rtp-video-streaming`: 송신 시작 조건과 재생 속도·반복 재생 규칙을 파일 소스로 한정하고, 카메라 소스의 송신(키프레임부터 시작, 타임스탬프 유지, 손실 회복, 끊김 대기와 세션 종료)을 추가한다.

## Impact

- 새 코드
  - `src/rtsp/`: RTSP URL 해석, 요청 생성과 응답 해석, H.264 RTP 재조립, 카메라 클라이언트
  - `src/media/live_video_source.h/.cpp`: 카메라 프레임을 여러 세션에 나눠 주는 실시간 소스
  - `src/webrtc/`의 카메라 송신 상태. 파일 송신과 패킷화·SRTP 보호 부분을 함께 쓴다.
  - 대응하는 `tests/`
- 바뀌는 코드
  - `src/webrtc/dtls_transport.h/.cpp`: 서버 쪽에서 close_notify를 보내는 종료
  - `src/webrtc/ice_lite_agent.h/.cpp`: 세션의 카메라 지정, 대기 타이머
  - `src/webrtc/video_stream_sender.h/.cpp`: 패킷화·SRTP 보호 부분 분리
  - `src/webrtc/signaling.h/.cpp`: 카메라 찾기와 목록 원소
  - `src/apps/web_server/main.cpp`: `--camera`, 카메라 클라이언트 시작
  - `README.md`, `CMakeLists.txt`
- 의존성: 새 라이브러리는 없다. 호스트 이름 해석에 libc `getaddrinfo`를 시작할 때 한 번 쓴다.
- 호환성: 기존 인자와 API 형식은 그대로다.
