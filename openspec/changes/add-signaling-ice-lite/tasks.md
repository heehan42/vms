# Tasks

## 1. 사전 확인과 빌드 구성

- [x] 1.1 WSL에서 `nc -u -l 50000`으로 기다리고, Windows PowerShell의 `System.Net.Sockets.UdpClient`로 WSL `eth0` 주소(NAT 모드)에 UDP 패킷을 보내 도착하는지 확인한다. 닿지 않으면 mirrored 모드와 방화벽 설정을 시도해, 성공한 네트워크 모드와 절차를 이 change의 design.md Risks에 기록한다
- [x] 1.2 루트 `CMakeLists.txt`에 `find_package(OpenSSL 3.0 REQUIRED)`와 `server_core`의 `OpenSSL::Crypto` 링크를 추가하고, `tests/CMakeLists.txt`에 `crypto`, `webrtc` 하위 디렉터리를 추가한다. `cmake -B build && cmake --build build`가 경고 없이 성공하고 기존 `ctest`가 모두 통과하는지 확인한다

## 2. 인터페이스 주소

- [x] 2.1 `src/core/network_interfaces.h/.cpp`에 `getifaddrs`로 동작 중이고 루프백이 아닌 IPv4 주소를 조회하는 함수를 구현한다. `tests/core/network_interfaces_test.cpp`로 결과에 `127.0.0.1`이 없고 오류 없이 반환되는지 확인한다

## 3. HTTP 본문 수신과 라우팅

- [x] 3.1 `src/http/http_request.h/.cpp`가 `Content-Length`, `Content-Type`, `Transfer-Encoding` 유무를 대소문자 구분 없이 보관하게 한다. `http_request_test`에 정상 값, 숫자가 아닌 `Content-Length`, 다른 값으로 중복된 `Content-Length`, 헤더 이름 대소문자 시나리오를 추가해 통과하는지 확인한다
- [x] 3.2 `src/http/http_response.h/.cpp`에 411, 413, 415, 501 상태 코드와 사유 문구를 추가하고, `http_response_test`로 상태 줄을 확인한다
- [x] 3.3 `HttpServer`에 POST 핸들러 표를 받는 `Start` 인자와 헤더 시점의 라우팅(경로별 405 `Allow`, 411, 413, 501)을 구현하고, 상태를 `kReceivingHeader`와 `kReceivingBody`로 나눈다. 기존 통합 테스트가 통과하는지 확인한다
- [x] 3.4 `http_server_test.sh`에 본문 오류 시나리오를 추가해 통과하는지 확인한다: `Content-Length: abc`의 `400`, `Transfer-Encoding: chunked`의 `501`, 헤더만 보낸 `Content-Length: 100000` 요청의 `413`. 413은 lingering close가 없으므로 본문을 보내지 않고 검사한다

## 4. OpenSSL 래퍼

- [x] 4.1 `src/crypto/openssl_error.h/.cpp`와 `src/crypto/random.h/.cpp`에 오류 큐 메시지 변환, `RAND_bytes` 기반 난수 바이트와 `ice-char` 문자열 생성을 구현한다. 단위 테스트로 생성 문자열의 길이와 문자 집합, 두 번 생성한 값이 서로 다른지 확인한다
- [x] 4.2 `src/crypto/hmac_sha1.h/.cpp`에 일회성 `HMAC(EVP_sha1(), ...)` 래퍼를 구현하고, `tests/crypto/hmac_sha1_test.cpp`로 RFC 2202 테스트 벡터를 통과하는지 확인한다
- [x] 4.3 `src/crypto/certificate.h/.cpp`에 ECDSA P-256 자체 서명 인증서와 키 생성, SHA-256 fingerprint 문자열 생성을 구현한다. `tests/crypto/certificate_test.cpp`로 생성 성공, fingerprint 형식(대문자 16진수 32쌍, `:` 구분, 95자), 같은 인증서의 fingerprint가 같은지 확인한다

## 5. STUN 메시지

- [x] 5.1 `src/webrtc/stun_message.h/.cpp`에 CRC32, STUN 헤더와 속성 해석(`USERNAME`, `USE-CANDIDATE`, `MESSAGE-INTEGRITY`, `FINGERPRINT`, `XOR-MAPPED-ADDRESS`), 원인별 `StunError` 반환을 구현한다. `tests/webrtc/stun_message_test.cpp`로 다음을 확인한다: CRC32 `"123456789"` → `0xCBF43926`, RFC 5769 2.1 견본 요청의 해석과 FINGERPRINT 검증, 2.2 견본 응답의 `XOR-MAPPED-ADDRESS`(`192.0.2.1:32853`) 해독, 잘린 패킷과 잘못된 magic cookie 거부
- [x] 5.2 `MESSAGE-INTEGRITY` 검증(`CRYPTO_memcmp`)과 Binding 성공 응답 생성(트랜잭션 ID, `XOR-MAPPED-ADDRESS`, `MESSAGE-INTEGRITY`, `FINGERPRINT`)을 구현한다. `stun_message_test`로 RFC 5769 2.1 견본의 비밀번호 검증 성공과 틀린 비밀번호 거부, 직접 만든 응답이 해석과 검증을 통과하는지 확인한다

## 6. SDP

