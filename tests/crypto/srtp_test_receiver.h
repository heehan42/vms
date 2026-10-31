#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "crypto/srtp.h"


namespace vms::test {


// 테스트에서 브라우저 역할로 SRTP 패킷의 인증 태그를 검증하고 복호화하는 수신 측.
//
// 서버의 SrtpSender와 같은 키 유도·키스트림 함수와 ComputeHmacSha1을 쓰지만, 헤더 길이·IV·태그 입력의
// 조립은 따로 작성해 SrtpSender의 조립 실수를 잡는다. ROC는 받은 순번이 직전보다 작아질 때 1 올리며,
// 테스트에서는 손실과 재정렬이 없다고 본다. 세션 키를 값으로 가지므로 복사·이동할 수 있다.
class SrtpTestReceiver {
public:
    // 마스터 키와 솔트로 세션 키를 유도해 수신 측을 만든다.
    //
    // master_key: 보내는 쪽과 같은 마스터 키
    // master_salt: 보내는 쪽과 같은 마스터 솔트
    // 반환값: ROC 0인 수신 측. 키 유도가 실패하면 오류 메시지
    static std::expected<SrtpTestReceiver, std::string> Create(
        std::span<const std::uint8_t, crypto::kSrtpMasterKeySize> master_key,
        std::span<const std::uint8_t, crypto::kSrtpMasterSaltSize> master_salt);


    // 받은 순번으로 ROC를 정해 패킷을 검증하고 복호화한다.
    //
    // srtp_packet: 받은 SRTP 패킷
    // 반환값: 복호화한 RTP 패킷. 형식이 틀리거나 태그가 맞지 않으면 원인 메시지
    //
    // 성공한 패킷만 ROC와 마지막 순번을 갱신한다.
    std::expected<std::vector<std::uint8_t>, std::string> Unprotect(
        std::span<const std::uint8_t> srtp_packet);


    // 정한 ROC로 패킷을 검증하고 복호화한다. 수신 측 상태는 바꾸지 않는다.
    //
    // srtp_packet: 받은 SRTP 패킷
    // rollover_counter: 가정할 ROC
    // 반환값: 복호화한 RTP 패킷. 형식이 틀리거나 태그가 맞지 않으면 원인 메시지
    std::expected<std::vector<std::uint8_t>, std::string> UnprotectWithRolloverCounter(
        std::span<const std::uint8_t> srtp_packet, std::uint32_t rollover_counter) const;


private:
    // 유도한 세션 키로 수신 측을 만든다. Create()에서만 호출한다.
    explicit SrtpTestReceiver(const crypto::SrtpSessionKeys& session_keys)
        : session_keys_(session_keys) {}


    // 보내는 쪽과 같은 마스터 키에서 유도한 세션 키
    crypto::SrtpSessionKeys session_keys_;


    // 지금까지 받은 순번에서 추정한 ROC
    std::uint32_t rollover_counter_ = 0;


    // 마지막으로 검증에 성공한 패킷의 순번. 아직 없으면 비어 있다.
    std::optional<std::uint16_t> last_sequence_number_;
};


}  // namespace vms::test
