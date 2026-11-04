# Tasks

## 1. 송신 공통부 분리

- [x] 1.1 `src/webrtc/srtp_video_packet_writer.h/.cpp`에 `SrtpVideoPacketWriter`를 만든다(design 결정 6). SSRC, payload type, 순번, `SrtpSender`를 가지고, `WriteFrame(frame, timestamp)`가 패킷화·헤더 작성·marker·SRTP 보호를 한다. `VideoStreamSender`가 이를 쓰도록 바꾼다. 기존 `ctest -R "video_stream_sender|ice_lite_agent"`가 수정 없이 통과하는지 확인한다
- [x] 1.2 `tests/webrtc/srtp_video_packet_writer_test.cpp`에서 확인한다(`ctest -R srtp_video_packet_writer` 통과)
  - 순번이 프레임을 넘어 이어지고 65535 다음은 0
  - 넘긴 타임스탬프가 그대로 쓰임
  - 마지막 패킷에만 marker

## 2. DTLS 서버 쪽 종료

- [x] 2.1 `DtlsTransport::Close()`를 추가하고 design 결정 8대로 동작하게 한다. `tests/webrtc/dtls_transport_test.cpp`에서 확인한다(`ctest -R dtls_transport` 통과)
  - 연결된 상태에서 Close하면 테스트 클라이언트가 close_notify를 받고 상태가 `kClosed`
  - 핸드셰이크 중에 Close하면 빈 목록이고 상태가 그대로

## 3. RTSP URL과 메시지

- [x] 3.1 `src/rtsp/rtsp_url.h/.cpp`에 `ParseRtspUrl`과 IPv4 주소 해석 함수를 추가한다. `tests/rtsp/rtsp_url_test.cpp`에서 확인한다(`ctest -R rtsp_url` 통과)
  - 포트 생략 시 554, 대문자 스킴, 경로 없음
  - 오류: `@` 포함, 포트 0·65536·숫자 아님, `http://`
  - `localhost` 해석
- [x] 3.2 `src/rtsp/rtsp_message.h/.cpp`에 design 결정 4의 함수들을 추가한다. `tests/rtsp/rtsp_message_test.cpp`에서 확인한다(`ctest -R rtsp_message` 통과)
  - 요청 문자열과 `CSeq`
  - 응답 해석: 나뉘어 도착, 본문 포함, 헤더 대소문자, 크기 초과, 형식 오류
  - `Session`의 timeout 있음·없음
  - `Transport`의 UDP 유니캐스트와 TCP 거부
  - control URL 해석: `*`, 절대, 상대, base 끝 `/` 유무
- [x] 3.3 `CMakeLists.txt`에 `src/rtsp/*.cpp`, `tests/CMakeLists.txt`에 `tests/rtsp`를 추가하고 빌드되는지 확인한다

## 4. H.264 RTP 재조립

- [x] 4.1 `src/rtsp/h264_rtp_depacketizer.h/.cpp`에 design 결정 5의 재조립기를 추가한다
- [x] 4.2 `tests/rtsp/h264_rtp_depacketizer_test.cpp`에서 확인한다(`ctest -R h264_rtp_depacketizer` 통과)
  - 단일 NAL, STAP-A(SPS·PPS)+IDR의 키프레임 판정
  - FU-A 3조각 재조립
  - 가운데 조각 손실 시 프레임 버림과 손실 이벤트
  - marker 없이 타임스탬프가 바뀔 때 앞 프레임 완료
  - 순번 65535→0 연속
  - 다른 payload type은 무시
  - SSRC 변경 시 손실 이벤트와 초기화
  - 4MiB 초과 프레임 버림
  - `webrtc::PacketizeH264Frame`으로 만든 패킷을 다시 재조립하면 원래 NAL 목록과 같음

## 5. 실시간 소스와 카메라 목록

- [x] 5.1 `src/media/live_video_source.h/.cpp`에 `LiveVideoSource`(design 결정 2)를, `src/media/camera_list.h/.cpp`에 이름·URL·소스를 묶은 `CameraList`를 추가한다
- [x] 5.2 `tests/media/live_video_source_test.cpp`에서 확인한다(`ctest -R live_video_source` 통과)
  - 구독자 둘이 같은 프레임을 받음
  - 해지한 구독자는 받지 않음
  - 콜백 안에서 자기 자신과 다른 구독자를 해지해도 안전
  - `IsConnected()`와 연결 이벤트 전달

## 6. 카메라 송신자

- [x] 6.1 `src/webrtc/live_video_stream_sender.h/.cpp`에 design 결정 6의 상태 기계와 타임스탬프 변환을 구현한다. 시각은 인자로 받는다
- [x] 6.2 `tests/webrtc/live_video_stream_sender_test.cpp`에서 확인한다(`ctest -R live_video_stream_sender` 통과)
  - 키프레임 전 프레임은 패킷 없음
  - 첫 키프레임의 타임스탬프가 초기값
  - 같은 연결 안의 간격 유지(32비트 순환 포함)
  - 손실 뒤 다음 키프레임까지 보내지 않고 순번은 이어짐
  - 재연결 뒤 타임스탬프가 경과 시간만큼 건너뜀
  - 끊긴 상태로 생성되면 연결 이벤트 전까지 프레임을 무시

