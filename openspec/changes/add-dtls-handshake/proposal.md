# Proposal

## Why

브라우저와 서버 사이의 ICE 연결은 확보했지만, 서버가 DTLS 패킷을 버리기 때문에 Chrome의 `connectionState`는 `failed`로 끝난다. WebRTC 미디어는 DTLS 핸드셰이크로 상대 인증서를 확인하고 SRTP 키를 정한 뒤에만 보낼 수 있다. 그래서 RTP 송신에 앞서 DTLS 핸드셰이크를 완료해 `connectionState`가 `connected`가 되는 경로를 먼저 만든다. DTLS 재전송에 필요한 타이머를 이번에 이벤트 루프에 넣고, 이전 change에서 타이머 때문에 미뤄 둔 ICE 세션 만료도 함께 넣는다.

## What Changes

- 이벤트 루프에 일회성 타이머(등록, 취소)를 추가한다.
- 서버를 DTLS 서버(`a=setup:passive`)로 동작시킨다.
  - nominated pair의 브라우저 주소에서 온 DTLS 패킷을 그 세션의 DTLS 처리로 넘긴다.
  - 핸드셰이크 메시지가 사라졌을 때 타이머로 재전송한다.
- 핸드셰이크에서 받은 브라우저 인증서를 offer의 `a=fingerprint`(SHA-256)와 비교한다. 다르면 핸드셰이크를 실패시킨다.
- `use_srtp` 확장으로 SRTP 프로파일(`SRTP_AES128_CM_SHA1_80`)을 협상한다. 핸드셰이크가 끝나면 SRTP 키 재료를 꺼내 세션에 보관하고, 협상 결과를 로그에 남긴다. 키 값은 로그에 남기지 않는다.
- offer 검증에 브라우저 인증서 fingerprint(`sha-256`)와 DTLS 역할(`a=setup`이 `actpass` 또는 `active`)을 추가한다. 조건을 만족하지 않으면 `400`이다.
- ICE 세션 만료를 추가한다. 생성 후 또는 마지막 유효 Binding 요청 후 30초 동안 유효한 요청이 없으면 세션과 그 DTLS 상태를 제거한다.
- 연결 확인 웹 페이지가 연결된 뒤 transport의 `dtlsState`와 `srtpCipher`를 표시한다.
- OpenSSL 요구 버전을 3.2 이상으로 올리고 `libssl`(`OpenSSL::SSL`)을 링크한다.

범위 밖(이후 마일스톤):

- SRTP 암호화, RTP 패킷화와 송신, libsrtp 도입
- nomination 변경에 따른 candidate pair 갱신. 이번에도 처음 nomination을 받은 pair만 쓴다
- HTTP 유휴 연결 타임아웃과 lingering close. 타이머를 쓰지만 이번 목표와 무관하므로 별도 change로 진행한다
- DTLS 재협상, DTLS 1.3, `SRTP_AES128_CM_SHA1_80` 외 프로파일(AEAD GCM 등)
- DataChannel(SCTP over DTLS)
- 브라우저가 DTLS 서버가 되는 경우(offer의 `a=setup:passive`)

## Capabilities

### New Capabilities

- `dtls-transport`: nominated pair 위에서 서버가 DTLS 서버로 핸드셰이크한다. 재전송, 브라우저 인증서 fingerprint 검증, SRTP 프로파일 협상과 키 재료 추출, 실패·종료 처리를 맡는다.

### Modified Capabilities

- `ice-lite`: nominated pair의 주소에서 온 DTLS 패킷을 버리지 않고 DTLS 처리로 넘긴다. 세션 만료 요구사항을 추가한다.
- `webrtc-signaling`: offer 검증에 fingerprint와 `a=setup` 조건을 추가한다. 연결 확인 페이지의 기대 결과가 `connectionState` `failed`에서 `connected`로 바뀌고, DTLS 상태 표시를 추가한다.

## Impact

- 새 코드
  - `src/crypto/dtls_context.h/.cpp`: SSL_CTX 래퍼
  - `src/webrtc/dtls_transport.h/.cpp`: 세션별 DTLS 상태
  - 대응하는 `tests/`
- 바뀌는 코드
  - `src/core/event_loop.h/.cpp`: 타이머
  - `src/crypto/certificate.h`: 키와 인증서 접근자
  - `src/webrtc/sdp.h/.cpp`: offer의 fingerprint와 setup 해석
  - `src/webrtc/ice_lite_agent.h/.cpp`: DTLS 분기, nominated pair 테이블, 세션 만료
  - `src/webrtc/signaling.cpp`, `src/apps/web_server/main.cpp`, `web/app.js`, `web/index.html`, `README.md`
- 의존성
  - OpenSSL 최소 버전이 3.0에서 3.2로 오른다. 3.2부터 들어간 datagram pair BIO를 쓰기 때문이다.
  - 링크 대상에 `OpenSSL::SSL`을 추가한다.
  - libsrtp는 아직 도입하지 않는다.
- HTTP 동작과 명령행 인자는 바뀌지 않는다.
