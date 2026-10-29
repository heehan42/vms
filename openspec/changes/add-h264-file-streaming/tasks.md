# Tasks

## 1. SRTP 송신 보호

- [ ] 1.1 `src/crypto/srtp.h/.cpp`에 design.md 결정 3의 `SrtpSessionKeys`, `DeriveSrtpSessionKeys`, `XorAesCmKeystream`을 구현하고 `CMakeLists.txt`의 `server_core`에 추가한다. `tests/crypto/srtp_test.cpp`를 만들어 다음을 확인한다.
  - RFC 3711 부록 B.3의 마스터 키·솔트로 유도한 암호화 키, 인증 키(앞 20바이트), 세션 솔트가 기대값과 같다
  - 부록 B.2의 세션 키와 IV로 만든 키스트림이 기대값과 같다(0으로 채운 버퍼에 XOR)
- [ ] 1.2 같은 파일에 `SrtpSender`(`Create`, `Protect`, 송신 ROC, 재사용 `EVP_CIPHER_CTX`, 소멸 시 `OPENSSL_cleanse`)를 구현한다. `tests/crypto/srtp_test_receiver.h/.cpp`에 공개 함수와 `ComputeHmacSha1`로 태그를 검증하고 복호화하는 테스트 전용 수신 측(순번 순환으로 ROC 계산)을 만들어 테스트 전용 라이브러리로 등록한다. `srtp_test`에 다음 시나리오를 추가해 통과하는지 확인한다.
  - 보호 결과의 헤더는 원본과 같고, 페이로드 길이는 같으며 내용은 다르고, 끝에 10바이트가 붙는다
  - 수신 측이 태그를 검증하고 복호화한 결과가 원본 RTP 패킷과 같다
  - CSRC가 있는 헤더도 헤더 길이를 올바르게 계산한다
  - 순번 65535 다음 0인 패킷은 ROC 1로 보호되어 수신 측 복호화가 통과하고, ROC 0을 가정하면 태그 검증이 실패한다
  - 태그 한 바이트를 바꾸면 수신 측 검증이 실패한다

## 2. H.264 파일 소스

- [ ] 2.1 `src/media/h264_file.h/.cpp`에 design.md 결정 4의 `H264Frame`(NAL 뷰 목록)과 `H264File::FromBytes`(NAL 분할, 프레임 묶기, 첫 프레임 검증), `Frames()`를 구현하고(복사 금지, 이동 허용) `server_core`와 `tests/media/`(새 하위 디렉터리, `tests/CMakeLists.txt`에 추가)에 등록한다. `tests/media/h264_file_test.cpp`로 다음을 확인한다.
  - 3바이트와 4바이트 시작 코드가 섞인 데이터에서 NAL 경계와 내용(시작 코드·끝 0 바이트 제외)
  - SPS, PPS, IDR, 슬라이스, 슬라이스 → 프레임 3개
  - `first_mb_in_slice`가 0이 아닌 슬라이스는 앞 프레임에 포함
  - SEI·AUD가 새 프레임을 시작하고, 끝의 슬라이스 없는 프레임은 버림
  - 시작 코드 없음, 첫 프레임에 IDR 없음, SPS 없음의 오류 문구
- [ ] 2.2 `H264File::Load(path)`로 파일 크기 확인(64MiB 상한)과 전체 읽기를 구현한다. `h264_file_test`에 임시 파일로 정상 읽기, 없는 경로, 64MiB를 넘는 희소 파일의 오류를 추가해 통과하는지 확인한다

## 3. RTP 패킷화

