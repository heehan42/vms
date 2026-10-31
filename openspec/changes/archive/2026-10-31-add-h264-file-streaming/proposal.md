# Proposal

## Why

DTLS 핸드셰이크가 끝나 브라우저의 `connectionState`는 `connected`가 되고 SRTP 키 재료도 세션에 보관되지만, 서버는 아직 미디어 패킷을 하나도 보내지 않아 브라우저 화면에는 아무것도 나오지 않는다. 프로젝트의 첫 목표인 "서버의 동영상 파일을 WebRTC로 재생"에 도달하려면 SRTP 암호화, RTP 패킷화, 파일 읽기가 모두 있어야 하므로, 이 셋을 한 change로 묶어 브라우저에 첫 영상을 띄운다.

## What Changes

- SRTP 송신 보호를 OpenSSL `libcrypto`로 직접 구현한다(`SRTP_AES128_CM_SHA1_80`).
  - 마스터 키·솔트로 세션 키 유도(AES-CM KDF, 키 유도 주기 0)
  - RTP 페이로드 AES-128 CTR 암호화, HMAC-SHA1 80비트 인증 태그
  - 송신 측 ROC(rollover counter) 관리
  - RFC 3711 부록 B의 테스트 벡터로 검증한다.
- Annex-B H.264 파일을 동영상 소스로 읽는다.
  - 새 명령행 인자 `--video-file <경로>`와 `--video-fps <1-120>`(기본 30)
  - 시작할 때 파일 전체를 읽어 NAL 단위와 액세스 단위(프레임)로 나누고, 첫 프레임이 SPS·PPS·IDR을 포함하는지 확인한다. 조건을 만족하지 않으면 서버가 시작하지 않는다.
  - `--video-file`을 주지 않으면 지금처럼 연결 확인까지만 하고 미디어는 보내지 않는다.
- DTLS가 완료된 세션마다 파일을 처음부터 RTP로 보낸다.
  - RFC 6184 packetization-mode 1: 단일 NAL 패킷과 FU-A. RTP 페이로드는 최대 1200바이트
  - 프레임마다 RTP 타임스탬프(90kHz)를 fps로 계산하고, 이벤트 루프 타이머로 재생 속도에 맞춰 보낸다.
  - 파일 끝에서는 처음부터 다시 보내며, 순번과 타임스탬프는 끊기지 않게 이어 간다.
  - 세션이 제거되거나 DTLS가 닫히면 송신을 멈춘다.
- answer에 세션의 SSRC를 알리는 `a=ssrc`(cname)와 `a=msid`를 넣는다.
- 연결 확인 웹 페이지에 `<video>` 요소를 두고 받은 트랙을 재생한다.
- README에 ffmpeg로 테스트 영상을 만드는 명령과 실행 방법을 적는다. ffmpeg는 개발 도구로만 쓰며 빌드·런타임 의존성이 아니다. 영상 파일은 커밋하지 않는다.

범위 밖(이후 마일스톤):

- RTCP 송수신(SR, PLI, NACK)과 브라우저가 보내는 SRTCP 처리. 브라우저의 RTCP는 지금처럼 버린다.
- MP4 등 컨테이너 파일, 동영상 목록 API와 선택 UI
- 파일 SPS의 프로파일에 맞춰 answer 코덱을 고르는 것. 지금은 기존 선택 규칙(42e01f 우선)을 그대로 쓴다.
- STAP-A 집합 패킷, packetization-mode 0, 오디오
- 프레임 안 패킷 간격 조절(pacing within frame). 프레임 하나의 패킷은 한 번에 보낸다.
- nomination 변경에 따른 송신 경로 갱신

## Capabilities

### New Capabilities

- `srtp`: SRTP 마스터 키로 세션 키를 유도하고 RTP 패킷을 암호화·인증해 SRTP 패킷으로 만드는 패킷 보호 동작.
- `video-file-source`: 서버가 재생할 H.264 동영상 파일을 명령행 인자로 받아 읽고, 프레임 단위로 나누어 검증하는 동영상 소스.
- `rtp-video-streaming`: 연결된 세션에 H.264 프레임을 RTP로 패킷화하고 재생 속도에 맞춰 SRTP로 보내는 송신 동작. 시작·반복·중지 조건을 포함한다.

### Modified Capabilities

- `webrtc-signaling`: answer 내용에 `a=ssrc`와 `a=msid`를 추가한다. 연결 확인 웹 페이지가 받은 영상을 재생하도록 바뀐다.

## Impact

- 새 코드
  - `src/crypto/srtp.h/.cpp`: SRTP 세션 키 유도와 패킷 보호
  - `src/media/h264_file.h/.cpp`: Annex-B 파일 읽기와 프레임 분할, 프레임 단위 `H264Frame`
  - `src/webrtc/rtp_packet.h/.cpp`, `src/webrtc/h264_packetizer.h/.cpp`: RTP 헤더와 H.264 패킷화
  - `src/webrtc/video_stream_sender.h/.cpp`: 세션별 송신 상태(프레임 위치, 순번, 타임스탬프, SRTP)
  - 대응하는 `tests/`
- 바뀌는 코드
  - `src/webrtc/ice_lite_agent.h/.cpp`: DTLS 완료 시 송신 시작, 프레임 타이머, 세션 제거 시 송신 중지
  - `src/webrtc/sdp.h/.cpp`, `src/webrtc/signaling.cpp`: answer의 `a=ssrc`·`a=msid`
  - `src/apps/web_server/main.cpp`: `--video-file`, `--video-fps`
  - `web/index.html`, `web/app.js`, `README.md`, `CMakeLists.txt`
- 의존성: 새 라이브러리는 없다. 이미 링크한 `OpenSSL::Crypto`의 AES와 HMAC을 쓴다.
- 기존 동작: `--video-file` 없이 실행하면 HTTP, 시그널링, ICE, DTLS 동작은 바뀌지 않는다.