- [x] 6.1 Windows Chrome의 개발자 도구 콘솔에서 `RTCPeerConnection`에 `addTransceiver('video', {direction: 'recvonly'})`로 만든 offer를 출력해 `tests/webrtc/data/chrome_offer.sdp`로 저장한다. 파일에 `m=video`, `a=ice-ufrag`, `packetization-mode=1`인 H.264가 있는지 확인한다
- [x] 6.2 `src/webrtc/sdp.h/.cpp`에 SDP 문법 층(줄, 세션 영역, 미디어 섹션, 속성)과 offer 해석(BUNDLE, 첫 비디오 섹션의 `mid`, ICE 자격 증명, H.264 선택, 나머지 섹션)을 구현한다. `tests/webrtc/sdp_test.cpp`로 견본 offer의 해석 결과와 비디오 없음, H.264 없음, 형식 오류의 실패 원인을 확인한다
- [x] 6.3 design.md 6장의 템플릿대로 answer 생성을 구현한다. `sdp_test`로 다음을 확인한다: 필수 줄(`a=ice-lite`, `a=setup:passive`, `a=fingerprint:sha-256`, `a=sendonly`, `a=rtcp-mux`, candidate, `a=end-of-candidates`) 포함, 오디오 섹션의 `m=audio 0` 거절과 BUNDLE 제외, 줄 끝 `\r\n`

## 7. ICE-lite 에이전트와 시그널링

- [x] 7.1 `src/webrtc/ice_lite_agent.h/.cpp`에 주소별 UDP 소켓, 첫 바이트 분류, 세션 표(생성, 서버 ufrag 중복 방지), Binding 검증과 응답, `USE-CANDIDATE` 연결 기록을 구현한다. `tests/webrtc/ice_lite_agent_test.cpp`로 `127.0.0.1`과 포트 0에서 다음을 확인한다: 서명한 요청에 대한 검증 가능한 응답과 `XOR-MAPPED-ADDRESS`, 틀린 비밀번호와 모르는 ufrag와 임의 바이트에 응답 없음, `USE-CANDIDATE` 뒤 연결됨 상태와 이어지는 요청에 대한 응답
- [x] 7.2 `src/webrtc/signaling.h/.cpp`에 `POST /offer` 핸들러(`Content-Type` 검사 415, offer 해석 실패 400, 세션 생성, answer 200, 난수와 OpenSSL 실패 500)를 구현하고, 각 분기의 로그 문구를 정한다. 빌드가 성공하는지 확인한다
- [x] 7.3 `src/apps/web_server/main.cpp`에 `--ice-port`(기본 50000)와 반복 가능한 `--ice-address` 해석, 주소 자동 조회, 인증서·에이전트·시그널링 생성과 `/offer` 등록, 사용 주소 로그를 구현한다. 잘못된 주소, 사용 중인 UDP 포트, 인증서 생성 실패 경로에서 오류 메시지와 0이 아닌 종료 코드가 나오는지 수동으로 확인한다
- [x] 7.4 `http_server_test.sh`가 `--ice-address 127.0.0.1 --ice-port ${ICE_TEST_PORT:-18090}`로 서버를 띄우게 하고, 시그널링 시나리오를 추가해 `ctest --test-dir build --output-on-failure`가 모두 통과하는지 확인한다. 시나리오는 다음과 같다: 견본 offer의 `200`·`application/sdp`·`a=ice-lite`·`127.0.0.1` candidate, 두 answer의 ufrag가 다르고 fingerprint가 같음, `415`, SDP가 아닌 본문의 `400`, `GET /offer`의 `405 Allow: POST`, `Content-Length` 없는 `POST /offer`의 `411`

## 8. 웹 페이지와 문서

- [x] 8.1 `web/index.html`과 `web/app.js`에 연결 버튼, 수신 전용 비디오 트랜시버로 offer 생성과 `POST /offer`, answer 적용, `iceConnectionState`·`connectionState`·시그널링 실패 상태 코드 표시를 추가한다. 기존 `app.js` 실행 문구는 유지한다. 통합 테스트의 `GET /` 검사가 계속 통과하는지 확인한다
- [x] 8.2 `README.md`에 OpenSSL 3.x(`libssl-dev`) 요구, `--ice-port`·`--ice-address` 사용법, 1.1에서 확인한 WSL2 네트워크 설정 절차, 연결 확인 방법(페이지 상태 표시와 `chrome://webrtc-internals`), `ICE_TEST_PORT` 환경 변수를 정리한다. 문서의 명령을 그대로 실행해 동작하는지 확인한다

## 9. 통합 검증

- [x] 9.1 WSL2에서 서버를 실행하고 Windows Chrome에서 페이지의 연결 버튼을 누른다. ICE 상태가 `checking`을 거쳐 `connected`가 되는지, 서버 로그에 연결 수립 기록이 남는지, `chrome://webrtc-internals`의 candidate pair가 `succeeded`인지 확인한다. 이후 연결 상태가 `failed`로 바뀌고, 서버 로그에 DTLS 패킷을 버린 기록이 남는지도 확인한다
- [x] 9.2 9.1 상태에서 Ctrl+C로 서버를 끄면 UDP 소켓을 포함해 정상 종료(종료 코드 0)되는지 확인한다
