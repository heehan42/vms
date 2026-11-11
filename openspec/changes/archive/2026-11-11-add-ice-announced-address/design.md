# Design

## Context

변경 이유는 proposal.md의 "Why" 절에 있다. 요구 동작은 이 change의 `specs/`(`ice-lite`, `webrtc-signaling`)에 있다. 용어의 뜻은 [background.md](background.md)에 있다.

### 현재 상태: ICE 주소의 흐름

- `main.cpp`의 `ParseCommandLine()`은 `--ice-address` 값을 `ParseIpv4Address()`로 해석한다. 그다음 `ParseCommandLine()`은 결과를 `ServerOptions::ice_addresses`(`std::vector<in_addr>`)에 더한다.
- `ResolveIceAddresses()`는 `ice_addresses`가 비어 있으면 `ListNonLoopbackIpv4Addresses()`로 인터페이스 주소를 조회한다.
- `IceLiteAgent::Start()`는 `IceLiteAgentConfig::addresses`의 주소마다 UDP 소켓을 하나 bind한다.
- `IceLiteAgent`의 생성자는 소켓마다 `IceCandidate`를 하나 만든다. 이때 `foundation`은 소켓 순번 + 1이고, `priority`는 `ComputeHostPriority(소켓 순번)`이다.
- `Signaling`은 `IceLiteAgent::CreateSession()`이 돌려준 `IceSessionParameters::candidates`를 SDP answer에 그대로 쓴다.
- `main.cpp`는 시작할 때 `Candidates()`마다 `ICE 대기 중: udp <주소>:<포트>` 한 줄을 출력한다.

### 현재 상태: 연결 경로 식별

- `IceLiteAgent`는 nominated pair를 `NominatedPairKey`로 식별한다. 이 키는 소켓 순번(`socket_index`)과 브라우저 주소(`remote_address`)의 조합이다.
- `IceLiteAgent`는 패킷의 목적지 주소를 보지 않는다. `IceLiteAgent`는 패킷이 도착한 소켓만 본다.