- [ ] 3.1 `src/webrtc/rtp_packet.h/.cpp`에 RTP 고정 헤더 쓰기(버전 2, marker, payload type, 순번, 타임스탬프, SSRC)를 구현한다. `tests/webrtc/rtp_packet_test.cpp`로 V=2·M=1·PT=102일 때 첫 두 바이트가 `0x80 0xE6`이고 나머지 필드가 네트워크 바이트 순서로 들어가는지 확인한다
- [ ] 3.2 `src/webrtc/h264_packetizer.h/.cpp`에 design.md 결정 5의 `PacketizeH264Frame`(단일 NAL, FU-A, AUD 건너뛰기, 최대 페이로드 1200)을 구현한다. `tests/webrtc/h264_packetizer_test.cpp`로 다음을 확인한다.
  - 100바이트 NAL → 그대로인 페이로드 1개
  - 1200바이트 NAL → 단일 NAL 패킷, 1201바이트 NAL → FU-A 2개
  - 헤더 `0x65`인 3000바이트 NAL → FU-A 3개(indicator `0x7C`, header `0x85`·`0x05`·`0x45`, 본문 1198·1198·603)이고 재조립하면 원본과 같다
  - 프레임에 AUD가 있으면 그 NAL은 결과에 없다

## 4. 세션별 송신 상태

- [ ] 4.1 `src/webrtc/video_stream_sender.h/.cpp`에 design.md 결정 6의 `VideoStreamSender`(`Create`, `SendDueFrame(now)`, 프레임 위치 n mod N, 타임스탬프 계산, 일정 재설정, marker 비트, SRTP 보호)를 구현한다. `tests/webrtc/video_stream_sender_test.cpp`에서 합성 `H264File`과 테스트 수신 측으로 시각을 직접 넘기며 다음을 확인한다.
  - 30fps에서 n번째 프레임의 예정 시각이 시작 + n × 33.33ms이고 이웃 프레임 타임스탬프 차이가 3000이다
  - fps 7에서 타임스탬프가 ⌊n × 90000 / 7⌋로 계산되어 누적 오차가 없다
  - 프레임 수 3인 파일에서 5프레임을 보내면 소스 위치가 0,1,2,0,1이고 순번과 타임스탬프가 끊기지 않는다
  - 순번 초기값이 65534일 때 순환해도 수신 측 복호화가 통과한다
  - 예정 시각보다 5프레임 늦게 호출하면 프레임 하나만 보내고 다음 예정 시각이 현재 + 한 프레임 간격이다
  - 프레임마다 마지막 패킷에만 marker 비트가 있고 payload type과 SSRC가 생성 값과 같다

## 5. SDP answer와 시그널링

- [ ] 5.1 `src/webrtc/sdp.h/.cpp`의 `SdpAnswerParameters`에 `ssrc`, `cname`, `media_stream_id`, `track_id`를 추가하고 design.md 결정 8의 `a=msid`, `a=ssrc` 줄을 협상 대상 섹션에 넣는다. `sdp_test`에 두 줄의 형식과 위치(거절한 섹션에는 없음)를 확인하는 시나리오를 추가해 기존 시나리오와 함께 통과하는지 확인한다
- [ ] 5.2 `IceLiteAgent::CreateSession`을 `IceSessionRequest`(ufrag, fingerprint, payload type)를 받고 `IceSessionParameters`(자격 증명, 겹치지 않는 0 아닌 무작위 SSRC)를 돌려주도록 바꾸고, `Session`에 SSRC와 payload type을 보관한다. `src/webrtc/signaling.cpp`에서 payload type을 0~127 정수로 바꿔 넘기고(범위 밖이면 `400`), cname을 만들어 answer 매개변수를 채운다. `ice_lite_agent_test`의 기존 호출을 고치고 두 세션의 SSRC가 다른지 확인한다. `tests/integration/http_server_test.sh`에 answer의 `a=ssrc`·`a=msid` 존재와 두 answer의 SSRC가 다른지 확인하는 시나리오를 추가해 `ctest --test-dir build --output-on-failure`가 통과하는지 확인한다

## 6. 에이전트의 송신 연동

