# Spec Delta

## MODIFIED Requirements

### Requirement: ICE 실행 설정
서버는 ICE용 UDP 포트(`--ice-port`, 기본 50000)와 ICE 주소(`--ice-address`, 여러 번 지정 가능)를 명령행 인자로 받아야 한다(SHALL). `--ice-address` 값의 형식은 `<bind address>[=<announced address>[,<announced address>]...]`이다. bind address는 이 머신의 인터페이스에 있는 주소여야 한다. 서버는 bind address마다 그 주소와 포트에 UDP 소켓을 하나 연다. announced address는 그 소켓의 candidate로 알릴 주소이며, 이 머신의 인터페이스에 없어도 된다. `=`가 없으면 서버는 bind address 하나를 announced address로 쓴다. `--ice-address`를 지정하지 않으면 서버는 동작 중인 인터페이스의 IPv4 주소 중 루프백이 아닌 것을 모두 bind address로 쓰고, 각 주소를 그대로 알린다. 서버는 시작할 때 bind address마다 bind address, 포트, announced address 목록을 로그에 출력한다.

#### Scenario: 기본값으로 실행
- **WHEN** ICE 인자 없이 서버를 실행한다
- **THEN** 서버는 루프백이 아닌 각 IPv4 인터페이스 주소의 50000번 UDP 포트에서 패킷을 받고, 사용하는 주소 목록을 로그에 출력한다

#### Scenario: 주소 지정
- **WHEN** 인터페이스 주소가 `127.0.0.1`과 `192.168.0.10`인 머신에서 `--ice-port 50100 --ice-address 127.0.0.1 --ice-address 192.168.0.10`으로 실행한다
- **THEN** 서버는 두 주소의 50100번 UDP 포트에서만 패킷을 받고, answer에는 두 주소의 candidate만 들어간다

#### Scenario: announced address 지정
- **WHEN** 인터페이스 주소가 `192.168.0.10`인 머신에서 `--ice-address 192.168.0.10=192.168.0.10,203.0.113.5`로 실행한다
- **THEN** 서버는 `192.168.0.10`의 50000번 UDP 포트 하나에서만 패킷을 받고, answer에는 `192.168.0.10`과 `203.0.113.5`의 candidate가 들어가며, 시작 로그의 한 줄에 bind address `192.168.0.10:50000`과 announced address 두 개가 나온다

#### Scenario: announced address로 보낸 연결 확인
- **WHEN** 공유기가 announced address `203.0.113.5:50000`으로 온 Binding 요청의 목적지를 bind address `192.168.0.10:50000`으로 바꿔 전달한다
- **THEN** 서버는 그 요청을 bind address의 소켓에서 받아 다른 요청과 같은 규칙으로 응답한다

#### Scenario: ICE 시작 실패
- **WHEN** 지정한 bind address나 announced address가 IPv4 형식이 아니거나, `=` 뒤에 주소가 없거나, bind address가 이 머신의 인터페이스에 없거나, 같은 bind address를 두 번 지정했거나, 같은 announced address가 두 번 나오거나, UDP 포트에 바인드할 수 없거나, 사용할 주소가 하나도 없다
- **THEN** 서버는 원인을 표준 오류에 출력하고 0이 아닌 종료 코드로 끝난다
