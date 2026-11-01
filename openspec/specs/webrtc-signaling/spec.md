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

### Requirement: 서버 인증서
서버는 시작할 때 자체 서명 인증서와 키를 만들고, 프로세스가 끝날 때까지 모든 answer의 fingerprint에 같은 인증서를 사용해야 한다(SHALL). 인증서를 만들지 못하면 원인을 표준 오류에 출력하고 0이 아닌 종료 코드로 끝난다.

#### Scenario: 같은 fingerprint
- **WHEN** 서버 실행 중 offer를 두 번 보낸다
- **THEN** 두 answer의 `a=fingerprint:sha-256` 값이 같다

### Requirement: 연결 상태 확인 웹 페이지
웹 루트의 기본 페이지는 버튼을 누르면 수신 전용 비디오 트랜시버로 offer를 만들어 `POST /offer`로 보내고 answer를 적용해야 한다(SHALL). 페이지는 처음 열릴 때 `GET /api/videos`로 동영상 목록을 받아 동영상마다 ID를 값과 표시 글자로 하는 `option` 요소를 만들어 `select` 요소에 목록 순서대로 넣고, 첫 동영상을 선택해 둔다. 연결할 때는 선택한 ID를 `encodeURIComponent`로 인코딩해 `video` 쿼리 매개변수로 붙인다. 목록이 비었거나 목록을 받지 못하면 `select` 요소에 그 사실을 표시하고 `video` 없이 연결한다. 페이지는 `iceConnectionState`와 `connectionState`의 현재 값과 시그널링 실패 원인을 화면에 표시한다. `connectionState`가 `connected`가 되면 선택된 transport의 `dtlsState`와 `srtpCipher`도 표시한다. 페이지는 `<video>` 요소를 두고, 받은 비디오 트랙을 자동 재생(음소거)으로 표시한다. 연결 버튼을 다시 누르면 이전 연결의 영상을 지우고 새 연결의 영상을 표시한다.

#### Scenario: Windows Chrome에서 ICE 연결
- **WHEN** WSL2에서 서버를 실행하고 Windows Chrome으로 페이지에 접속해 연결 버튼을 누른다
- **THEN** 화면의 ICE 상태가 `checking`을 거쳐 `connected`로 바뀐다

#### Scenario: DTLS 연결 완료
- **WHEN** ICE 상태가 `connected`가 된 뒤 DTLS 핸드셰이크가 끝난다
- **THEN** 화면의 연결 상태는 `connected`로 바뀌고, DTLS 상태 `connected`와 SRTP 암호 `SRTP_AES128_CM_HMAC_SHA1_80`이 표시된다

#### Scenario: 시그널링 실패 표시
- **WHEN** 서버가 offer에 `400 Bad Request`로 응답한다
- **THEN** 페이지는 응답 상태 코드를 화면에 표시한다

#### Scenario: 목록 표시
- **WHEN** 동영상 두 개가 있는 디렉터리를 지정해 실행한 서버의 페이지를 연다
- **THEN** `select` 요소에 두 ID의 `option` 요소가 목록 순서대로 나타나고 첫 번째가 선택되어 있다

#### Scenario: 영상 표시
- **WHEN** `select` 요소에서 동영상을 고르고 연결 버튼을 누른다
- **THEN** 연결 상태가 `connected`가 된 뒤 `<video>` 요소에 고른 동영상이 재생된다

#### Scenario: 다시 연결
- **WHEN** 영상이 재생되는 중에 `select` 요소에서 다른 동영상을 고르고 연결 버튼을 누른다
- **THEN** 이전 연결은 닫히고, 새로 고른 동영상이 처음부터 재생된다

#### Scenario: 동영상 없는 서버
- **WHEN** `--video-dir` 없이 실행한 서버의 페이지를 열고 연결 버튼을 누른다
- **THEN** `select` 요소에 동영상이 없다는 문구가 표시되고, 연결 상태는 `connected`가 되며 영상은 표시되지 않는다

### Requirement: Offer의 동영상 선택
서버는 `POST /offer` 요청 대상의 쿼리 매개변수 `video`로 그 세션에 보낼 동영상을 정해야 한다(SHALL). 쿼리는 `&`로 나눈 `이름=값` 쌍이며, 값의 퍼센트 인코딩(`%XX`)과 `+`(공백)를 해석한다. 값이 동영상 목록의 ID와 바이트 단위로 같으면 그 동영상을 세션에 지정한다. `video`가 없으면 동영상 없이 세션을 만든다. 다른 이름의 매개변수는 무시한다. 이 검사는 `Content-Type` 검사 뒤, SDP 검증 전에 한다.

#### Scenario: 목록에 있는 ID
- **WHEN** 목록에 `a.h264`가 있는 서버에 `POST /offer?video=a.h264`로 Chrome offer를 보낸다
- **THEN** 서버는 `200 OK`와 answer로 응답하고, DTLS가 완료되면 그 세션에 `a.h264`를 보낸다

#### Scenario: 인코딩된 한글 ID
- **WHEN** 목록에 `시험.h264`가 있고 페이지가 `encodeURIComponent`로 인코딩한 값을 `video`로 보낸다
- **THEN** 서버는 그 세션에 `시험.h264`를 지정한다

#### Scenario: video 없음
- **WHEN** 쿼리 없이 `POST /offer`로 Chrome offer를 보낸다
- **THEN** 서버는 `200 OK`와 answer로 응답하고, 연결 상태가 `connected`가 되어도 RTP 패킷을 보내지 않는다

### Requirement: 동영상 선택 오류
서버는 `video` 매개변수를 해석할 수 없거나 목록에서 찾을 수 없으면 세션을 만들지 않고 오류로 응답하며 원인을 로그에 남겨야 한다(SHALL). 퍼센트 인코딩이 잘못되었거나(`%` 뒤에 16진수 두 자리가 없음) `video`가 두 번 이상 오면 `400 Bad Request`, 값이 목록의 어떤 ID와도 같지 않으면 `404 Not Found`로 응답한다.

#### Scenario: 목록에 없는 ID
- **WHEN** `POST /offer?video=missing.h264`로 Chrome offer를 보낸다
- **THEN** 서버는 `404 Not Found`로 응답하고 세션을 만들지 않는다

#### Scenario: 경로처럼 보이는 ID
- **WHEN** `POST /offer?video=..%2Fsecret.h264`로 Chrome offer를 보낸다
- **THEN** 서버는 `404 Not Found`로 응답하고, 디렉터리 밖의 파일을 읽지 않는다

#### Scenario: 잘못된 퍼센트 인코딩
- **WHEN** `POST /offer?video=%zz`로 Chrome offer를 보낸다
- **THEN** 서버는 `400 Bad Request`로 응답한다

#### Scenario: 중복된 video
- **WHEN** `POST /offer?video=a.h264&video=b.h264`로 Chrome offer를 보낸다
- **THEN** 서버는 `400 Bad Request`로 응답한다
