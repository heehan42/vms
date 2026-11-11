# Spec Delta

## MODIFIED Requirements

### Requirement: Answer 내용
서버는 Chrome이 `setRemoteDescription`으로 받아들이는 answer를 만들어야 한다(SHALL). 세션 수준에는 `a=ice-lite`를 넣는다. answer의 미디어 섹션은 offer와 같은 개수와 순서를 유지한다. 협상 대상 섹션은 offer의 `mid`, 선택한 H.264 payload type, `a=sendonly`, `a=rtcp-mux`, 이 세션에 새로 만든 ICE 자격 증명, 서버 인증서의 SHA-256 fingerprint, `a=setup:passive`, announced address마다의 host candidate와 `a=end-of-candidates`를 포함한다. host candidate의 포트는 그 announced address가 속한 소켓의 포트이고, priority는 명령행에 쓴 announced address 순서대로 높으며, foundation은 candidate마다 다르다. 또한 이 세션의 송신 SSRC를 알리는 `a=ssrc:<SSRC> cname:<CNAME>`과 스트림·트랙 이름을 알리는 `a=msid`를 포함한다. SSRC는 세션마다 무작위로 정한 0이 아닌 32비트 값이다. 나머지 미디어 섹션은 포트 0으로 거절한다. offer에 BUNDLE 그룹이 있었을 때만 협상 대상 섹션으로 이루어진 BUNDLE 그룹을 넣는다.

#### Scenario: 브라우저가 answer 적용
- **WHEN** 연결 확인 페이지가 서버의 answer로 `setRemoteDescription`을 호출한다
- **THEN** 호출이 오류 없이 완료된다

#### Scenario: 세션마다 다른 자격 증명
- **WHEN** 같은 offer를 두 번 보낸다
- **THEN** 두 answer의 `a=ice-ufrag`와 `a=ice-pwd`는 서로 다르다

#### Scenario: 추가 미디어 섹션 거절
- **WHEN** 비디오 섹션 뒤에 오디오 섹션이 함께 있는 offer를 보낸다
- **THEN** answer의 오디오 섹션은 `m=audio 0`으로 거절되고 BUNDLE 그룹에서 빠진다

#### Scenario: 송신 SSRC 알림
- **WHEN** Chrome offer를 보내고 answer를 받는다
- **THEN** 협상 대상 섹션에 `a=ssrc` 줄과 `a=msid` 줄이 하나씩 있고, 같은 offer를 다시 보내 받은 answer의 SSRC와는 서로 다르다

#### Scenario: 브라우저의 트랙 연결
- **WHEN** 연결 확인 페이지가 answer를 적용한다
- **THEN** `track` 이벤트로 받은 비디오 트랙이 `a=msid`의 스트림에 속한다

#### Scenario: 소켓 하나의 여러 announced address
- **WHEN** `--ice-address 127.0.0.1=127.0.0.1,203.0.113.5 --ice-port 50100`으로 실행한 서버에 offer를 보낸다
- **THEN** answer에는 `127.0.0.1 50100`과 `203.0.113.5 50100`의 host candidate가 이 순서로 들어가고, 앞의 candidate의 priority가 더 높으며, 두 candidate의 foundation은 서로 다르다

#### Scenario: 여러 ICE 주소에 걸친 순서
- **WHEN** `--ice-address 192.168.0.10=192.168.0.10,203.0.113.5 --ice-address 127.0.0.1`로 실행한 서버에 offer를 보낸다
- **THEN** answer의 host candidate는 `192.168.0.10`, `203.0.113.5`, `127.0.0.1` 순서이고 priority도 이 순서대로 높다