- [ ] 6.1 `src/webrtc/ice_lite_agent.h/.cpp`에 design.md 결정 7을 구현한다.
  - `IceLiteAgentConfig::video_file`(`const media::H264File*`, 기본 `nullptr`)과 `video_fps`
  - `FinishDtlsStep`의 `kConnected` 전이에서 `VideoStreamSender` 생성, 0ms 프레임 타이머, "송신 시작" 로그(세션, SSRC, payload type)
  - `HandleFrameTimer`: 패킷을 nominated pair로 송신, 다음 예정 시각으로 타이머 재설정
  - `kClosed`·`kFailed` 전이와 `RemoveSession`·소멸자에서 프레임 타이머 취소와 "송신 중지" 로그
  - `sendto` 실패 시 패킷 버림과 세션당 첫 실패 로그
  - `SrtpSender` 생성이나 보호 실패 시 로그 후 그 세션 송신 중지

  `ice_lite_agent_test`에 프레임 3개짜리 합성 `H264File`과 높은 fps(예: 100)로 다음 시나리오를 추가해 기존 시나리오와 함께 통과하는지 확인한다.
  - 핸드셰이크 뒤 테스트 클라이언트가 받은 패킷을 `ExportSrtpKeyingMaterial`의 서버용 키로 복호화하면 SSRC·payload type이 `CreateSession`의 값과 같고, 순번이 연속되며, 재조립한 첫 5프레임이 소스 0,1,2,0,1과 같다
  - close_notify를 보낸 뒤에는 패킷이 더 오지 않는다(잠시 기다려 확인)
  - 두 세션이 동시에 받고, 한 세션의 close_notify 뒤에도 다른 세션은 계속 받는다
  - `video_file`이 없는 에이전트는 핸드셰이크 뒤 RTP를 보내지 않는다
  - 송신 중인 세션이 만료되면 패킷이 더 오지 않는다

## 7. 명령행 인자

- [ ] 7.1 `src/apps/web_server/main.cpp`에 `--video-file`과 `--video-fps <1-120>`(기본 30)을 추가하고 사용법 문구를 고친다. `--video-file`이 있으면 에이전트보다 먼저 `H264File::Load`로 읽고 경로·프레임 수·fps를 로그에 남기며, 실패하면 원인 출력 후 0이 아닌 코드로 종료한다. `http_server_test.sh`에 `--video-fps 0`, `--video-fps abc`, 없는 `--video-file`, 시작 코드 없는 텍스트 파일의 0이 아닌 종료 코드 시나리오를 추가해 통과하는지 확인한다

## 8. 웹 페이지와 문서

- [ ] 8.1 `web/index.html`에 `<video id="remote-video" autoplay muted playsinline>`을 추가하고, `web/app.js`에서 `track` 이벤트로 `srcObject`를 설정하며 다시 연결할 때 이전 영상을 비운다(design.md 결정 8). 통합 테스트의 `GET /` 검사가 계속 통과하는지 확인한다
- [ ] 8.2 `README.md`에 다음을 정리하고, 적은 ffmpeg 명령을 그대로 실행해 파일이 만들어지는지 확인한다.
  - ffmpeg 설치(`sudo apt install ffmpeg`)와 테스트 영상 생성 명령(background.md "ffmpeg 옵션"의 옵션), 영상 파일은 커밋하지 않는다는 점
  - `--video-file`, `--video-fps` 사용법과 실행 예
  - 확인 기준: 페이지의 영상 표시, `chrome://webrtc-internals` inbound-rtp의 `framesPerSecond`·`packetsReceived`

## 9. 통합 검증

- [ ] 9.1 README 명령으로 만든 파일을 지정해 WSL2에서 서버를 실행하고 Windows Chrome에서 연결한다. 다음을 확인한다.
  - 페이지의 `<video>`에 테스트 패턴이 움직이며 표시되고, 10초가 지나도 멈추지 않고 처음부터 반복된다
  - `chrome://webrtc-internals` inbound-rtp: `framesPerSecond` 약 30, `packetsReceived` 증가, `framesDecoded` 증가, `packetsLost` 0 근처, SRTP 복호화 실패 없음
  - 서버 로그: 파일 로드(프레임 300, fps 30), 송신 시작(SSRC, payload type)
- [ ] 9.2 두 번째 탭을 열어 연결하고 두 탭 모두 영상이 나오는지, 두 번째 탭이 처음부터 재생되는지 확인한다. 한 탭에서 연결 버튼을 다시 누르면 새 영상이 처음부터 나오는지 확인한다. 탭을 닫고 서버 로그에 송신 중지(close_notify 또는 세션 만료)가 남는지 확인한 뒤, Ctrl+C로 서버가 종료 코드 0으로 끝나는지 확인한다