그래서 소켓 하나에 candidate가 여러 개여도 연결 경로 식별은 바뀌지 않는다. [port forwarding](background.md#port-forwarding)으로 들어온 패킷도 bind address의 소켓에 도착한다.

## Goals / Non-Goals

**Goals:**

- 소켓 구성과 candidate 구성을 한 설정 값으로 표현한다. 소켓은 bind address마다 하나이고, candidate는 [announced address](background.md#announced-address)마다 하나다.
- `--ice-address` 값의 해석과 중복 검사는 `main.cpp` 밖에서 단위 테스트할 수 있다.
- `IceLiteAgent`의 STUN, DTLS, RTCP 처리는 바뀌지 않는다.

**Non-Goals:**

- 공유기, WSL2, Windows 방화벽 설정을 자동화하지 않는다. 이 설정은 README의 절차로만 다룬다.
- announced address로 실제 패킷이 도달하는지 시작할 때 검사하지 않는다.

## Decisions

### 1. 설정 구조: `IceListenAddress`

`src/webrtc/ice_listen_address.h`에 다음 구조체를 둔다.

```cpp
struct IceListenAddress {
    in_addr bind_address;
    std::vector<in_addr> announced_addresses;  // 비어 있지 않다. 앞의 주소일수록 priority가 높다.
};
```

`IceLiteAgentConfig::addresses`의 타입은 `std::vector<in_addr>`에서 `std::vector<IceListenAddress>`로 바뀐다.

이유는 다음과 같다.

1. 소켓은 bind address마다 하나다. 그래서 bind address를 기준으로 announced address를 묶으면 설정 구조가 소켓 구조와 같아진다.
2. `IceLiteAgent`는 소켓 순번으로 candidate를 찾을 필요가 없다. `IceLiteAgent`는 설정 순서대로 candidate를 만들기만 하면 된다.

검토한 대안은 `(bind address, announced address)` 쌍의 평평한 목록이다. 이 대안을 쓰면 같은 bind address의 쌍을 모아 소켓 하나로 합치는 단계가 따로 필요하다. 그래서 이 대안은 택하지 않는다.

### 2. `announced_addresses`는 항상 채운다

`=`가 없는 값과 자동 조회 결과도 `announced_addresses = {bind_address}`로 채운다. 이 변환은 `IceLiteAgent` 바깥에서 한다.

- `ParseIceListenAddress()`가 `=` 없는 값을 변환한다.
- `main.cpp`의 `ResolveIceAddresses()`가 자동 조회 결과를 변환한다.

`IceLiteAgent::Start()`는 `announced_addresses`가 빈 `IceListenAddress`를 받으면 오류를 돌려준다.

이유는 빈 목록의 뜻을 하나로 정하기 위해서다. 빈 목록에 "bind address를 알린다"는 뜻을 주면 `IceLiteAgent`와 해석 코드가 같은 규칙을 각각 알아야 한다.

### 3. 명령행 형식: `<bind>=<announced>,<announced>`

`--ice-address` 값의 형식은 `<bind address>[=<announced address>[,<announced address>]...]`다.

이유는 다음과 같다.

1. bind address가 여러 개일 때 announced address가 어느 소켓에 속하는지 값 하나 안에서 드러난다.
2. `--camera <이름>=<URL>`과 같은 `=` 구분 방식이다.

검토한 대안은 별도 옵션 `--ice-announced-address`를 반복하는 방식이다. 이 방식은 bind address가 둘 이상이면 announced address가 어느 소켓에 속하는지 정할 수 없다. 그래서 이 대안은 택하지 않는다.

해석 함수는 `src/webrtc/ice_listen_address.*`에 둔다.

- `ParseIceListenAddress(std::string_view)`는 값 하나를 `IceListenAddress`로 바꾼다. 실패하면 오류 메시지를 돌려준다.
- `CheckIceListenAddresses(std::span<const IceListenAddress>)`는 bind address 중복과 announced address 중복을 검사한다.

`main.cpp`는 두 함수를 부르기만 한다. 그래서 형식과 오류 조건을 `tests/webrtc/ice_listen_address_test.cpp`에서 단위 테스트할 수 있다.

### 4. 중복을 시작할 때 거부한다

다음 두 경우 서버는 시작하지 않는다.

- **같은 bind address를 두 번 지정한 경우.** 두 번째 bind는 어차피 `EADDRINUSE`로 실패한다. 하지만 이때 오류 메시지는 원인을 가리키지 않는다. 그래서 해석 단계에서 먼저 검사한다.
- **같은 announced address가 두 번 나오는 경우.** answer에 같은 주소의 candidate가 두 개 들어간다. 그러면 브라우저는 같은 주소로 connectivity check를 두 번 보낸다. 이 설정은 사용자의 실수일 가능성이 크다.

검사 범위는 모든 `--ice-address`에 걸친다. 자동 조회 결과는 중복이 없으므로 검사하지 않는다.

### 5. candidate 순번은 전체 목록에서 센다

`IceLiteAgent`의 생성자는 설정 순서대로 announced address를 하나씩 센다. 그다음 생성자는 그 순번으로 candidate 값을 정한다.

- `foundation`은 순번 + 1이다.
- `priority`는 `ComputeHostPriority(순번)`이다.
- `port`는 그 announced address가 속한 소켓의 실제 포트다.

예를 들어 `--ice-address 192.168.0.10=192.168.0.10,203.0.113.5 --ice-address 127.0.0.1`의 candidate는 다음과 같다.

| 순번 | 소켓 | announced address | foundation | priority |
|---|---|---|---|---|
| 0 | `192.168.0.10` | `192.168.0.10` | 1 | 2130706431 |
| 1 | `192.168.0.10` | `203.0.113.5` | 2 | 2130706175 |
| 2 | `127.0.0.1` | `127.0.0.1` | 3 | 2130705919 |

foundation을 candidate마다 다르게 두는 이유는 다음과 같다.

1. RFC 8445의 규칙대로라면 같은 base의 host candidate는 foundation이 같다.
2. 브라우저는 foundation이 같은 candidate pair를 frozen 상태로 두고 차례로 확인할 수 있다.
3. 그러면 외부 브라우저는 LAN 주소의 확인이 끝나기를 기다린 다음에야 public address를 확인할 수 있다.

announced address는 서로 다른 네트워크 경로다. 그래서 foundation을 나누어 브라우저가 모든 candidate를 함께 확인하도록 한다.

### 6. 시작 로그는 `main.cpp`가 설정에서 만든다

`main.cpp`는 `IceListenAddress`마다 다음 한 줄을 출력한다.

```text
ICE 대기 중: udp 192.168.0.10:50000, candidate 192.168.0.10, 203.0.113.5
```

포트는 `--ice-port` 값이다. `IceCandidate`는 bind address를 담지 않는다. 그래서 `Candidates()`로는 소켓별로 묶을 수 없다. `IceCandidate`에 bind address를 더하는 대안은 SDP와 관계없는 값을 SDP용 구조체에 넣는다. 그래서 이 대안은 택하지 않는다.

### 7. announced address는 정적으로 지정한다

서버는 public address를 자동으로 조회하지 않는다.

이유는 다음과 같다.

1. 가정용 public address는 자주 바뀌지 않는다. 주소가 바뀌면 서버를 새 값으로 다시 실행하면 된다.
2. 자동 조회는 서버가 시작할 때만 동작한다. 그래서 실행 중에 주소가 바뀌는 경우는 자동 조회로도 해결되지 않는다.
3. 자동 조회는 외부 서비스에 의존한다. 그러면 외부 서비스가 응답하지 않을 때 서버의 시작이 늦어지거나 실패한다.

옵션 형식은 나중에 자동 조회 표기를 더할 수 있다. 예를 들어 `=auto` 같은 값을 더할 수 있다.

### 8. STUN 서버와 TURN 서버를 두지 않는다

1. port forwarding을 설정하면 서버는 public address로 도달할 수 있다.
2. [ICE-lite](background.md#ice-lite)에서는 브라우저가 먼저 connectivity check를 보낸다.
3. 브라우저 쪽 NAT는 브라우저가 보낸 패킷을 기록한다. 그래서 그 NAT는 서버의 응답을 통과시킨다.

따라서 브라우저가 NAT 뒤에 있어도 연결된다. 서버는 자기 public address를 STUN으로 알아낼 필요도 없다. 사용자가 announced address로 지정하기 때문이다.

### 9. WSL2 환경은 mirrored mode와 loopback candidate로 문서화한다

README는 다음 구성을 안내한다.

- WSL2는 [mirrored mode](background.md#mirrored-mode)로 실행한다. [NAT mode](background.md#nat-mode)에서는 Windows가 외부 UDP 패킷을 WSL2로 전달하지 않는다.
- 같은 PC의 Windows 브라우저를 위해 `--ice-address 127.0.0.1`을 추가한다.

mirrored mode의 Windows는 자기 LAN 주소로 보낸 패킷을 기본 설정에서 WSL2로 전달하지 않는다. 그래서 같은 PC의 Windows 브라우저는 LAN 주소 candidate로 연결할 수 없다. 해결 방법은 두 가지를 검토했다.

| 방법 | 확인 결과 | 결정 |
|---|---|---|
| `--ice-address 127.0.0.1` 추가 | 개발 PC에서 Windows Chrome의 WebRTC 재생을 확인했다 | 택함 |
| `.wslconfig`의 `[experimental]` 절 `hostAddressLoopback=true` | 시험하지 않았다. 실험 기능이므로 WSL 버전에 따라 동작이 바뀔 수 있다 | README에 대안으로만 적음 |

loopback candidate는 외부 브라우저의 answer에도 들어간다. 외부 브라우저가 보낸 이 candidate의 확인은 그 브라우저 자신에게 돌아가 실패한다. 이 실패는 다른 candidate의 연결에 영향을 주지 않는다.

## Risks / Trade-offs

### 서버에 접근 제어가 없다

[Risk] port forwarding을 켜면 인터넷의 누구나 실시간 영상과 녹화를 볼 수 있다.
→ README에 경고를 적는다. 그리고 외부 데모를 할 때만 공유기의 port forwarding 규칙을 켜는 운영 방법을 적는다. 접근 제어는 다음 change에서 HTTPS와 함께 다룬다.

### public address가 바뀐다

[Risk] 공유기의 public address가 바뀌면 웹 페이지는 열린다. 하지만 answer의 candidate는 예전 주소를 가리킨다. 그래서 WebRTC 연결만 실패한다.
→ 시작 로그에 announced address를 출력한다(결정 6). 그리고 README에 이 증상과 확인 명령(`curl -4 ifconfig.me`)을 적는다.

### NAT hairpinning을 지원하지 않는 공유기

[Risk] 같은 Wi-Fi의 기기가 public address로 보낸 패킷이 공유기에서 되돌아오지 않을 수 있다([NAT hairpinning](background.md#nat-hairpinning)).
→ README의 예는 LAN 주소와 public address를 둘 다 announced address로 지정한다. LAN 주소를 앞에 두므로 같은 Wi-Fi의 기기는 LAN 주소로 연결된다.

### private address가 외부에 노출된다

[Risk] 외부 브라우저도 answer에서 LAN 주소와 `127.0.0.1`을 본다.
→ 이 주소들은 외부에서 도달할 수 없다. 그래서 실제 피해는 작다고 판단한다. 노출을 원하지 않는 사용자는 public address만 announced address로 지정하면 된다.

### UDP를 막는 네트워크

[Risk] 회사망이나 학교망처럼 외부 UDP를 막는 네트워크의 브라우저는 연결할 수 없다.
→ 이번 범위에서는 해결하지 않는다. ICE-TCP나 TURN을 다루는 이후 change의 몫이다.

### CGNAT

[Risk] 통신사가 [CGNAT](background.md#cgnat)를 쓰면 port forwarding 규칙을 설정할 수 없다.
→ README에 CGNAT 확인 방법을 적는다. 개발 PC의 회선은 CGNAT가 아님을 `tracert`로 확인했다.

## Migration Plan

- `=` 없는 `--ice-address` 값과 `--ice-address`를 생략한 실행은 지금과 같은 answer를 만든다. 그래서 기존 실행 명령과 통합 테스트는 그대로 동작한다.
- 되돌릴 때는 `--ice-address`에서 `=` 뒤를 지우면 된다.
