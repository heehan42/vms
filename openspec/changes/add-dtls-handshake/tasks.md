# Tasks

## 1. 빌드 구성

- [ ] 1.1 루트 `CMakeLists.txt`의 OpenSSL 요구 버전을 3.2로 올리고 `server_core`에 `OpenSSL::SSL`을 `PUBLIC`으로 추가한다. `cmake -B build && cmake --build build`가 경고 없이 성공하고 기존 `ctest --test-dir build`가 모두 통과하는지 확인한다

## 2. 이벤트 루프 타이머

- [ ] 2.1 `src/core/event_loop.h/.cpp`에 design.md 결정 3의 `AddTimer`·`CancelTimer`를 구현한다. 대기 시간은 가장 이른 기한까지를 밀리초로 올림해 계산하고, fd 이벤트 묶음 뒤에 기한이 지난 타이머를 실행한다. `tests/core/event_loop_test.cpp`에 다음 시나리오를 추가해 통과하는지 확인한다: 기한 순서대로 실행, 같은 기한은 등록 순서, 취소한 타이머 미실행, 핸들러 안의 자기 취소와 새 등록(새 0초 타이머는 다음 반복에 실행), 타이머와 fd 이벤트를 함께 처리, 없는 ID 취소는 무시

## 3. DTLS 컨텍스트

- [ ] 3.1 `src/crypto/certificate.h/.cpp`에 키·인증서 원시 포인터 접근자(소유권 이전 없음)와, 임의 `X509`의 SHA-256 fingerprint 문자열을 만드는 함수를 추가한다. 기존 fingerprint 생성이 그 함수를 쓰게 바꾼다. `certificate_test`에 같은 인증서에 대해 두 경로의 결과가 같은지 확인하는 시나리오를 추가해 통과하는지 확인한다
- [ ] 3.2 `src/crypto/dtls_context.h/.cpp`에 design.md 결정 5의 SSL_CTX 설정을 구현한다: DTLS 서버 메서드, 1.2 고정, 인증서와 키, `SRTP_AES128_CM_SHA1_80`, 피어 인증서 요구와 검증 콜백 연결 지점. `tests/crypto/dtls_context_test.cpp`로 생성 성공, 키와 인증서의 일치(`SSL_CTX_check_private_key`), SRTP 프로파일 목록 설정을 확인한다

## 4. DtlsTransport

- [ ] 4.1 `tests/webrtc/dtls_test_client.h/.cpp`에 `DTLS_client_method`, 자체 서명 인증서, datagram pair BIO, `use_srtp`를 켜고 끌 수 있는 테스트 전용 DTLS 클라이언트를 만든다. `tests/webrtc/CMakeLists.txt`에 테스트 전용 정적 라이브러리로 등록하고 빌드가 성공하는지 확인한다
- [ ] 4.2 `src/webrtc/dtls_transport.h/.cpp`에 design.md 결정 6을 구현한다.
  - datagram pair BIO, MTU 1200, `HandleDatagram`·`HandleTimeout`·`NextTimeout`
  - 상태 전이(`kHandshaking`, `kConnected`, `kFailed`, `kClosed`)와 실패 원인 문구
  - 검증 콜백의 fingerprint 비교
  - SRTP 프로파일 확인과 60바이트 키 재료 분리, 소멸 시 `OPENSSL_cleanse`

  `tests/webrtc/dtls_transport_test.cpp`로 다음을 확인한다.
  - 정상 핸드셰이크: 양쪽 프로파일이 `SRTP_AES128_CM_SHA1_80`이고, 서버 키 재료가 클라이언트가 꺼낸 값과 같으며, 클라이언트가 받은 서버 인증서 fingerprint가 `Certificate`의 값과 같다
  - fingerprint 불일치 시 `kFailed`와 클라이언트 쪽 핸드셰이크 오류
  - `use_srtp` 미제안 시 `kFailed`
  - 첫 응답 묶음을 버리고 약 1초 뒤 `HandleTimeout`으로 재전송받아 완료하며, 완료 후 `NextTimeout`이 비어 있다
  - close_notify 뒤 `kClosed`이고 이후 데이터그램에 출력이 없다

## 5. SDP와 시그널링

- [ ] 5.1 `src/webrtc/sdp.h/.cpp`의 offer 해석에 design.md 결정 8의 `a=fingerprint`(sha-256, 미디어 수준 우선, 대문자 정규화)와 `a=setup`(`actpass`·`active`, 없으면 `active`) 처리를 추가하고 `SdpOffer::remote_fingerprint`를 채운다. `sdp_test`에서 해석 성공을 기대하는 직접 만든 offer에 `a=fingerprint:sha-256` 줄을 넣고, 다음 시나리오를 추가해 기존 시나리오와 함께 통과하는지 확인한다: 견본 Chrome offer의 fingerprint, 소문자 값의 정규화, fingerprint 없음, `sha-1`만 있음, 형식이 틀린 값, `a=setup:passive`의 오류
- [ ] 5.2 `IceLiteAgent::CreateSession`이 브라우저 fingerprint를 받도록 바꾸고 `src/webrtc/signaling.cpp`에서 넘긴다. `tests/integration/http_server_test.sh`에 fingerprint 줄을 지운 offer와 `a=setup:passive`로 바꾼 offer의 `400` 시나리오를 추가하고, 기존 시나리오와 함께 `ctest --test-dir build --output-on-failure`가 통과하는지 확인한다

