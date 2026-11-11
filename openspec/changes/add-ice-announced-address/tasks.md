# Tasks

## 1. `IceListenAddress`와 값 해석

- [x] 1.1 `src/webrtc/ice_listen_address.h`에 `IceListenAddress`를 추가한다(design 결정 1). `server_core`에 새 소스를 등록한다. `tests/webrtc/CMakeLists.txt`에 `add_unit_test(ice_listen_address_test)`를 더한다. 빌드가 성공하는지 확인한다.
- [x] 1.2 `ParseIceListenAddress()`를 구현한다(design 결정 2, 3). `tests/webrtc/ice_listen_address_test.cpp`에서 다음 경우를 검사한다.
  - `192.168.0.10` → `announced_addresses`가 `{192.168.0.10}`
  - `192.168.0.10=192.168.0.10,203.0.113.5` → announced address 두 개가 쓴 순서대로 들어감
  - `192.168.0.10=203.0.113.5` → bind address가 announced address에 들어가지 않음
  - 오류: `192.168.0.10=`, `192.168.0.10=,203.0.113.5`, `192.168.0.10=bad`, `bad=203.0.113.5`, `=203.0.113.5`
- [x] 1.3 `CheckIceListenAddresses()`를 구현한다(design 결정 4). 같은 테스트 파일에서 bind address 중복, 서로 다른 `--ice-address`에 걸친 announced address 중복, 중복 없는 목록을 검사한다. `ctest -R ice_listen_address`가 통과하는지 확인한다.

## 2. `IceLiteAgent`의 candidate 생성

- [x] 2.1 `IceLiteAgentConfig::addresses`의 타입을 `std::vector<IceListenAddress>`로 바꾼다. `IceLiteAgent::Start()`는 `bind_address`마다 소켓을 연다. `IceLiteAgent::Start()`는 `announced_addresses`가 빈 항목을 받으면 오류를 돌려준다. 기존 `tests/webrtc/ice_lite_agent_test.cpp`의 설정을 새 타입으로 고친다. 기존 시나리오가 모두 통과하는지 확인한다.
- [x] 2.2 생성자가 announced address마다 candidate를 만들도록 바꾼다(design 결정 5). `ice_lite_agent_test.cpp`에 다음 검사를 더한다.
  - bind address `127.0.0.1`, announced address `127.0.0.1`과 `203.0.113.5`인 에이전트의 `Candidates()`가 두 개이고, 두 candidate의 포트가 같고, priority가 2130706431과 2130706175이며, foundation이 서로 다름
  - 이 에이전트의 `127.0.0.1` 소켓으로 보낸 Binding 요청에 성공 응답이 옴
  - `announced_addresses`가 빈 설정으로 `Start()`를 부르면 오류가 옴
  - `ctest -R ice_lite_agent`가 통과함
- [x] 2.3 `ice_lite_agent.h`의 `IceLiteAgentConfig::addresses`와 `IceCandidate`의 주석을 바뀐 뜻에 맞게 고친다. 주석이 "소켓은 bind address마다, candidate는 announced address마다"를 설명하는지 확인한다.

## 3. 명령행과 시작 로그

