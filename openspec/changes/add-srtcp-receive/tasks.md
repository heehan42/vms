# Tasks

## 1. SRTCP 세션 키

- [ ] 1.1 `src/crypto/srtp.cpp`의 `KeyDerivationLabel`에 0x03~0x05를 더하고, 레이블 세 개로 `SrtpSessionKeys`를 채우는 내부 함수로 기존 유도를 묶는다. 그 위에 `DeriveSrtcpSessionKeys`를 공개한다(design 결정 2). `tests/crypto/srtp_test.cpp`에서 기존 RFC 3711 B.3 벡터가 그대로 통과하는지, SRTCP 세 키가 `XorAesCmKeystream`에 레이블 0x03~0x05로 직접 만든 IV를 넣은 결과와 같고 SRTP 키와 다른지 확인한다(`ctest -R srtp` 통과)

## 2. SRTCP 수신 측

- [ ] 2.1 `tests/crypto`에 브라우저 역할의 `SrtcpTestSender`(`Protect(rtcp_compound, encrypt)`: 인덱스 0부터 1씩 증가, 인덱스 지정 오버로드)를 `srtp_test_receiver` 라이브러리에 추가한다. IV, 인덱스 단어, 태그 입력은 서버 코드와 따로 조립한다
- [ ] 2.2 `src/crypto/srtp.h/.cpp`에 `SrtcpUnprotectError`와 `SrtcpReceiver`(`Create`, `Unprotect`, 소멸 시 키 삭제, SSRC별 64개 재생 창 최대 8개)를 추가한다(design 결정 2·3·4). `srtp_test`에서 다음을 확인한다(`ctest -R srtp` 통과).
  - E=1 왕복 복원, E=0 인증만 통과
  - 암호문·인덱스 1비트 변조 시 `kAuthenticationFailed`, 21바이트 패킷은 `kTooShort`
  - 같은 패킷 두 번째는 `kReplayed`, 인덱스 10 뒤 9는 통과, 100 뒤 30은 `kReplayed`
  - 태그가 틀린 인덱스 5 뒤 올바른 인덱스 5는 통과
  - 9번째 SSRC는 `kTooManySsrcs`
  - 오류에 SSRC와 인덱스가 담기는지

## 3. RTCP 해석

- [ ] 3.1 `src/webrtc/rtcp_packet.h/.cpp`에 `RtcpReportBlock`, `RtcpPacket`, `ParseRtcpCompoundPacket`을 추가하고 `CMakeLists.txt`의 `server_core`에 넣는다(design 결정 5). `tests/webrtc/rtcp_packet_test.cpp`에서 다음을 확인한다(`ctest -R rtcp_packet` 통과).
  - background의 RR 예시 바이트를 해석한 다섯 값과 LSR·DLSR
  - RR+SDES 복합 패킷의 CNAME
  - SR의 보고 블록, PT 206 패킷의 종류와 길이
  - 형식 오류: 남은 길이를 넘는 length, 버전 1, 4바이트 미만 남은 데이터, padding 길이 0 또는 본문 초과
  - 누적 손실 음수(`FF FF FF`)는 -1

## 4. 에이전트 연결

- [ ] 4.1 `ice_lite_agent.cpp`의 `PacketKind`에 `kRtcp`를 더하고 `ClassifyPacket`이 둘째 바이트(192~223)를 보게 바꾼다. `Session`에 `srtcp_receiver`와 `rtcp_stats`를 추가하고, DTLS kConnected 전이에서 `remote_key/remote_salt`로 수신 측을 만들고(실패 시 로그), kFailed·kClosed에서 지운다(design 결정 6)
- [ ] 4.2 `HandleRtcpPacket`을 추가해 nominated pair와 수신 측이 있는 세션만 처리하고, 나머지는 `LogDroppedPacket("RTCP", ...)`로 버린다. 복호화와 해석 결과로 통계를 갱신하고, 첫 복합 패킷 상세 로그와 실패별 처음 3개 상세 로그를 남긴다. CNAME은 출력할 수 없는 바이트를 `?`로 바꾸고 64바이트로 자른다. `RemoveSession`에서 수신 요약을 남기고, `FindRtcpReceiveStats`를 공개한다
- [ ] 4.3 `tests/webrtc/ice_lite_agent_test.cpp`에서 `DtlsTestClient`로 핸드셰이크한 뒤 client 키로 보호한 RTCP를 보내 다음을 확인한다(`ctest -R ice_lite_agent` 통과).
  - RR+SDES 하나는 복합 패킷 수 1, RR 1, SDES 1, 마지막 보고 블록 값 일치
  - 서버 쪽(local) 키로 보호한 패킷 5개는 인증 실패 5, 그 뒤 올바른 패킷은 정상 집계
  - 같은 패킷 재전송은 재생 1
  - DTLS 완료 전과 nominated pair가 아닌 주소에서 온 RTCP는 통계가 바뀌지 않음
  - 둘째 바이트가 102인 RTP 패킷은 버림
  - 동영상 송신 세션에서 실패 패킷 뒤에도 RTP 수신이 계속됨

## 5. 문서와 실제 브라우저 확인

- [ ] 5.1 README의 확인 절차에 "몇 초 뒤 서버 로그에 `RTCP 수신 시작: ...`이 남고 대상 SSRC가 answer의 SSRC와 같다", "탭을 닫아 세션이 제거되면 `RTCP 수신 요약: ...`에서 인증 실패가 0이다"를 추가한다
- [ ] 5.2 Chrome으로 동영상을 재생해 README에 적은 대로 로그를 확인한다. `RTCP 버림`이 없고, 요약의 인증 실패·재생·형식 오류가 모두 0이며, 영상 재생이 이전과 같은지 확인한다(`srtp` "브라우저가 보낸 패킷", `rtcp` "브라우저의 수신 보고" 시나리오)
