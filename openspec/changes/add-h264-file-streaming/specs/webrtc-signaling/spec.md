# Spec Delta

## MODIFIED Requirements

### Requirement: Answer 내용
서버는 Chrome이 `setRemoteDescription`으로 받아들이는 answer를 만들어야 한다(SHALL). 세션 수준에는 `a=ice-lite`를 넣는다. answer의 미디어 섹션은 offer와 같은 개수와 순서를 유지한다. 협상 대상 섹션은 offer의 `mid`, 선택한 H.264 payload type, `a=sendonly`, `a=rtcp-mux`, 이 세션에 새로 만든 ICE 자격 증명, 서버 인증서의 SHA-256 fingerprint, `a=setup:passive`, ICE 주소마다의 host candidate와 `a=end-of-candidates`를 포함한다. 또한 이 세션의 송신 SSRC를 알리는 `a=ssrc:<SSRC> cname:<CNAME>`과 스트림·트랙 ID를 알리는 `a=msid`를 포함한다. SSRC는 세션마다 무작위로 정한 0이 아닌 32비트 값이다. 나머지 미디어 섹션은 포트 0으로 거절한다. offer에 BUNDLE 그룹이 있었을 때만 협상 대상 섹션으로 이루어진 BUNDLE 그룹을 넣는다.

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

### Requirement: 연결 상태 확인 웹 페이지
웹 루트의 기본 페이지는 버튼을 누르면 수신 전용 비디오 트랜시버로 offer를 만들어 `POST /offer`로 보내고 answer를 적용해야 한다(SHALL). 페이지는 `iceConnectionState`와 `connectionState`의 현재 값과 시그널링 실패 원인을 화면에 표시한다. `connectionState`가 `connected`가 되면 선택된 transport의 `dtlsState`와 `srtpCipher`도 표시한다. 페이지는 `<video>` 요소를 두고, 받은 비디오 트랙을 자동 재생(음소거)으로 표시한다. 연결 버튼을 다시 누르면 이전 연결의 영상을 지우고 새 연결의 영상을 표시한다.

#### Scenario: Windows Chrome에서 ICE 연결
- **WHEN** WSL2에서 서버를 실행하고 Windows Chrome으로 페이지에 접속해 연결 버튼을 누른다
- **THEN** 화면의 ICE 상태가 `checking`을 거쳐 `connected`로 바뀐다

#### Scenario: DTLS 연결 완료
- **WHEN** ICE 상태가 `connected`가 된 뒤 DTLS 핸드셰이크가 끝난다
- **THEN** 화면의 연결 상태는 `connected`로 바뀌고, DTLS 상태 `connected`와 SRTP 암호 `SRTP_AES128_CM_HMAC_SHA1_80`이 표시된다

#### Scenario: 시그널링 실패 표시
- **WHEN** 서버가 offer에 `400 Bad Request`로 응답한다
- **THEN** 페이지는 응답 상태 코드를 화면에 표시한다

#### Scenario: 영상 표시
- **WHEN** 동영상 파일을 지정해 실행한 서버에 연결한다
- **THEN** 연결 상태가 `connected`가 된 뒤 `<video>` 요소에 영상이 재생된다

#### Scenario: 다시 연결
- **WHEN** 영상이 재생되는 중에 연결 버튼을 다시 누른다
- **THEN** 이전 연결은 닫히고, 새 연결의 영상이 처음부터 재생된다