- [x] 3.1 `main.cpp`의 `ServerOptions::ice_addresses`를 `std::vector<IceListenAddress>`로 바꾼다. `ParseCommandLine()`은 `ParseIceListenAddress()`와 `CheckIceListenAddresses()`를 부른다. `ResolveIceAddresses()`는 자동 조회 결과를 `announced_addresses = {bind_address}`로 바꾼다. 사용법 문자열과 `ParseCommandLine()` 주석의 `--ice-address` 형식을 고친다. `./build/vms_web_server --ice-address 127.0.0.1=` 실행이 오류 메시지, 사용법, 종료 코드 1로 끝나는지 확인한다.
- [x] 3.2 시작 로그를 `IceListenAddress`마다 한 줄로 바꾼다(design 결정 6). `./build/vms_web_server --ice-address 127.0.0.1=127.0.0.1,203.0.113.5` 실행의 로그가 `ICE 대기 중: udp 127.0.0.1:50000, candidate 127.0.0.1, 203.0.113.5`인지 확인한다.
- [x] 3.3 `tests/integration/http_server_test.sh`에 다음 시나리오를 더한다. `ctest -R http_server`가 통과하는지 확인한다.
  - `--ice-address 127.0.0.1=127.0.0.1,203.0.113.5`로 실행한 서버의 answer에 두 host candidate가 이 순서로 있고, 포트가 `--ice-port` 값임
  - `127.0.0.1=`, `127.0.0.1=bad`, `127.0.0.1 --ice-address 127.0.0.1`, `127.0.0.1=203.0.113.5,203.0.113.5`로 실행하면 서버가 0이 아닌 종료 코드로 끝남
  - 기존 `--ice-address 127.0.0.1` 시나리오가 그대로 통과함

## 4. README

- [x] 4.1 실행 인자 표의 `--ice-address` 설명, 시작 로그 예, 자동 조회 설명을 바뀐 형식에 맞게 고친다. README의 예 명령을 그대로 실행해 로그가 문서와 같은지 확인한다.
- [x] 4.2 "WSL2에서 실행하기" 절을 NAT mode와 mirrored mode로 나누어 다시 쓴다. mirrored mode 절에는 `.wslconfig` 설정, `wslinfo --networking-mode` 확인, 같은 PC의 Windows 브라우저를 위한 `--ice-address 127.0.0.1`, 대안인 `hostAddressLoopback`을 적는다. 문서 작성 규칙에 맞는지 다시 읽어 확인한다.
- [x] 4.3 "외부 네트워크에서 접속하기" 절을 새로 쓴다. 다음 내용을 순서대로 적는다. 문서 작성 규칙에 맞는지 다시 읽어 확인한다.
  1. CGNAT 확인 방법
  2. Hyper-V 방화벽 규칙(TCP 8080, UDP 50000)
  3. 공유기의 DHCP 고정 할당과 port forwarding 규칙
  4. `--ice-address <LAN 주소>=<LAN 주소>,<public address> --ice-address 127.0.0.1` 실행 예
  5. 외부 브라우저의 접속 주소 `http://<public address>:8080/`
  6. public address가 바뀌었을 때의 증상과 `curl -4 ifconfig.me` 확인
  7. 접근 제어가 없다는 경고와 데모 때만 port forwarding을 켜는 운영 방법

## 5. 수동 검증

- [x] 5.1 README 4.3의 절차대로 개발 PC를 설정한다. 같은 Wi-Fi의 휴대폰에서 `http://<LAN 주소>:8080/`으로 카메라 영상이 재생되는지 확인한다.
- [x] 5.2 휴대폰의 Wi-Fi를 끄고 LTE로 `http://<public address>:8080/`에 접속한다. 카메라 영상과 녹화가 재생되는지 확인한다. 서버 로그의 `ICE 연결됨` 줄에 휴대폰의 통신사 쪽 주소가 남는지 확인한다.
- [x] 5.3 같은 PC의 Windows Chrome에서 `http://localhost:8080/`으로 카메라 영상이 재생되는지 확인한다. `chrome://webrtc-internals`에서 선택된 candidate pair의 서버 주소가 `127.0.0.1`인지 확인한다.
- [x] 5.4 검증이 끝나면 공유기의 port forwarding 규칙을 끈다. 외부에서 `http://<public address>:8080/`이 열리지 않는지 확인한다.

## Workflow follow-up

- 리뷰가 끝나면 change를 아카이브한다.
- 아카이브 뒤 `openspec/specs/ice-lite/spec.md`와 `openspec/specs/webrtc-signaling/spec.md`에 변경이 반영됐는지 확인한다.
