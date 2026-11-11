# Proposal

## Why

지금 서버는 같은 PC나 같은 내부망의 브라우저에만 WebRTC 영상을 보낼 수 있다. 원인은 `--ice-address` 값 하나가 두 역할을 함께 하기 때문이다.

1. `--ice-address` 값은 UDP 소켓의 [bind address](background.md#bind-address)가 된다.
2. 같은 값이 SDP answer의 [host candidate](background.md#host-candidate) 주소가 된다.

공유기 뒤에 있는 서버는 [public address](background.md#public-address)를 bind할 수 없다. 그래서 answer에는 [private address](background.md#private-address)만 들어간다. 외부 네트워크의 브라우저는 private address로 패킷을 보낼 수 없다.

프로젝트의 다음 단계는 외부 네트워크의 브라우저(예: LTE 휴대폰)가 집의 서버에 접속해 카메라 영상을 재생하는 것이다. 이 단계에는 bind address와 candidate 주소를 따로 지정하는 기능이 필요하다.

## What Changes

### `--ice-address` 형식

- `--ice-address`는 `<bind address>[=<announced address>[,<announced address>]...]` 형식을 받는다. [announced address](background.md#announced-address)는 bind address와 별개로 SDP answer의 candidate에 적는 주소다.
- `=`가 없으면 서버는 bind address를 그대로 알린다. 이 형식은 지금 동작과 같다.
- `--ice-address`를 생략하면 서버는 지금처럼 인터페이스 주소를 자동으로 조회한다. 서버는 조회한 주소를 그대로 알린다.
- announced address는 이 머신의 인터페이스에 없어도 된다.
- 다음 경우 서버는 원인을 표준 오류에 출력한다. 그다음 서버는 0이 아닌 종료 코드로 끝난다.
  - `=` 뒤에 주소가 없는 경우
  - announced address가 IPv4 형식이 아닌 경우
  - 같은 bind address를 두 번 지정한 경우
  - 같은 announced address가 두 번 나오는 경우

### SDP answer의 candidate

- 서버는 announced address마다 host candidate를 하나씩 만든다.
- candidate의 포트는 그 announced address가 속한 소켓의 포트다.
- candidate의 [priority](background.md#candidate-priority)는 명령행에 쓴 순서대로 높다. 이 순서는 여러 `--ice-address`에 걸쳐 이어진다.
- candidate의 [foundation](background.md#foundation)은 candidate마다 다르다.

### 시작 로그

서버는 bind address마다 한 줄을 출력한다. 한 줄에는 bind address, 포트, 그 소켓의 announced address 목록이 들어간다.

### 문서

README에 다음 내용을 적는다.

- 바뀐 `--ice-address` 형식
- WSL2의 [NAT mode](background.md#nat-mode)와 [mirrored mode](background.md#mirrored-mode)에서의 실행 방법
- 외부 네트워크에서 접속하는 절차
  1. mirrored mode 설정
  2. [Hyper-V 방화벽](background.md#hyper-v-방화벽) 규칙
  3. 공유기의 [port forwarding](background.md#port-forwarding) 규칙
  4. [DHCP 고정 할당](background.md#dhcp-고정-할당)
  5. `--ice-address` 지정 예
- 같은 PC의 Windows 브라우저로 볼 때 `--ice-address 127.0.0.1`을 추가하는 방법
- public address가 바뀌었을 때의 증상과 확인 방법
- 서버에 접근 제어가 없다는 경고. 그래서 외부 데모를 할 때만 port forwarding 규칙을 켜는 운영 방법

### 범위 밖

- 접근 제어(인증, HTTPS)
- IPv6 candidate
- public address 자동 조회
- UPnP IGD, NAT-PMP로 공유기에 port forwarding 규칙을 요청하는 기능
- ICE-TCP, TURN. 그래서 UDP를 막는 네트워크의 브라우저는 계속 연결할 수 없다.
- STUN 서버

## Capabilities

### New Capabilities

없음.

### Modified Capabilities

- `ice-lite`: "ICE 실행 설정"을 고친다. `--ice-address`에 announced address를 지정하는 형식과 그 오류 조건을 더한다.
- `webrtc-signaling`: "Answer 내용"을 고친다. answer의 host candidate는 ICE 주소마다가 아니라 announced address마다 하나씩 들어간다.

## Impact

### 새 코드

- `src/webrtc/ice_listen_address.*`: bind address 하나와 announced address 목록을 묶는 `IceListenAddress`, `--ice-address` 값 해석, 중복 검사
- `tests/webrtc/ice_listen_address_test.cpp`: 해석과 중복 검사의 단위 테스트

### 바뀌는 코드

- `src/webrtc/ice_lite_agent.*`: `IceLiteAgentConfig::addresses`의 타입을 `std::vector<in_addr>`에서 `std::vector<IceListenAddress>`로 바꾼다. candidate는 announced address마다 만든다.
- `src/apps/web_server/main.cpp`: `--ice-address` 해석, 자동 조회 결과의 변환, 사용법 문자열, 시작 로그
- `tests/webrtc/ice_lite_agent_test.cpp`: 소켓 하나에 announced address가 여러 개인 경우
- `tests/integration/http_server_test.sh`: answer의 candidate 목록과 시작 실패
- `README.md`

### 호환성

- `=` 없는 `--ice-address` 값과 `--ice-address`를 생략한 실행은 지금과 같게 동작한다.
- 서버의 빌드 의존성은 바뀌지 않는다.
