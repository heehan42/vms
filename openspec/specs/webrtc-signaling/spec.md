# webrtc-signaling Specification

## Purpose

브라우저와 서버가 WebRTC 연결을 맺을 수 있도록 브라우저의 SDP offer를 받아 서버의 연결 정보를 담은 SDP answer로 응답한다. 이후 DTLS와 미디어 송신이 이 협상 결과 위에 추가된다.

## Requirements

### Requirement: Offer 엔드포인트
서버는 `POST /offer` 요청의 본문을 SDP offer로 받아, 처리에 성공하면 SDP answer를 본문으로 `200 OK`와 `Content-Type: application/sdp`로 응답해야 한다(SHALL). 요청의 `Content-Type`이 `application/sdp`(매개변수 허용, 대소문자 무시)가 아니면 `415 Unsupported Media Type`으로 응답한다. 난수 생성 등 서버 내부 처리에 실패하면 원인을 로그에 남기고 `500 Internal Server Error`로 응답한다.

#### Scenario: Chrome offer 처리
- **WHEN** Chrome이 수신 전용 비디오 트랜시버 하나로 만든 offer를 `Content-Type: application/sdp`로 `POST /offer`에 보낸다
- **THEN** 서버는 `200 OK`, `Content-Type: application/sdp`와 함께 answer를 응답한다

#### Scenario: 잘못된 Content-Type
- **WHEN** 클라이언트가 `Content-Type: text/plain`으로 `POST /offer`를 요청한다
- **THEN** 서버는 `415 Unsupported Media Type`으로 응답한다

### Requirement: Offer 검증
서버는 offer에서 포트가 0이 아닌 첫 번째 비디오 미디어 섹션을 협상 대상으로 골라야 한다(SHALL). offer에 BUNDLE 그룹이 있으면 그 그룹에 속한 섹션 중에서 고른다. 다음 중 하나라도 해당하면 `400 Bad Request`로 응답하고 원인을 로그에 남긴다: 고를 섹션이 없음, 그 섹션에 `a=mid`가 없음, ICE 자격 증명(`a=ice-ufrag`, `a=ice-pwd`)이나 `sha-256` 해시의 `a=fingerprint`가 그 섹션에도 세션 수준에도 없음, `a=setup`이 `actpass`나 `active`가 아님, `packetization-mode=1`인 H.264 코덱이 없음, SDP 형식이 잘못됨.

#### Scenario: 비디오 섹션이 없는 offer
- **WHEN** 오디오 미디어 섹션만 있는 offer를 보낸다
- **THEN** 서버는 `400 Bad Request`로 응답한다

#### Scenario: H.264가 없는 offer
- **WHEN** 비디오 섹션에 VP8 코덱만 있는 offer를 보낸다
- **THEN** 서버는 `400 Bad Request`로 응답한다

#### Scenario: SDP가 아닌 본문
- **WHEN** `Content-Type: application/sdp`로 `hello`라는 본문을 보낸다
- **THEN** 서버는 `400 Bad Request`로 응답한다

#### Scenario: fingerprint가 없는 offer
- **WHEN** Chrome offer에서 `a=fingerprint` 줄을 지우고 보낸다
- **THEN** 서버는 `400 Bad Request`로 응답한다

#### Scenario: 브라우저가 DTLS 서버를 요구하는 offer
- **WHEN** Chrome offer의 `a=setup:actpass`를 `a=setup:passive`로 바꿔 보낸다
- **THEN** 서버는 `400 Bad Request`로 응답한다

### Requirement: Answer 내용
서버는 Chrome이 `setRemoteDescription`으로 받아들이는 answer를 만들어야 한다(SHALL). 세션 수준에는 `a=ice-lite`를 넣는다. answer의 미디어 섹션은 offer와 같은 개수와 순서를 유지한다. 협상 대상 섹션은 offer의 `mid`, 선택한 H.264 payload type, `a=sendonly`, `a=rtcp-mux`, 이 세션에 새로 만든 ICE 자격 증명, 서버 인증서의 SHA-256 fingerprint, `a=setup:passive`, ICE 주소마다의 host candidate와 `a=end-of-candidates`를 포함한다. 나머지 미디어 섹션은 포트 0으로 거절한다. offer에 BUNDLE 그룹이 있었을 때만 협상 대상 섹션으로 이루어진 BUNDLE 그룹을 넣는다.

#### Scenario: 브라우저가 answer 적용
- **WHEN** 연결 확인 페이지가 서버의 answer로 `setRemoteDescription`을 호출한다
- **THEN** 호출이 오류 없이 완료된다

#### Scenario: 세션마다 다른 자격 증명
- **WHEN** 같은 offer를 두 번 보낸다
- **THEN** 두 answer의 `a=ice-ufrag`와 `a=ice-pwd`는 서로 다르다

#### Scenario: 추가 미디어 섹션 거절
- **WHEN** 비디오 섹션 뒤에 오디오 섹션이 함께 있는 offer를 보낸다
- **THEN** answer의 오디오 섹션은 `m=audio 0`으로 거절되고 BUNDLE 그룹에서 빠진다

### Requirement: 서버 인증서
서버는 시작할 때 자체 서명 인증서와 키를 만들고, 프로세스가 끝날 때까지 모든 answer의 fingerprint에 같은 인증서를 사용해야 한다(SHALL). 인증서를 만들지 못하면 원인을 표준 오류에 출력하고 0이 아닌 종료 코드로 끝난다.

#### Scenario: 같은 fingerprint
- **WHEN** 서버 실행 중 offer를 두 번 보낸다
- **THEN** 두 answer의 `a=fingerprint:sha-256` 값이 같다

### Requirement: 연결 상태 확인 웹 페이지
웹 루트의 기본 페이지는 버튼을 누르면 수신 전용 비디오 트랜시버로 offer를 만들어 `POST /offer`로 보내고 answer를 적용해야 한다(SHALL). 페이지는 `iceConnectionState`와 `connectionState`의 현재 값과 시그널링 실패 원인을 화면에 표시한다. `connectionState`가 `connected`가 되면 선택된 transport의 `dtlsState`와 `srtpCipher`도 표시한다.

#### Scenario: Windows Chrome에서 ICE 연결
- **WHEN** WSL2에서 서버를 실행하고 Windows Chrome으로 페이지에 접속해 연결 버튼을 누른다
- **THEN** 화면의 ICE 상태가 `checking`을 거쳐 `connected`로 바뀐다

#### Scenario: DTLS 연결 완료
- **WHEN** ICE 상태가 `connected`가 된 뒤 DTLS 핸드셰이크가 끝난다
- **THEN** 화면의 연결 상태는 `connected`로 바뀌고, DTLS 상태 `connected`와 SRTP 암호 `SRTP_AES128_CM_HMAC_SHA1_80`이 표시된다

#### Scenario: 시그널링 실패 표시
- **WHEN** 서버가 offer에 `400 Bad Request`로 응답한다
- **THEN** 페이지는 응답 상태 코드를 화면에 표시한다
