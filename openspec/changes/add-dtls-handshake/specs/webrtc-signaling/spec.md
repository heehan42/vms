# Spec Delta

## MODIFIED Requirements

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

## REMOVED Requirements

### Requirement: 연결 확인 웹 페이지
**Reason**: DTLS 핸드셰이크를 구현해 연결 상태의 기대 결과가 `failed`에서 `connected`로 바뀌고, DTLS 상태 표시가 추가되었다. 기존 시나리오 "DTLS 미구현 상태의 연결 상태"가 더 이상 성립하지 않아 요구사항을 새 이름으로 교체한다.
**Migration**: "연결 상태 확인 웹 페이지" 요구사항이 같은 페이지 동작과 DTLS 상태 표시를 함께 정의한다.

## ADDED Requirements

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
