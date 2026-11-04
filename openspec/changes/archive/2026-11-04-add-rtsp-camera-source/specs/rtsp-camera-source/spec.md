# Spec Delta

## Purpose

명령행으로 등록한 RTSP 카메라에 서버가 상시 연결해 H.264 영상을 RTP로 받아 프레임으로 재조립하고, 끊기면 다시 연결한다. 받은 프레임은 카메라를 지정한 WebRTC 세션들이 함께 쓴다.

## ADDED Requirements

### Requirement: 카메라 등록 인자
서버는 `--camera <이름>=<URL>` 인자를 0번 이상 받아 카메라를 등록해야 한다(SHALL). 값은 첫 `=`를 기준으로 이름과 URL로 나눈다. 쓴 순서가 카메라 목록 순서다. 이름이 비었거나, UTF-8이 아니거나, 이미 등록한 카메라 이름과 같거나, `=`가 없으면 원인과 사용법을 표준 오류에 출력하고 0이 아닌 종료 코드로 끝난다. 카메라 이름은 파일 동영상 이름과 겹쳐도 된다.

#### Scenario: 카메라 두 대
- **WHEN** `--camera front=rtsp://127.0.0.1:8554/cam --camera back=rtsp://127.0.0.1:9554/cam`으로 실행한다
- **THEN** 서버는 시작하고, 카메라 목록은 `front`, `back` 순서다

#### Scenario: 겹치는 이름
- **WHEN** `--camera front=rtsp://127.0.0.1:8554/cam --camera front=rtsp://127.0.0.1:9554/cam`으로 실행한다
- **THEN** 서버는 이름이 겹친다는 원인을 출력하고 0이 아닌 종료 코드로 끝난다

#### Scenario: 형식이 틀린 값
- **WHEN** `--camera rtsp://127.0.0.1:8554/cam` 또는 `--camera =rtsp://127.0.0.1:8554/cam`으로 실행한다
- **THEN** 서버는 원인과 사용법을 출력하고 0이 아닌 종료 코드로 끝난다

### Requirement: 카메라 URL
서버는 카메라 URL을 `rtsp://<호스트>[:<포트>][/<경로>]` 형식으로 해석해야 한다(SHALL). 스킴은 대소문자를 구분하지 않고, 포트를 생략하면 554다. 호스트는 시작할 때 한 번 IPv4 주소로 해석한다. 스킴이 `rtsp`가 아니거나, 사용자 정보(`@`)가 있거나, 포트가 1~65535 정수가 아니거나, 호스트를 IPv4 주소로 해석할 수 없으면 원인을 출력하고 0이 아닌 종료 코드로 끝난다. 시작할 때 카메라에 연결할 수 없는 것은 오류가 아니다.

#### Scenario: 포트 생략
- **WHEN** `--camera front=rtsp://127.0.0.1/cam`으로 실행한다
- **THEN** 서버는 `127.0.0.1:554`로 연결을 시도한다

#### Scenario: 사용자 정보
- **WHEN** `--camera front=rtsp://user:pass@127.0.0.1:8554/cam`으로 실행한다
- **THEN** 서버는 인증을 지원하지 않는다는 원인을 출력하고 0이 아닌 종료 코드로 끝난다

#### Scenario: 카메라가 꺼져 있음
- **WHEN** 아무것도 듣고 있지 않은 포트를 가리키는 카메라로 실행한다
- **THEN** 서버는 시작하고, 그 카메라의 연결 실패와 재시도를 로그에 남긴다

### Requirement: RTSP 연결 절차
서버는 카메라마다 RTSP/1.0 제어 연결을 TCP로 맺고 `OPTIONS`, `DESCRIBE`, `SETUP`, `PLAY` 순서로 요청해야 한다(SHALL). 모든 요청에는 1부터 1씩 늘어나는 `CSeq`를 넣는다. `DESCRIBE`는 `Accept: application/sdp`로 보내고, 응답 SDP에서 `H264/90000` rtpmap을 가진 첫 비디오 미디어 섹션을 고른다. `SETUP` 대상은 그 섹션의 `a=control`을 `Content-Base` 또는 요청 URL 기준으로 해석한 주소다. `SETUP` 응답의 `Session`을 이후 요청에 넣는다. 응답 상태가 2xx가 아니거나 고를 섹션이 없으면 연결 실패로 본다.

#### Scenario: vms-cam 연결
- **WHEN** `vms_camera_server --test-source`를 실행하고 `--camera cam=rtsp://127.0.0.1:8554/cam`으로 서버를 실행한다
- **THEN** 서버는 `PLAY` 응답까지 받고, 카메라 연결 성공과 해상도·payload type을 로그에 남긴다

#### Scenario: 없는 경로
- **WHEN** 카메라 URL의 경로가 서버에 없어 `DESCRIBE`에 `404 Not Found`가 온다
- **THEN** 서버는 응답 상태를 원인으로 연결 실패를 로그에 남기고 재연결 간격 뒤에 다시 시도한다