## 6. 에이전트의 DTLS 연결과 세션 만료

- [ ] 6.1 `src/webrtc/ice_lite_agent.h/.cpp`에 design.md 결정 7을 구현한다.
  - `IceLiteAgent::Start`가 `DtlsContext`를 받는다
  - nominated pair 테이블(소켓 순번 + 브라우저 주소)과, 처음 nomination을 받을 때 `DtlsTransport` 생성
  - DTLS 분기: 출력을 nominated pair로 송신하고 재전송 타이머를 재설정하며, 상태가 바뀌면 로그를 남긴다
  - 실패·닫힌 세션과 nominated pair가 아닌 주소의 DTLS 패킷 버림
  - 기존 주석의 "지명", "쌍", "세션 표" 표현을 nomination, nominated pair, candidate pair, 세션 테이블로 정리(`ice_lite_agent.h/.cpp`, `stun_message.cpp`, `tests/webrtc/ice_lite_agent_test.cpp`)

  `ice_lite_agent_test`에 다음 시나리오를 추가하고, 기존 STUN 시나리오와 함께 통과하는지 확인한다.
  - `127.0.0.1`에서 `USE-CANDIDATE` 뒤 같은 UDP 소켓으로 테스트 클라이언트의 핸드셰이크 완료
  - 다른 포트에서 보낸 ClientHello에 응답 없음
  - 처음과 다른 주소의 nomination 변경 뒤에도 DTLS는 처음 주소에서만 받음
- [ ] 6.2 세션 만료를 구현한다.
  - `IceLiteAgentConfig::session_timeout`(기본 30초)
  - 생성·마지막 유효 요청 시각과 세션별 만료 타이머(남은 시간으로 다시 걸기)
  - 제거 순서(타이머 취소 → nominated pair 테이블 → 세션)와 제거 로그
  - 소멸자의 타이머 취소

  `ice_lite_agent_test`에 `session_timeout` 200ms로 다음 시나리오를 추가해 통과하는지 확인한다.
  - 요청 없는 세션은 만료 뒤 서명한 요청에 응답 없음
  - 100ms마다 요청을 보내면 500ms 뒤에도 응답
  - 핸드셰이크를 마친 세션이 만료되면 같은 주소의 DTLS 패킷에 응답 없음
- [ ] 6.3 `src/apps/web_server/main.cpp`에서 인증서 다음에 `DtlsContext`를 만들어 에이전트에 넘긴다. 생성 실패 시 원인 출력과 0이 아닌 종료 코드를 보장한다. 서버를 실행해 시작 로그가 이전과 같은지, Ctrl+C로 종료 코드 0으로 끝나는지 확인한다

## 7. 웹 페이지와 문서

- [ ] 7.1 `web/index.html`과 `web/app.js`에 DTLS 상태와 SRTP 암호 표시 영역을 추가한다. `connectionState`가 `connected`가 되면 `getStats()`의 `transport` 항목에서 `dtlsState`와 `srtpCipher`를 읽어 표시한다. 통합 테스트의 `GET /` 검사가 계속 통과하는지 확인한다
- [ ] 7.2 `README.md`에 다음 내용을 정리한다. 문서의 빌드 명령을 그대로 실행해 동작하는지 확인한다.
  - OpenSSL 3.2 이상 요구
  - 연결 확인 기준: 페이지의 연결 상태 `connected`, `chrome://webrtc-internals`의 `dtlsState`·`dtlsRole`·`srtpCipher`
  - 세션 만료 동작

## 8. 통합 검증

- [ ] 8.1 WSL2에서 서버를 실행하고 Windows Chrome에서 연결 버튼을 누른다. 다음을 확인한다.
  - 페이지: ICE 상태 `connected`, 연결 상태 `connected`, DTLS 상태 `connected`, SRTP 암호 `AES_CM_128_HMAC_SHA1_80`
  - `chrome://webrtc-internals` transport 통계: `dtlsState: connected`, `dtlsRole: client`, `srtpCipher: AES_CM_128_HMAC_SHA1_80`
  - 서버 로그: 프로파일 이름이 든 DTLS 연결 기록

  서버 로그에 nomination 전 DTLS 패킷을 버린 기록이 있는지와, 연결까지 걸린 시간을 확인한다. 1초 이상 늦어지면 design.md Risks의 처리 방식을 기록한다.
- [ ] 8.2 8.1 상태에서 탭을 닫고, 약 30초 뒤 서버 로그에 세션 제거 기록이 남는지 확인한다. 연결 버튼을 여러 번 눌러 새 세션마다 연결되는지 확인한다. 이어서 Ctrl+C로 서버를 끄면 종료 코드 0으로 끝나는지 확인한다
