# Proposal

## Why

동영상을 WebRTC로 보내려면 먼저 브라우저와 서버가 SDP를 교환하고, UDP 위에서 ICE 연결 확인을 통과해야 한다. 이 경로에는 WSL2에서 Windows 브라우저가 서버의 UDP 포트에 닿는지라는 가장 큰 불확실성이 있으므로, 먼저 Windows Chrome에서 `iceConnectionState`가 `connected`가 되는 최소 경로를 만들어 이후 DTLS·SRTP·RTP를 올릴 기반을 검증한다.

## What Changes

- 브라우저가 SDP offer를 `POST /offer`로 보내면 서버가 SDP answer로 응답하는 시그널링 엔드포인트를 추가한다.
- 서버를 ICE-lite 에이전트로 동작시킨다. UDP 소켓에서 STUN Binding 요청을 검증(USERNAME, MESSAGE-INTEGRITY, FINGERPRINT)하고 응답하며, `USE-CANDIDATE`를 받으면 연결이 수립된 것으로 기록한다.
- 시작할 때 자체 서명 인증서를 만들어 answer의 `a=fingerprint`에 넣는다. Chrome은 fingerprint가 없는 answer를 거부하기 때문이며, DTLS 핸드셰이크 자체는 이후 마일스톤에서 구현한다.
- HTTP 서버가 `Content-Length` 기반 요청 본문을 받고 `POST` 엔드포인트로 라우팅하도록 확장한다. 본문 관련 오류(411, 413, 415, 501) 응답을 추가한다.
- 서버 실행 인자에 ICE용 UDP 포트(`--ice-port`)와, answer의 host candidate로 브라우저에 알려 줄 서버 주소(`--ice-address`)를 추가한다. 브라우저는 이 주소로 STUN 요청을 보낸다. 주소를 지정하지 않으면 서버의 네트워크 인터페이스 주소 중 루프백을 제외한 IPv4 주소를 모두 사용한다.
- OpenSSL(C API)을 첫 외부 의존성으로 도입한다. HMAC-SHA1, 난수, 인증서 생성에 사용한다.
- 웹 페이지에 연결 시작 버튼과 ICE·연결 상태 표시를 추가한다.

범위 밖(이후 마일스톤):

- DTLS 핸드셰이크, SRTP, RTP 송신, 동영상 파일 읽기
- trickle ICE, full ICE(서버 측 연결 확인 송신), TURN, IPv6, ICE-TCP
- NAT 밖 브라우저의 접속(공인 주소를 candidate로 알리기, server-reflexive candidate 수집). 확장 방향은 design.md "향후 확장 지점"에 적는다
- 동영상 목록 API, 재협상(renegotiation), 세션 명시적 종료 API
- 타이머와 이를 쓰는 기능(ICE 세션 만료, HTTP 유휴 연결 타임아웃, lingering close). 이후 마일스톤에서 DTLS 재전송처럼 타이머가 꼭 필요한 기능을 구현할 때 함께 추가하며, 그때까지의 위험은 design.md에 적는다

## Capabilities

### New Capabilities

- `webrtc-signaling`: 브라우저의 SDP offer를 받아 해석하고, 서버의 ICE 자격 증명·인증서 fingerprint·candidate를 담은 SDP answer를 돌려주는 시그널링 동작과 이를 확인하는 웹 페이지.
- `ice-lite`: UDP 포트에서 STUN Binding 요청을 검증·응답하고 ICE 세션의 연결 수립을 기록하는 ICE-lite 에이전트 동작과 그 실행 설정.

### Modified Capabilities

- `http-server`: 요청 본문 수신과 `POST` 라우팅이 추가되어 요청 오류 응답(411, 413, 415, 501, 경로별 405 `Allow`)이 바뀐다.

## Impact

- 새 코드: `src/core/`(인터페이스 주소 조회), `src/crypto/`(OpenSSL 래퍼), `src/webrtc/`(SDP, STUN, ICE-lite 에이전트, 시그널링), 대응하는 `tests/`
- 수정 코드: `src/http/`(본문 수신, 라우팅), `src/apps/web_server/main.cpp`(인자, 조립), `web/`(연결 확인 페이지), `CMakeLists.txt`, `README.md`
- 의존성: OpenSSL 3.x. 시스템에 설치된 패키지(`libssl-dev`)를 CMake로 찾아 `OpenSSL::Crypto`를 링크하며, 저장소에 소스를 포함하지 않는다
- 실행 환경: WSL2 네트워크 모드(NAT 또는 mirrored)에 따라 Windows 브라우저가 닿는 주소가 달라지므로, `--ice-address`와 README 안내가 필요하다