### Requirement: UDP 미디어 수신
서버는 `SETUP`의 `Transport`에 `RTP/AVP;unicast;client_port=<n>-<n+1>`을 넣어야 한다(SHALL). `n`은 서버가 바인드한 짝수 UDP 포트이며 `n+1`도 함께 바인드한다. RTP는 `n`, RTCP는 `n+1`로 받는다. 출발 IPv4 주소가 카메라 호스트 주소와 다른 패킷은 버린다. RTCP 패킷은 읽어서 버린다. 응답 `Transport`가 UDP 유니캐스트가 아니면 연결 실패로 본다.

#### Scenario: 포트 쌍 알림
- **WHEN** 서버가 카메라에 `SETUP`을 보낸다
- **THEN** `client_port`의 첫 포트는 짝수이고 두 번째 포트는 첫 포트에 1을 더한 값이며, 서버는 두 포트에서 모두 패킷을 받는다

#### Scenario: 다른 주소의 패킷
- **WHEN** 카메라 호스트가 아닌 주소에서 RTP 포트로 패킷을 보낸다
- **THEN** 서버는 그 패킷을 프레임 재조립에 쓰지 않는다

### Requirement: H.264 프레임 재조립
서버는 고른 payload type의 RTP 패킷을 RFC 6184에 따라 H.264 NAL 단위로 되돌리고 프레임으로 묶어야 한다(SHALL). 단일 NAL(유형 1~23), STAP-A(24), FU-A(28)를 지원하며, 다른 유형의 패킷은 버린다. 같은 타임스탬프의 NAL 단위가 한 프레임이며, marker 비트가 켜진 패킷이나 타임스탬프가 바뀐 패킷에서 앞 프레임을 끝낸다. 순번이 1씩 늘지 않으면(빠짐·순서 바뀜·중복) 만들던 프레임을 버리고 손실로 알린다. 4MiB를 넘는 프레임도 버리고 손실로 알린다.

#### Scenario: FU-A 재조립
- **WHEN** 3000바이트 IDR NAL을 FU-A 패킷 3개로 나눠 순서대로 받는다
- **THEN** 재조립한 프레임에는 원래 NAL 하나가 들어 있다

#### Scenario: STAP-A
- **WHEN** SPS와 PPS를 담은 STAP-A 패킷 하나와 IDR 단일 NAL 패킷(marker 켜짐)을 같은 타임스탬프로 받는다
- **THEN** 재조립한 프레임은 SPS, PPS, IDR 순서의 NAL 세 개를 가진다

#### Scenario: 빠진 조각
- **WHEN** FU-A 패킷 3개 중 가운데 패킷이 빠진다
- **THEN** 그 프레임은 버려지고 손실로 알려진다

### Requirement: 세션 유지와 끊김 감지
서버는 `PLAY` 성공 뒤 `SETUP` 응답 `Session`의 `timeout`(없으면 60초)의 절반마다 `Session`을 넣은 `OPTIONS`를 보내야 한다(SHALL). 다음 중 하나가 되면 카메라 연결이 끊긴 것으로 보고 제어 연결과 UDP 소켓을 닫는다: 제어 연결이 닫히거나 오류가 남, 요청 뒤 5초 안에 응답이 끝나지 않음, 2xx가 아닌 응답, `PLAY` 성공 뒤 3초 동안 RTP 패킷이 없음. 끊긴 사실과 원인을 카메라마다 로그에 남긴다.

#### Scenario: 카메라 서버 종료
- **WHEN** 연결된 `vms_camera_server`를 Ctrl+C로 끝낸다
- **THEN** 서버는 3초 안에 그 카메라가 끊겼다는 로그를 남긴다

#### Scenario: 긴 연결
- **WHEN** vms-cam에 연결한 채 3분 동안 둔다
- **THEN** 카메라 연결은 끊기지 않고 RTP를 계속 받는다

### Requirement: 재연결
서버는 카메라 연결이 실패하거나 끊기면 1초 뒤 다시 연결을 시도하고, 연속으로 실패할 때마다 간격을 두 배로 늘리되 10초를 넘기지 않아야 한다(SHALL). `PLAY`에 성공하면 간격을 1초로 되돌린다. 서버가 종료 시그널을 받으면 연결된 카메라에 `TEARDOWN`을 보내고(응답은 기다리지 않음) 소켓을 닫는다.

#### Scenario: 카메라 재시작
- **WHEN** 연결된 `vms_camera_server`를 끝내고 5초 뒤 다시 실행한다
- **THEN** 서버는 10초 안에 그 카메라에 다시 연결했다는 로그를 남긴다

#### Scenario: 재시도 간격
- **WHEN** 카메라가 계속 꺼져 있다
- **THEN** 연결 시도 로그의 간격은 약 1, 2, 4, 8, 10, 10초로 늘어난다
