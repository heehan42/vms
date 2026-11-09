# Spec Delta

## MODIFIED Requirements

### Requirement: 연결 상태 확인 웹 페이지
사용자가 연결 버튼을 누르면, 웹 루트의 기본 페이지는 offer를 `POST /offer`로 보내고 answer를 적용해야 한다(SHALL). 페이지는 수신 전용 비디오 트랜시버로 offer를 만든다.

소스 목록:

- 페이지가 처음 열릴 때 페이지는 `GET /api/sources`로 소스 목록을 받는다.
- 페이지는 소스마다 `option` 요소를 하나 만든다. 페이지는 `option` 요소를 `select` 요소에 목록 순서대로 넣는다.
- 페이지는 첫 소스를 선택해 둔다.
- `option`의 표시 글자는 종류 이름과 소스 이름이다. 종류 이름은 `file`이 "파일", `camera`가 "카메라", `recording`이 "녹화"다.
- 목록이 비었거나 페이지가 목록을 받지 못하면, 페이지는 `select` 요소에 그 사실을 표시한다. 이 경우 페이지는 `type`과 `name` 없이 연결한다.

연결:

- 연결할 때 페이지는 선택한 소스의 `type`과 `name`을 각각 `encodeURIComponent`로 인코딩한다. 페이지는 인코딩한 값을 `type`과 `name` 쿼리 매개변수로 붙인다.
- 선택한 소스의 `type`이 `recording`이면 페이지는 offer를 보내지 않는다. 페이지는 이전 연결을 닫는다. 그다음 페이지는 `recording-playback`의 "웹 페이지의 녹화 재생"을 따른다.
- 연결 버튼을 다시 누르면 페이지는 이전 연결의 영상을 지운다. 그다음 페이지는 새 연결의 영상을 표시한다.

상태 표시:

- 페이지는 `iceConnectionState`의 현재 값을 화면에 표시한다.
- 페이지는 `connectionState`의 현재 값을 화면에 표시한다.
- 페이지는 시그널링 실패 원인을 화면에 표시한다.
- `connectionState`가 `connected`가 되면 페이지는 선택된 transport의 `dtlsState`와 `srtpCipher`도 표시한다.
- 페이지는 `<video>` 요소를 둔다. 페이지는 받은 비디오 트랙을 그 요소에 자동 재생(음소거)으로 표시한다.

#### Scenario: Windows Chrome에서 ICE 연결
- **WHEN** WSL2에서 서버를 실행한다. 사용자가 Windows Chrome으로 페이지에 접속해 연결 버튼을 누른다
- **THEN** 화면의 ICE 상태가 `checking`을 거쳐 `connected`로 바뀐다

#### Scenario: DTLS 연결 완료
- **WHEN** ICE 상태가 `connected`가 된다. 그 뒤 DTLS 핸드셰이크가 끝난다
- **THEN** 화면의 연결 상태가 `connected`로 바뀐다. 화면에 DTLS 상태 `connected`와 SRTP 암호 `SRTP_AES128_CM_HMAC_SHA1_80`이 표시된다

#### Scenario: 시그널링 실패 표시
- **WHEN** 서버가 offer에 `400 Bad Request`로 응답한다
- **THEN** 페이지는 응답 상태 코드를 화면에 표시한다

#### Scenario: 목록 표시
- **WHEN** 동영상 두 개가 있는 디렉터리를 지정해 서버를 실행한다. 사용자가 서버의 페이지를 연다
- **THEN** `select` 요소에 `option` 요소 두 개가 목록 순서대로 나타난다. 각 `option`은 "파일"과 동영상 이름을 표시한다. 첫 번째 `option`이 선택되어 있다

#### Scenario: 영상 표시
- **WHEN** 사용자가 `select` 요소에서 동영상을 고른다. 사용자가 연결 버튼을 누른다
- **THEN** 페이지는 `type=file`과 고른 이름으로 offer를 보낸다. 연결 상태가 `connected`가 된 뒤 `<video>` 요소에 고른 동영상이 재생된다

#### Scenario: 다시 연결
- **WHEN** 영상이 재생되는 중에 사용자가 `select` 요소에서 다른 동영상을 고른다. 사용자가 연결 버튼을 누른다
- **THEN** 이전 연결이 닫힌다. 새로 고른 동영상이 처음부터 재생된다

#### Scenario: 동영상 없는 서버
- **WHEN** `--video-dir` 없이 서버를 실행한다. 사용자가 서버의 페이지를 열고 연결 버튼을 누른다
- **THEN** `select` 요소에 소스가 없다는 문구가 표시된다. 연결 상태는 `connected`가 된다. 영상은 표시되지 않는다

#### Scenario: 녹화 소스 표시
- **WHEN** 서버의 녹화 디렉터리에 닫힌 녹화 파일이 있는 `cam` 폴더가 있다. 사용자가 서버의 페이지를 연다
- **THEN** `select` 요소에 "녹화 · cam" `option` 요소가 있다

#### Scenario: 녹화 소스를 고르면 offer를 보내지 않음
- **WHEN** 사용자가 WebRTC로 영상을 보고 있다. 사용자가 "녹화 · cam"을 골라 연결 버튼을 누른다
- **THEN** 이전 연결이 닫힌다. 페이지는 `POST /offer`를 보내지 않는다. 페이지는 `GET /api/recordings/playlist.m3u8?camera=cam`을 요청한다
