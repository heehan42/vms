#pragma once

#include <netinet/in.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>


namespace vms::webrtc {


// STUN 헤더의 고정값. 이 값이 없으면 STUN 메시지가 아니며, XOR-MAPPED-ADDRESS 인코딩 키로도 쓴다.
inline constexpr std::uint32_t kStunMagicCookie = 0x2112A442;


// STUN 헤더 크기(바이트)
inline constexpr std::size_t kStunHeaderSize = 20;


// 요청과 응답을 짝짓는 96비트 트랜잭션 ID
using StunTransactionId = std::array<std::uint8_t, 12>;


// 서버가 다루는 STUN 메시지 타입(메서드와 클래스를 합친 값).
enum class StunMessageType : std::uint16_t {
    // Binding 요청. ICE 연결 확인에 쓴다.
    kBindingRequest = 0x0001,

    // Binding 성공 응답
    kBindingSuccessResponse = 0x0101,
};


// STUN 메시지 해석·검증 실패 원인. 로그에 원인을 남기는 데 쓴다.
enum class StunError {
    // 헤더 형식이 STUN이 아님(크기, 앞 2비트, magic cookie, 길이 필드)
    kNotStunMessage,

    // 속성이 메시지 끝을 넘거나 알려진 속성의 길이가 규격과 다름
    kMalformedAttribute,

    // FINGERPRINT 속성이 없음
    kMissingFingerprint,

    // FINGERPRINT 값이 계산값과 다름
    kFingerprintMismatch,

    // MESSAGE-INTEGRITY 속성이 없음
    kMissingMessageIntegrity,

    // MESSAGE-INTEGRITY 값이 계산값과 다름(비밀번호가 틀림)
    kMessageIntegrityMismatch,

    // HMAC 계산 자체가 실패함
    kMessageIntegrityComputationFailed,
};


// 실패 원인을 로그용 문구로 바꾼다.
//
// error: 실패 원인
// 반환값: 정적 문자열을 가리키는 설명 문구
std::string_view StunErrorDescription(StunError error);


// 해석한 STUN 메시지 중 서버가 쓰는 부분.
//
// username과 packet은 해석한 원본 패킷을 가리키므로, 원본 버퍼가 살아 있는 동안에만 유효하다.
struct StunMessage {
    // 메시지 타입. StunMessageType에 없는 값일 수도 있다.
    std::uint16_t type = 0;


    // 트랜잭션 ID
    StunTransactionId transaction_id{};


    // USERNAME 속성 값. 없으면 빈 optional
    std::optional<std::string_view> username;


    // USE-CANDIDATE 속성이 있었는지 여부
    bool has_use_candidate = false;


    // XOR-MAPPED-ADDRESS 속성을 해독한 IPv4 주소와 포트. 없거나 IPv6이면 빈 optional
    std::optional<sockaddr_in> xor_mapped_address;


    // MESSAGE-INTEGRITY 속성이 시작하는 위치(패킷 앞에서부터의 바이트 수). 없으면 빈 optional
    std::optional<std::size_t> message_integrity_offset;


    // FINGERPRINT 속성이 시작하는 위치. 없으면 빈 optional. 있으면 항상 마지막 속성이다.
    std::optional<std::size_t> fingerprint_offset;


    // 해석한 원본 패킷 전체
    std::span<const std::uint8_t> packet;
};


// CRC-32(IEEE 802.3, 다항식 0xEDB88320)를 계산한다.
//
// data: 계산할 바이트
// 반환값: CRC-32 값
std::uint32_t ComputeCrc32(std::span<const std::uint8_t> data);


// UDP 패킷을 STUN 메시지로 해석한다. 무결성 검증은 하지 않는다.
//
// packet: 받은 UDP 데이터그램 전체. 반환값이 쓰이는 동안 살아 있어야 한다.
// 반환값: 해석한 메시지. 헤더 형식이 틀리면 kNotStunMessage, 속성 구조가 틀리면 kMalformedAttribute
//
// MESSAGE-INTEGRITY 뒤에는 FINGERPRINT만 의미가 있으므로 그 밖의 속성은 무시하며(RFC 8489),
// FINGERPRINT 뒤에 속성이 더 있으면 kMalformedAttribute로 거부한다.
std::expected<StunMessage, StunError> ParseStunMessage(std::span<const std::uint8_t> packet);


// FINGERPRINT 속성이 있고 값이 CRC-32 XOR 0x5354554E와 같은지 검증한다.
//
// message: ParseStunMessage의 결과
// 반환값: 성공하면 빈 값, 아니면 kMissingFingerprint 또는 kFingerprintMismatch
std::expected<void, StunError> VerifyFingerprint(const StunMessage& message);


// MESSAGE-INTEGRITY 속성이 있고 password로 계산한 HMAC-SHA1과 같은지 검증한다.
//
// message: ParseStunMessage의 결과
// password: 키로 쓸 ICE 비밀번호. 요청을 받는 쪽의 ice-pwd다.
// 반환값: 성공하면 빈 값, 아니면 실패 원인
//
// 헤더의 길이 필드를 MESSAGE-INTEGRITY 속성 끝까지로 고친 메시지로 계산하므로,
// 뒤에 FINGERPRINT가 붙어 있어도 검증할 수 있다. 비교는 상수 시간으로 한다.
std::expected<void, StunError> VerifyMessageIntegrity(const StunMessage& message,
                                                      std::string_view password);


// Binding 성공 응답을 만든다.
//
// transaction_id: 응답할 요청의 트랜잭션 ID
// mapped_address: 요청을 보낸 쪽의 주소와 포트. XOR-MAPPED-ADDRESS에 담는다.
// password: MESSAGE-INTEGRITY 키. 요청을 검증한 것과 같은 서버 ice-pwd다.
// 반환값: XOR-MAPPED-ADDRESS, MESSAGE-INTEGRITY, FINGERPRINT 순서로 담은 응답 바이트.
//   HMAC 계산이 실패하면 kMessageIntegrityComputationFailed
std::expected<std::vector<std::uint8_t>, StunError> BuildBindingSuccessResponse(
    const StunTransactionId& transaction_id, const sockaddr_in& mapped_address,
    std::string_view password);


// Binding 요청을 만든다. 서버는 요청을 보내지 않으며, 테스트에서 브라우저 역할을 할 때 쓴다.
//
// transaction_id: 요청의 트랜잭션 ID
// username: USERNAME 값. 예: "서버ufrag:브라우저ufrag"
// password: MESSAGE-INTEGRITY 키(요청을 받는 쪽의 ice-pwd)
// use_candidate: USE-CANDIDATE 속성을 넣을지 여부
// 반환값: USERNAME, (USE-CANDIDATE), MESSAGE-INTEGRITY, FINGERPRINT를 담은 요청 바이트.
//   HMAC 계산이 실패하면 kMessageIntegrityComputationFailed
std::expected<std::vector<std::uint8_t>, StunError> BuildBindingRequest(
    const StunTransactionId& transaction_id, std::string_view username, std::string_view password,
    bool use_candidate);


}  // namespace vms::webrtc
