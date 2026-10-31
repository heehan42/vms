# Spec Delta

## ADDED Requirements

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

## MODIFIED Requirements

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
