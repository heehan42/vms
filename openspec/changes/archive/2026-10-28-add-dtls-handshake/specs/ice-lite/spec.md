# Spec Delta

## MODIFIED Requirements

### Requirement: 잘못된 요청 무시
서버는 검증에 실패한 패킷에 응답하지 않고 원인을 로그에 남겨야 한다(SHALL). STUN 형식이 아니거나, `FINGERPRINT`가 없거나 틀리거나, `USERNAME`이 어떤 세션과도 맞지 않거나, `MESSAGE-INTEGRITY`가 없거나 틀린 요청이 이에 해당한다. 첫 바이트로 STUN이 아니라고 판정한 패킷 중 연결된 세션의 nominated pair의 주소에서 온 DTLS 패킷은 DTLS 처리로 넘긴다. 그 밖의 패킷(nominated pair가 아닌 주소의 DTLS, RTP 등)은 버리며, 브라우저가 재전송하므로 원격 주소마다 처음 한 번만 로그를 남긴다. 서버는 이런 패킷 때문에 종료되거나 다른 세션의 처리를 멈추지 않는다.

#### Scenario: 틀린 비밀번호
- **WHEN** 세션의 `USERNAME`은 맞지만 다른 비밀번호로 서명한 Binding 요청이 도착한다
- **THEN** 서버는 응답하지 않고 무결성 검증 실패를 로그에 남긴다

#### Scenario: 알 수 없는 세션
- **WHEN** 어떤 answer에도 없던 `ufrag`를 `USERNAME`에 담은 Binding 요청이 도착한다
- **THEN** 서버는 응답하지 않는다

#### Scenario: 임의 바이트
- **WHEN** 무작위 바이트로 채운 UDP 패킷이 도착한다
- **THEN** 서버는 응답하지 않고 계속 다른 요청을 처리한다

### Requirement: 연결 수립 기록
서버는 세션에서 `USE-CANDIDATE` 속성이 있는 유효한 Binding 요청을 처음 받으면 그 세션을 연결됨 상태로 바꾸고, 세션과 브라우저 주소를 로그에 남겨야 한다(SHALL). 처음 nomination을 받은 pair(브라우저 주소와 요청이 도착한 서버 주소)를 세션의 DTLS 경로로 고정하며, 이후 다른 pair에 대한 `USE-CANDIDATE`가 와도 성공 응답만 보내고 경로는 바꾸지 않는다.

#### Scenario: 브라우저의 지명
- **WHEN** 브라우저가 `USE-CANDIDATE`가 있는 Binding 요청을 보낸다
- **THEN** 서버는 성공 응답을 보내고 연결 수립 로그를 한 번 남긴다

#### Scenario: STUN이 아닌 패킷
- **WHEN** 연결된 세션의 nominated pair의 브라우저 주소에서 DTLS ClientHello가 도착한다
- **THEN** 서버는 그 패킷을 해당 세션의 DTLS 처리로 넘긴다

#### Scenario: 연결을 유지하는 브라우저
- **WHEN** 연결된 브라우저가 몇 초마다 동의 확인(consent) Binding 요청을 보낸다
- **THEN** 서버는 매 요청에 성공 응답을 보내며, 연결 수립 로그는 다시 남기지 않는다

#### Scenario: 다른 pair의 nomination 변경
- **WHEN** 연결된 세션에 처음과 다른 브라우저 주소에서 `USE-CANDIDATE`가 있는 유효한 Binding 요청이 도착한다
- **THEN** 서버는 성공 응답을 보내지만, DTLS 패킷은 계속 처음 nomination을 받은 주소에서 온 것만 받는다

## ADDED Requirements

### Requirement: 세션 만료
서버는 세션을 만든 뒤, 또는 그 세션의 마지막 유효한 Binding 요청 뒤 30초 동안 유효한 Binding 요청이 없으면 그 세션을 제거하고 로그를 남겨야 한다(SHALL). 세션을 제거하면 그 세션의 DTLS 상태와 nominated pair의 주소도 함께 사라진다. 제거된 세션의 자격 증명으로 온 요청은 알 수 없는 세션으로 처리한다. 동의 확인 요청도 유효한 요청이므로, 브라우저가 연결을 유지하는 동안에는 세션이 만료되지 않는다.

#### Scenario: 사용하지 않은 세션
- **WHEN** answer를 받은 뒤 30초 동안 아무 Binding 요청도 보내지 않는다
- **THEN** 서버는 세션을 제거하고, 이후 그 세션의 자격 증명으로 서명한 요청에 응답하지 않는다

#### Scenario: 동의 확인으로 유지되는 세션
- **WHEN** 연결된 브라우저가 30초를 넘는 동안 몇 초마다 동의 확인 요청을 보낸다
- **THEN** 세션은 유지되고 매 요청에 성공 응답을 받는다

#### Scenario: 페이지를 닫은 브라우저
- **WHEN** 연결된 브라우저 탭을 닫아 동의 확인 요청이 끊긴다
- **THEN** 마지막 요청 후 30초가 지나면 서버는 세션을 제거하고 로그를 남긴다
