# Spec Delta

## MODIFIED Requirements

### Requirement: 잘못된 요청 무시
서버는 검증에 실패한 패킷에 응답하지 않고 원인을 로그에 남겨야 한다(SHALL). STUN 형식이 아니거나, `FINGERPRINT`가 없거나 틀리거나, `USERNAME`이 어떤 세션과도 맞지 않거나, `MESSAGE-INTEGRITY`가 없거나 틀린 요청이 이에 해당한다. 첫 바이트로 STUN이 아니라고 판정한 패킷 중 연결된 세션의 nominated pair의 주소에서 온 DTLS 패킷은 DTLS 처리로 넘긴다. 첫 바이트가 128~191이고 둘째 바이트가 192~223인 패킷은 RTCP로 보고, DTLS가 완료된 세션의 nominated pair의 주소에서 왔으면 그 세션의 RTCP 처리로 넘긴다. 그 밖의 패킷(nominated pair가 아닌 주소의 DTLS, RTP, DTLS가 완료되지 않은 세션이나 nominated pair가 아닌 주소의 RTCP 등)은 버리며, 브라우저가 재전송하므로 원격 주소마다 처음 한 번만 로그를 남긴다. 서버는 이런 패킷 때문에 종료되거나 다른 세션의 처리를 멈추지 않는다.

#### Scenario: 틀린 비밀번호
- **WHEN** 세션의 `USERNAME`은 맞지만 다른 비밀번호로 서명한 Binding 요청이 도착한다
- **THEN** 서버는 응답하지 않고 무결성 검증 실패를 로그에 남긴다

#### Scenario: 알 수 없는 세션
- **WHEN** 어떤 answer에도 없던 `ufrag`를 `USERNAME`에 담은 Binding 요청이 도착한다
- **THEN** 서버는 응답하지 않는다

#### Scenario: 임의 바이트
- **WHEN** 무작위 바이트로 채운 UDP 패킷이 도착한다
- **THEN** 서버는 응답하지 않고 계속 다른 요청을 처리한다

#### Scenario: 연결된 세션의 RTCP
- **WHEN** DTLS가 완료된 세션의 nominated pair의 브라우저 주소에서 둘째 바이트가 201(RR)인 패킷이 도착한다
- **THEN** 서버는 그 패킷을 해당 세션의 RTCP 처리로 넘긴다

#### Scenario: DTLS 완료 전의 RTCP
- **WHEN** nomination은 받았지만 DTLS가 완료되지 않은 세션의 브라우저 주소에서 RTCP 패킷이 도착한다
- **THEN** 서버는 그 패킷을 버리고 그 주소에 대해 처음 한 번만 로그를 남긴다

#### Scenario: RTP 패킷
- **WHEN** DTLS가 완료된 세션의 nominated pair의 브라우저 주소에서 둘째 바이트가 RTCP 범위가 아닌 RTP 패킷이 도착한다
- **THEN** 서버는 그 패킷을 버린다
