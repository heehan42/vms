#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "crypto/srtp.h"


namespace vms::test {


// 테스트에서 브라우저 역할로 RTCP 복합 패킷을 SRTCP 패킷으로 보호하는 송신 측.
//
// 키 유도는 서버의 DeriveSrtcpSessionKeys를 쓰지만, IV·E 비트와 인덱스 단어·태그 입력의 조립은 따로
// 작성해 서버 수신 측(SrtcpReceiver)의 조립 실수를 잡는다. 세션 키를 값으로 가지므로 복사·이동할 수 있다.
class SrtcpTestSender {
public:
    // 마스터 키와 솔트로 SRTCP 세션 키를 유도해 송신 측을 만든다.
    //
    // master_key: 받는 쪽과 같은 마스터 키
    // master_salt: 받는 쪽과 같은 마스터 솔트
    // 반환값: 다음 인덱스가 0인 송신 측. 키 유도가 실패하면 오류 메시지
    static std::expected<SrtcpTestSender, std::string> Create(
        std::span<const std::uint8_t, crypto::kSrtpMasterKeySize> master_key,
        std::span<const std::uint8_t, crypto::kSrtpMasterSaltSize> master_salt);


    // 다음 인덱스로 RTCP 복합 패킷을 보호하고 인덱스를 1 올린다.
    //
    // rtcp_compound: 평문 RTCP 복합 패킷. 8바이트 이상이어야 한다.
    // encrypt: true이면 E 비트를 1로 하고 첫 8바이트 뒤를 암호화한다. false이면 인증만 한다.
    // 반환값: SRTCP 패킷. 입력이 8바이트보다 짧거나 OpenSSL이 실패하면 오류 메시지
    std::expected<std::vector<std::uint8_t>, std::string> Protect(std::span<const std::uint8_t> rtcp_compound,
                                                                  bool encrypt);


    // 정한 인덱스로 RTCP 복합 패킷을 보호한다. 송신 측의 다음 인덱스는 바꾸지 않는다.
    //
    // rtcp_compound: 평문 RTCP 복합 패킷. 8바이트 이상이어야 한다.
    // encrypt: true이면 E 비트를 1로 하고 첫 8바이트 뒤를 암호화한다.
    // srtcp_index: 패킷에 실을 31비트 SRTCP 인덱스
    // 반환값: SRTCP 패킷. 입력이 8바이트보다 짧거나 OpenSSL이 실패하면 오류 메시지
    std::expected<std::vector<std::uint8_t>, std::string> Protect(std::span<const std::uint8_t> rtcp_compound,
                                                                  bool encrypt, std::uint32_t srtcp_index) const;


private:
    // 유도한 세션 키로 송신 측을 만든다. Create()에서만 호출한다.
    explicit SrtcpTestSender(const crypto::SrtpSessionKeys& session_keys) : session_keys_(session_keys) {}


    // 받는 쪽과 같은 마스터 키에서 유도한 SRTCP 세션 키
    crypto::SrtpSessionKeys session_keys_;


    // 인덱스를 지정하지 않은 Protect가 쓸 다음 SRTCP 인덱스
    std::uint32_t next_index_ = 0;
};


}  // namespace vms::test
