# dtls-transport Specification

## Purpose

브라우저가 고른 nominated pair 위에서 서버가 DTLS 서버로 핸드셰이크한다. 이 과정에서 서로의 인증서를 확인하고, 이후 미디어 암호화에 쓸 SRTP 프로파일과 키 재료를 정한다.

## Requirements

### Requirement: DTLS 핸드셰이크
서버는 연결된 ICE 세션의 nominated pair의 브라우저 주소에서 온 DTLS 패킷을 그 세션의 DTLS 서버 역할로 처리해 핸드셰이크를 완료해야 한다(SHALL). 핸드셰이크 응답은 그 pair의 서버 주소에서 브라우저 주소로 보낸다. 서버는 DTLS 1.2를 사용하고 answer에 알린 인증서로 자신을 인증하며, 핸드셰이크가 완료되면 세션과 협상 결과를 로그에 한 번 남긴다.

#### Scenario: Chrome과의 핸드셰이크
- **WHEN** ICE가 연결된 Chrome이 ClientHello를 보낸다
- **THEN** 서버는 핸드셰이크를 완료하고, Chrome의 `connectionState`가 `connected`가 된다

#### Scenario: 서버 인증서 확인
- **WHEN** 테스트 DTLS 클라이언트가 서버와 핸드셰이크를 마친다
- **THEN** 클라이언트가 받은 서버 인증서의 SHA-256 fingerprint는 answer의 `a=fingerprint` 값과 같다

#### Scenario: nominated pair가 아닌 주소의 DTLS 패킷
- **WHEN** 어떤 세션의 nominated pair에도 속하지 않는 주소에서 DTLS 패킷이 도착한다
- **THEN** 서버는 응답하지 않고 패킷을 버린다

### Requirement: 핸드셰이크 재전송
서버는 자신이 보낸 핸드셰이크 메시지에 대한 브라우저의 다음 메시지가 제한 시간 안에 오지 않으면 그 메시지를 다시 보내야 한다(SHALL). 제한 시간은 1초에서 시작해 재전송할 때마다 두 배로 늘리며, 핸드셰이크가 끝나면 재전송을 멈춘다. 브라우저가 이미 받은 메시지를 다시 보내면 서버는 마지막으로 보낸 메시지를 다시 보낸다.

#### Scenario: 서버 응답 유실
- **WHEN** 테스트 클라이언트가 서버의 첫 응답 묶음(ServerHello부터 ServerHelloDone까지)을 받지 않은 것으로 처리하고 아무것도 보내지 않는다
- **THEN** 서버는 약 1초 뒤 같은 핸드셰이크 메시지를 다시 보내고, 클라이언트는 다시 받은 메시지로 핸드셰이크를 완료한다

### Requirement: 브라우저 인증서 확인
서버는 핸드셰이크에서 브라우저 인증서를 요구하고, 그 인증서의 SHA-256 fingerprint가 해당 세션 offer의 `a=fingerprint:sha-256` 값과 같은지 확인해야 한다(SHALL). 비교는 16진수 대소문자를 구분하지 않는다. 인증서가 없거나 fingerprint가 다르면 핸드셰이크를 실패시키고 원인을 로그에 남긴다. 브라우저 인증서는 자체 서명이므로 인증 기관 검증은 하지 않는다.

#### Scenario: fingerprint 일치
- **WHEN** offer에 적힌 fingerprint와 같은 인증서를 가진 클라이언트가 핸드셰이크한다
- **THEN** 핸드셰이크가 완료된다

#### Scenario: fingerprint 불일치
- **WHEN** offer에 적힌 fingerprint와 다른 인증서를 가진 클라이언트가 핸드셰이크한다
- **THEN** 서버는 핸드셰이크를 실패시키고, 클라이언트의 핸드셰이크는 오류로 끝나며, 서버 로그에 fingerprint 불일치가 남는다

### Requirement: SRTP 프로파일 협상과 키 재료
서버는 핸드셰이크에서 `use_srtp` 확장으로 SRTP 프로파일 `SRTP_AES128_CM_SHA1_80`을 협상해야 한다(SHALL). 핸드셰이크가 끝나면 RFC 5764 방식으로 브라우저용·서버용 SRTP 마스터 키와 솔트를 꺼내 세션에 보관한다. 브라우저가 이 프로파일을 제안하지 않아 협상하지 못하면 핸드셰이크를 실패로 처리한다. 로그에는 프로파일 이름만 남기고 키 값은 남기지 않는다.

#### Scenario: 협상 결과
- **WHEN** `SRTP_AES128_CM_SHA1_80`을 제안한 클라이언트가 핸드셰이크를 마친다
- **THEN** 양쪽이 선택한 프로파일은 `SRTP_AES128_CM_SHA1_80`이고, 서버가 보관한 키 재료는 클라이언트가 같은 방식으로 꺼낸 값과 같다

#### Scenario: Chrome에서의 확인
- **WHEN** Chrome과 핸드셰이크가 끝난 뒤 `chrome://webrtc-internals`의 transport 통계를 본다
- **THEN** `dtlsState`는 `connected`, `dtlsRole`은 `client`, `srtpCipher`는 `SRTP_AES128_CM_HMAC_SHA1_80`이다

#### Scenario: SRTP 미제안
- **WHEN** `use_srtp` 확장 없이 ClientHello를 보낸 클라이언트가 핸드셰이크한다
- **THEN** 서버는 핸드셰이크를 실패로 처리하고 원인을 로그에 남긴다

### Requirement: 핸드셰이크 이후 패킷과 종료
서버는 핸드셰이크가 끝난 뒤 받은 DTLS 레코드도 계속 처리해야 한다(SHALL). 브라우저가 보낸 종료 알림(close_notify)이나 치명적 경고를 받으면 그 세션의 DTLS를 닫힘 또는 실패로 기록하고 로그를 남긴다. 핸드셰이크가 실패했거나 DTLS가 닫힌 세션에 이후 도착한 DTLS 패킷은 버린다. 응용 데이터 레코드는 아직 쓰지 않으므로 버린다. 서버는 이런 패킷 때문에 종료되거나 다른 세션의 처리를 멈추지 않는다.

#### Scenario: 브라우저의 연결 종료
- **WHEN** 핸드셰이크를 마친 클라이언트가 close_notify를 보낸다
- **THEN** 서버는 그 세션의 DTLS 종료를 로그에 남기고, 이후 같은 주소의 DTLS 패킷을 버린다

#### Scenario: 실패한 세션과 다른 세션
- **WHEN** 한 세션에서 offer에 적힌 fingerprint와 다른 인증서로 핸드셰이크해 실패한 뒤, 다른 세션에서 offer에 적힌 fingerprint와 같은 인증서로 핸드셰이크한다
- **THEN** 두 번째 세션의 핸드셰이크는 정상적으로 완료된다