## 7. RTSP 카메라 클라이언트

- [x] 7.1 `src/rtsp/rtsp_camera_client.h/.cpp`에 design 결정 3의 상태 기계를 구현한다. 비차단 TCP 연결, 요청·응답, UDP 포트 쌍, 출발 주소 검사, RTCP 버림, 유지 `OPTIONS`, 3초 RTP 감시, 5초 응답 제한, 재연결 간격, `TEARDOWN`, 연결·끊김·원인 로그를 포함한다. 시간 값은 설정 구조체로 받아 테스트에서 줄일 수 있게 한다
- [x] 7.2 `tests/rtsp/rtsp_camera_client_test.cpp`에서 같은 루프에 띄운 가짜 RTSP 서버(TCP 응답 + UDP로 RTP 송신)로 확인한다(`ctest -R rtsp_camera_client` 통과)
  - 정상 절차와 요청 순서, `CSeq`
  - `client_port`가 짝수·홀수 쌍
  - 프레임이 `LiveVideoSource`로 전달됨
  - DESCRIBE 404 뒤 재시도
  - 응답 없음 → 제한 시간 뒤 끊김
  - RTP 중단 → 끊김 이벤트
  - 서버가 연결을 닫으면 끊김 이벤트와 재연결 간격 증가
  - 재연결 성공 시 간격 초기화
  - 유지 `OPTIONS`에 `Session` 포함
  - `Stop()` 시 `TEARDOWN`

## 8. 에이전트의 카메라 세션

- [x] 8.1 `IceSessionRequest`와 세션에 카메라 지정, 구독, `LiveVideoStreamSender`, 대기 타이머를 추가한다(design 결정 7). `IceLiteAgentConfig::camera_wait_timeout`을 추가한다. 송신 시작 로그를 소스 종류와 이름으로 바꾼다
- [x] 8.2 `tests/webrtc/ice_lite_agent_test.cpp`에 카메라 세션 테스트를 추가한다. 테스트 코드가 `LiveVideoSource`에 직접 프레임과 이벤트를 넣는다(`ctest -R ice_lite_agent` 통과)
  - DTLS 완료 뒤 키프레임부터 SRTP 패킷 수신
  - 두 세션이 같은 소스 공유
  - 끊김 → 짧은 대기 시간 안에 연결 → 이어서 수신
  - 끊김 → 대기 시간 초과 → 테스트 클라이언트가 close_notify를 받고 세션이 사라짐
  - 끊긴 카메라로 DTLS가 완료되어도 대기 타이머가 걸림
  - 세션 제거 뒤 구독 해지

## 9. 시그널링, 명령행, 통합 테스트

- [x] 9.1 `Signaling`이 `CameraList`를 참조하게 하고, `type=camera` 찾기와 목록의 카메라 원소를 추가한다. `tests/webrtc/signaling_test.cpp`에서 확인한다(`ctest -R signaling` 통과)
  - 카메라 이름 offer는 200
  - 파일 이름을 `type=camera`로 보내면 404
  - 파일과 같은 이름의 카메라를 종류로 구분
  - 목록 순서와 카메라 원소에 `frames` 없음
- [x] 9.2 `main.cpp`에 `--camera`를 추가하고, 카메라 목록·클라이언트 생성, 시작과 종료 시 `Stop()`, 시작 로그를 구현한다. 사용법 문구를 고친다
- [x] 9.3 `tests/integration/http_server_test.sh`에서 확인한다(`ctest -R integration` 통과)
  - 시작 실패: `=` 없음, 빈 이름, 이름 중복, `@` 포함 URL, 해석할 수 없는 호스트
  - 꺼진 카메라로도 시작하고 목록에 카메라 원소가 있음
  - `POST /offer?type=camera&name=<이름>`는 200, 없는 카메라는 404

## 10. 문서와 실제 카메라 검증

- [x] 10.1 README에 `--camera` 인자, vms-cam(`--test-source`)과 함께 실행하는 명령, 카메라 대기·재연결 동작을 적는다. 적은 명령을 그대로 실행해 서버 로그에 카메라 연결 성공이 나오는지 확인한다
- [x] 10.2 vms-cam과 Windows Chrome으로 확인한다
  - "카메라 · cam"을 골라 1초 안팎에 영상이 표시됨
  - 두 탭이 같은 카메라를 볼 때 vms-cam 로그의 클라이언트가 하나임
  - vms-cam을 끄고 5초 뒤 켜면 같은 탭에서 영상이 이어짐
  - 끄고 켜지 않으면 약 30초 뒤 연결 상태가 `connected`가 아니게 됨
  - 3분 이상 연결이 유지됨
  - `chrome://webrtc-internals`의 `framesPerSecond`가 vms-cam의 `--fps`와 비슷함
- [x] 10.3 실제 USB 카메라(vms-cam 기본 설정)로 10.2의 첫 항목을 다시 확인한다

## Workflow follow-up

- 검토 후 change를 아카이브하고, 아카이브 이동과 명세 반영 내용을 커밋하도록 제안한다.
