# Background

## 읽는 순서

이 문서를 먼저 읽는다. 그다음 proposal.md, specs, design.md, tasks.md 순서로 읽는다.

이 change의 큰 흐름은 다음과 같다.

1. 집의 서버는 공유기 뒤에 있다. 그래서 서버는 인터넷에서 쓰는 주소를 소켓에 붙일 수 없다.
2. 공유기는 port forwarding 규칙으로 외부 패킷을 서버에 전달한다.
3. 서버는 소켓에 붙인 주소와 별개로, 외부 브라우저가 보낼 주소를 SDP answer에 알린다.
4. 개발 PC에서는 WSL2의 네트워크 방식과 Windows 방화벽도 외부 패킷을 통과시켜야 한다.

## 개념

### 주소와 NAT

#### private address

private address는 내부망 안에서만 쓰는 IPv4 주소다.

- **왜 필요한가.** IPv4 주소는 수가 부족하다. 그래서 가정과 회사는 내부 기기에 private address를 주고, 인터넷 쪽에는 주소 하나만 쓴다.
- **동작과 예시.** 범위는 `10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`이다. 인터넷의 라우터는 이 범위로 가는 패킷을 전달하지 않는다. 예를 들어 개발 PC의 Wi-Fi 주소 `192.168.219.105`는 private address다.
- **이 change에서.** 서버가 bind할 수 있는 주소는 private address뿐이다(proposal "Why").

#### public address

public address는 인터넷 전체에서 하나뿐인 IPv4 주소다.

