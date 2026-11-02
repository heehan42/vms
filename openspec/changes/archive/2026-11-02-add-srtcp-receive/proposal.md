# Proposal

## Why

서버는 브라우저가 보내는 RTCP를 모두 버린다. 브라우저는 수신 보고(RR)로 손실률과 jitter를 알려 주고, 이후 PLI·NACK 같은 피드백도 RTCP로 보낸다. 그런데 서버에는 이를 받을 SRTCP 복호 경로가 없다. 이 change는 SRTCP 수신과 RTCP 해석까지만 만들어, 이후 PLI 응답, SR 송신, NACK 재전송을 각각 작은 change로 올릴 기반을 마련한다.

## What Changes

- 브라우저 쪽(DTLS client) 마스터 키와 솔트로 SRTCP 세션 키를 유도한다. 유도 레이블은 0x03 암호화, 0x04 인증, 0x05 솔트다.
- 받은 SRTCP 패킷을 다음 순서로 처리한다.
  - 끝의 인증 태그(80비트)를 검증한다.
  - SRTCP 인덱스로 재생 공격 방지 창(64개)을 확인한다.
  - 암호화 표시(E 비트)가 있으면 페이로드를 복호화한다.
- 복호화한 RTCP 복합 패킷을 개별 RTCP 패킷으로 나누어 해석한다. 이번 change에서는 RR의 보고 블록과 SDES의 CNAME만 읽고, 나머지는 패킷 종류와 길이만 확인한다.
- UDP로 받은 패킷 중 첫 바이트가 128~191이고 둘째 바이트가 192~223인 것을 RTCP로 구분한다. DTLS가 완료된 세션의 nominated pair에서 온 RTCP만 SRTCP로 처리하고, 나머지 RTP·RTCP는 지금처럼 버린다.
- 수신 결과는 로그로만 남긴다. 세션마다 첫 RTCP 복합 패킷은 자세히 남기고, 세션을 제거할 때 종류별 수신 개수와 마지막 RR 값을 요약해 남긴다.
- 인증 실패, 재생 거부, 형식 오류는 패킷을 버리고 세션별로 개수를 센다. 세션마다 처음 3개만 SSRC, SRTCP 인덱스, 길이, 원인을 자세히 남기고, 세션을 제거할 때 개수를 요약해 남긴다. 로그는 기존 서버 로그(표준 오류)에만 남기고 별도 파일은 만들지 않는다.
- README의 확인 절차에 RTCP 수신 로그를 추가한다.

범위 밖(이후 change):

- answer의 `a=rtcp-fb` 협상과 PLI 응답(키프레임 재송신)
- SRTCP 송신과 SR(Sender Report)
- NACK 협상과 재전송 버퍼
- RTCP 수신 통계를 HTTP 등으로 외부에 노출하는 것

## Capabilities

### New Capabilities

- `rtcp`: 브라우저가 보낸 RTCP를 받아 해석하고, 그 결과와 수신 실패를 기록하는 동작. 이후 PLI 응답, SR 송신, NACK 처리가 이 capability에 추가된다.

### Modified Capabilities

- `srtp`: 송신 RTP 보호에 더해, 브라우저 쪽 키로 SRTCP 세션 키를 유도하고 SRTCP 패킷을 검증·복호화하며 재생된 패킷을 거부하는 요구사항을 추가한다.
- `ice-lite`: "잘못된 요청 무시"에서 RTP를 일괄로 버리던 규칙을 바꾼다. DTLS가 완료된 세션의 nominated pair에서 온 RTCP는 RTCP 처리로 넘긴다.

## Impact

- 바뀌는 코드
  - `src/crypto/srtp.h/.cpp`: SRTCP 세션 키 유도, 수신 측 SRTCP 검증·복호화와 재생 방지
  - `src/webrtc/ice_lite_agent.h/.cpp`: RTP/RTCP 구분, 세션별 SRTCP 수신 상태와 기록, 세션 제거 시 요약 로그
  - `README.md`의 확인 절차
- 새 코드
  - `src/webrtc/rtcp_packet.h/.cpp`: RTCP 복합 패킷 분리와 RR·SDES 해석
  - 대응하는 `tests/` 파일, 브라우저 역할로 SRTCP 패킷을 만드는 테스트 전용 송신 측
- SDP answer, 시그널링 HTTP API, 명령행 인자는 바뀌지 않는다.
- 새 외부 의존성은 없다. OpenSSL의 AES-CTR과 기존 HMAC-SHA1을 그대로 쓴다.