- **왜 필요한가.** 외부 브라우저는 public address로만 집의 공유기에 패킷을 보낼 수 있다.
- **동작과 예시.** 가정에서는 통신사가 공유기에 public address 하나를 준다. 이 문서의 예는 문서용 주소 `203.0.113.5`(RFC 5737)를 쓴다. 가정용 public address는 보통 동적으로 배정된다. 그래서 공유기를 다시 켜거나 오래 지나면 바뀔 수 있다.
- **이 change에서.** 외부 브라우저용 [announced address](#announced-address)로 쓴다(design 결정 7, Risks "public address가 바뀐다").

#### NAT

NAT(Network Address Translation)는 패킷의 주소를 바꾸는 기능이다.

- **왜 필요한가.** 내부 기기 여러 대가 public address 하나로 인터넷을 쓰게 한다.
- **동작과 예시.** 공유기는 다음 순서로 NAT를 수행한다.
  1. 내부 기기가 외부로 패킷을 보낸다.
  2. 공유기는 패킷의 출발지를 내부 주소에서 public address와 공유기 포트로 바꾼다.
  3. 공유기는 이 대응 관계를 표에 기록한다.
  4. 응답이 오면 공유기는 표를 보고 목적지를 내부 주소로 되돌린다.

  외부에서 먼저 보낸 패킷은 표에 대응 관계가 없다. 그래서 공유기는 그 패킷을 버린다.
- **이 change에서.** 서버로 오는 첫 패킷은 브라우저가 먼저 보낸다. 그래서 공유기에 [port forwarding](#port-forwarding) 규칙이 필요하다. 반대로 브라우저 쪽 NAT는 브라우저가 먼저 보냈으므로 서버의 응답을 통과시킨다(design 결정 8).

#### port forwarding

port forwarding은 외부 포트로 온 패킷을 내부 주소로 보내는 공유기의 고정 규칙이다.

- **왜 필요한가.** [NAT](#nat)는 외부에서 먼저 보낸 패킷을 버린다. port forwarding 규칙이 있으면 그 패킷도 내부 기기에 도착한다.
- **동작과 예시.** 규칙은 프로토콜, 외부 포트, 내부 주소, 내부 포트로 이루어진다. 이 change에서 쓸 규칙은 다음 두 개다. 규칙은 클라이언트와 관계없이 모든 외부 패킷에 적용된다. TCP 규칙은 UDP 패킷에 적용되지 않는다.

  | 프로토콜 | 외부 포트 | 내부 주소 | 내부 포트 |
  |---|---|---|---|
  | TCP | 8080 | `192.168.219.105` | 8080 |
  | UDP | 50000 | `192.168.219.105` | 50000 |

  공유기는 외부 패킷의 목적지를 `203.0.113.5:50000`에서 `192.168.219.105:50000`으로 바꾼다. 서버의 응답은 반대로 출발지가 `203.0.113.5:50000`으로 바뀐다.
- **이 change에서.** README의 "외부 네트워크에서 접속하기" 절(tasks 4.3). design의 "현재 상태: 연결 경로 식별".

#### NAT hairpinning

NAT hairpinning은 내부 기기가 자기 공유기의 public address로 보낸 패킷을 공유기가 다시 내부로 돌려보내는 기능이다.

- **왜 필요한가.** 같은 Wi-Fi의 기기가 public address로 서버에 접속할 때 필요하다.
- **동작과 예시.** 이 기능을 지원하지 않는 공유기도 있다. 그런 공유기에서는 같은 Wi-Fi의 휴대폰이 `203.0.113.5:50000`으로 보낸 패킷이 서버에 도착하지 않는다.
- **이 change에서.** LAN 주소를 announced address에 함께 넣는 이유다(design Risks "NAT hairpinning을 지원하지 않는 공유기").

#### CGNAT

CGNAT(Carrier-Grade NAT)는 통신사가 공유기 바깥에 한 번 더 두는 [NAT](#nat)다.

- **왜 필요한가.** 통신사도 IPv4 주소가 부족하다. 그래서 여러 가입자가 public address 하나를 나누어 쓴다.
- **동작과 예시.** CGNAT 환경에서 공유기의 WAN 주소는 `100.64.0.0/10` 범위다. 사용자는 통신사 장비에 port forwarding 규칙을 넣을 수 없다. 확인 방법은 두 가지다. 공유기 관리 화면의 WAN 주소와 `curl -4 ifconfig.me`의 결과를 비교한다. 또는 `tracert -d 8.8.8.8`의 두 번째 hop이 `100.64.0.0/10` 범위인지 본다.
- **이 change에서.** README의 CGNAT 확인 절차(tasks 4.3). design Risks "CGNAT".

### 소켓과 candidate

#### bind address

bind address는 `bind()` 시스템 호출로 UDP 소켓에 붙이는 로컬 IPv4 주소다.

- **왜 필요한가.** 소켓은 bind address로 온 패킷만 받는다.
- **동작과 예시.** 프로그램은 자기 머신의 인터페이스에 있는 주소만 bind할 수 있다. 그래서 공유기가 가진 `203.0.113.5`를 서버가 bind하면 `EADDRNOTAVAIL` 오류가 난다.
- **이 change에서.** `IceListenAddress::bind_address`(design 결정 1). spec `ice-lite` "ICE 실행 설정".

#### candidate

candidate는 상대에게 "이 주소와 포트로 보내라"고 알리는 후보 주소다. ICE(RFC 8445)가 정의한다.

- **왜 필요한가.** 두 peer 사이에는 여러 네트워크 경로가 있을 수 있다. ICE는 candidate 쌍을 하나씩 확인해 실제로 통하는 경로를 찾는다.
- **동작과 예시.** SDP에서는 다음 형식의 줄로 쓴다.

  ```text
  a=candidate:<foundation> 1 udp <priority> <주소> <포트> typ host
  a=candidate:2 1 udp 2130706175 203.0.113.5 50000 typ host
  ```
- **이 change에서.** spec `webrtc-signaling` "Answer 내용".

#### host candidate

host candidate는 `typ host`로 표시한 candidate다. 원래는 인터페이스에 실제로 있는 주소로 만든다.

- **왜 필요한가.** 서버가 알릴 수 있는 가장 단순한 candidate다.
- **동작과 예시.** 이 change에서 서버는 public address도 `typ host`로 알린다. 브라우저는 candidate의 종류와 관계없이 그 주소로 확인을 보낸다. 다른 미디어 서버(예: mediasoup)도 같은 방식을 쓴다.
- **이 change에서.** proposal "SDP answer의 candidate".

#### candidate priority

candidate priority는 브라우저가 candidate를 확인하는 순서를 정하는 32비트 값이다. 값이 클수록 먼저 확인한다.

- **왜 필요한가.** 경로가 여러 개 통하면 브라우저는 priority가 높은 경로를 고른다.
- **동작과 예시.** RFC 8445의 공식은 `2^24 × type preference + 2^8 × local preference + (256 − component ID)`다. host candidate의 type preference는 126이고 component ID는 1이다. 서버는 local preference를 `65535 − 순번`으로 정한다.

  | 순번 | 계산 | priority |
  |---|---|---|
  | 0 | 126×2^24 + 65535×2^8 + 255 | 2130706431 |
  | 1 | 126×2^24 + 65534×2^8 + 255 | 2130706175 |
  | 2 | 126×2^24 + 65533×2^8 + 255 | 2130705919 |
- **이 change에서.** design 결정 5. spec `webrtc-signaling` "Answer 내용".

#### foundation

foundation은 같은 종류, 같은 base 주소의 candidate를 묶는 식별자다.

- **왜 필요한가.** RFC 8445의 full agent는 foundation이 같은 candidate pair를 frozen 상태로 두고 차례로 확인할 수 있다. 같은 경로일 가능성이 큰 pair를 한꺼번에 확인하지 않기 위해서다.
- **동작과 예시.** 서버는 candidate마다 foundation을 1, 2, 3처럼 다르게 준다.
- **이 change에서.** design 결정 5.

#### ICE-lite

ICE-lite는 RFC 8445가 정의한 간소화된 ICE 구현이다. public address로 도달할 수 있는 서버를 위한 방식이다.

- **왜 필요한가.** 서버는 자기 쪽 경로를 탐색할 필요가 없다. 그래서 서버 구현이 단순해진다.
- **동작과 예시.**
  1. ICE-lite 서버는 host candidate만 알린다.
  2. ICE-lite 서버는 connectivity check를 먼저 보내지 않는다.
  3. ICE-lite 서버는 받은 connectivity check에 응답만 한다.

  connectivity check는 STUN Binding 요청(RFC 8489)이다.
- **이 change에서.** design 결정 8. 서버가 STUN 서버나 TURN 서버 없이 외부 브라우저와 연결되는 이유다.

#### announced address

announced address는 [bind address](#bind-address)와 별개로 SDP answer의 candidate에 적는 주소다. 이 change에서 정한 용어다.

- **왜 필요한가.** 서버가 bind할 수 있는 주소와 외부 브라우저가 보낼 주소가 다르기 때문이다.
- **동작과 예시.** bind address 하나에 announced address가 여러 개일 수 있다.

  ```text
  --ice-address 192.168.219.105=192.168.219.105,203.0.113.5
  ```

  이 값으로 서버는 소켓 하나를 `192.168.219.105:50000`에 bind한다. 그리고 서버는 candidate 두 개를 알린다. 같은 Wi-Fi의 기기는 `192.168.219.105`로 도달한다. 외부 기기는 `203.0.113.5`로 보내고, 공유기가 그 패킷을 같은 소켓으로 전달한다.
- **이 change에서.** `IceListenAddress::announced_addresses`(design 결정 1~3). spec `ice-lite` "ICE 실행 설정".

### 개발 PC의 네트워크

#### NAT mode

NAT mode는 WSL2의 기본 네트워크 방식이다.

- **왜 필요한가.** WSL2를 Windows 네트워크와 분리한다.
- **동작과 예시.** WSL2는 Windows 안의 가상 네트워크에 있다. WSL2의 `eth0` 주소는 `172.x.x.x` 같은 주소다. Windows 밖의 기기는 이 주소로 패킷을 보낼 수 없다. Windows의 `netsh interface portproxy`는 외부 연결을 WSL2로 넘길 수 있다. 하지만 이 명령은 TCP만 전달한다.
- **이 change에서.** NAT mode에서는 외부 UDP 패킷이 서버에 도착하지 않는다(design 결정 9).

#### mirrored mode

mirrored mode는 WSL2가 Windows의 네트워크 인터페이스를 함께 쓰는 방식이다.

- **왜 필요한가.** WSL2 안의 서버가 Windows의 LAN 주소를 bind할 수 있다. 그래서 외부 UDP 패킷도 서버에 도착한다.
- **동작과 예시.** `%UserProfile%\.wslconfig`의 `[wsl2]` 절에 `networkingMode=mirrored`를 쓴다. 그다음 `wsl --shutdown`으로 WSL2를 다시 시작한다. `wslinfo --networking-mode`의 결과가 `mirrored`이면 적용된 것이다. mirrored mode에는 두 가지 특징이 있다.
  1. Windows의 `localhost`로 보낸 TCP와 UDP는 WSL2로 전달된다.
  2. Windows가 자기 LAN 주소로 보낸 패킷은 기본 설정에서 WSL2로 전달되지 않는다. `.wslconfig`의 `[experimental]` 절 `hostAddressLoopback=true`가 이 동작을 바꾼다.
- **이 change에서.** 같은 PC의 Windows 브라우저를 위해 `--ice-address 127.0.0.1`을 더하는 이유다(design 결정 9).

#### Hyper-V 방화벽

Hyper-V 방화벽은 WSL2 같은 Hyper-V 가상 머신으로 들어오는 연결을 거르는 Windows 방화벽이다.

- **왜 필요한가.** mirrored mode에서 외부 기기가 WSL2로 보낸 연결은 이 방화벽을 통과해야 한다.
- **동작과 예시.** 관리자 권한 PowerShell에서 규칙을 더한다. `{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}`은 Microsoft가 공개한 WSL의 VM 식별자다.

  ```text
  New-NetFirewallHyperVRule -Name "vms-ice" -DisplayName "vms ICE" -Direction Inbound -VMCreatorId "{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}" -Protocol UDP -LocalPorts 50000
  ```

  규칙은 출발지와 관계없이 그 포트의 연결을 허용한다. 실제로 도달할 수 있는 기기의 범위는 공유기의 port forwarding 규칙이 정한다.
- **이 change에서.** README의 "외부 네트워크에서 접속하기" 절(tasks 4.3).

#### DHCP 고정 할당

DHCP 고정 할당은 공유기가 특정 기기에 항상 같은 private address를 주도록 하는 설정이다.

- **왜 필요한가.** port forwarding 규칙의 내부 주소와 `--ice-address`의 bind address는 PC의 LAN 주소로 고정되어 있다. PC의 주소가 바뀌면 두 설정이 모두 맞지 않게 된다.
- **동작과 예시.** 공유기 관리 화면에서 PC의 MAC 주소에 `192.168.219.105`를 지정한다.
- **이 change에서.** README의 "외부 네트워크에서 접속하기" 절(tasks 4.3).

## 참고 문서

| 용어 | 문서 | 절 |
| --- | --- | --- |
| private address | RFC 1918 | 3 |
| 문서용 주소(`203.0.113.0/24`) | RFC 5737 | 3 |
| NAT | RFC 3022 | 2 |
| NAT hairpinning | RFC 4787 | 6 |
| CGNAT 주소 범위 | RFC 6598 | 7 |
| candidate, host candidate, foundation | RFC 8445 | 5.1.1 |
| candidate priority | RFC 8445 | 5.1.2 |
| ICE-lite | RFC 8445 | 2.5 |
| connectivity check(STUN Binding) | RFC 8489 | 3 |
| SDP의 candidate 줄 | RFC 8839 | 5.1 |
| WSL2 mirrored mode, `hostAddressLoopback` | Microsoft Learn "Accessing network applications with WSL", "Advanced settings configuration in WSL" | Mirrored mode networking, `.wslconfig` |
| Hyper-V 방화벽 | Microsoft Learn "Hyper-V Firewall" | Configuring Hyper-V Firewall |
