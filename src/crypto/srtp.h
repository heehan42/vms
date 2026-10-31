#pragma once

#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>


namespace vms::crypto {


// SRTP_AES128_CM_SHA1_80 마스터 키 크기(바이트)
inline constexpr std::size_t kSrtpMasterKeySize = 16;


// SRTP_AES128_CM_SHA1_80 마스터 솔트 크기(바이트)
inline constexpr std::size_t kSrtpMasterSaltSize = 14;


// 세션 인증 키 크기(바이트). HMAC-SHA1 키로 쓴다.
inline constexpr std::size_t kSrtpAuthenticationKeySize = 20;


// 패킷 끝에 붙는 인증 태그 크기(바이트). HMAC-SHA1 결과의 앞 80비트다.
inline constexpr std::size_t kSrtpAuthenticationTagSize = 10;


// AES-CM 키스트림의 IV 크기(바이트)
inline constexpr std::size_t kAesCmIvSize = 16;


// 마스터 키·솔트에서 유도한 SRTP 세션 키(RFC 3711 4.3).
struct SrtpSessionKeys {
    // 페이로드를 암호화하는 AES-128 키(레이블 0x00)
    std::array<std::uint8_t, kSrtpMasterKeySize> encryption_key{};


    // 인증 태그를 만드는 HMAC-SHA1 키(레이블 0x01)
    std::array<std::uint8_t, kSrtpAuthenticationKeySize> authentication_key{};


    // 패킷 IV를 만드는 세션 솔트(레이블 0x02)
    std::array<std::uint8_t, kSrtpMasterSaltSize> salt{};
};


// RFC 3711의 AES-CM 키 유도 함수로 세션 키를 만든다.
//
// master_key: DTLS-SRTP로 정한 마스터 키
// master_salt: DTLS-SRTP로 정한 마스터 솔트
// 반환값: 세션 키. OpenSSL이 실패하면 오류 메시지
//
// 키 유도 주기(key derivation rate)는 0이므로 패킷 인덱스와 무관하게 한 번만 유도한다.
// 반환값은 비밀 값이므로 호출자가 다 쓴 뒤 OPENSSL_cleanse로 지워야 한다.
std::expected<SrtpSessionKeys, std::string> DeriveSrtpSessionKeys(
    std::span<const std::uint8_t, kSrtpMasterKeySize> master_key,
    std::span<const std::uint8_t, kSrtpMasterSaltSize> master_salt);


// AES-128 Counter Mode 키스트림을 data에 XOR한다. 암호화와 복호화가 같은 연산이다.
//
// key: AES-128 키
// iv: 첫 블록의 카운터 값. 하위 16비트를 블록 카운터로 쓴다.
// data: 제자리에서 바뀌는 데이터. 0으로 채운 버퍼를 넘기면 키스트림 자체를 얻는다.
// 반환값: 성공하면 빈 값, OpenSSL이 실패하면 오류 메시지
//
// 호출마다 OpenSSL 컨텍스트를 새로 만든다. 키 유도와 테스트처럼 드물게 부르는 곳에 쓴다.
std::expected<void, std::string> XorAesCmKeystream(std::span<const std::uint8_t, kSrtpMasterKeySize> key,
                                                   std::span<const std::uint8_t, kAesCmIvSize> iv,
                                                   std::span<std::uint8_t> data);


// SSRC 하나가 보내는 RTP 패킷을 SRTP_AES128_CM_SHA1_80으로 보호하는 송신 측 상태.
//
// 세션 키, 재사용하는 AES 컨텍스트, 송신 ROC(rollover counter)를 소유한다. 보호하는 패킷의 순번은
// 항상 1씩 증가한다고 보고, 순번이 직전보다 작아지면 65535에서 0으로 넘어간 것으로 보고 ROC를 올린다.
// 세션 키는 소멸할 때 지운다. OpenSSL 컨텍스트를 가지므로 std::unique_ptr로만 다룬다.
class SrtpSender {
public:
    // 마스터 키와 솔트로 세션 키를 유도하고 보호에 쓸 AES 컨텍스트를 만든다.
    //
    // master_key: 서버가 보내는 패킷용 마스터 키. 복사해 쓰지 않고 유도에만 쓴다.
    // master_salt: 서버가 보내는 패킷용 마스터 솔트
    // 반환값: ROC 0인 송신 상태. 키 유도나 컨텍스트 생성이 실패하면 오류 메시지
    static std::expected<std::unique_ptr<SrtpSender>, std::string> Create(
        std::span<const std::uint8_t, kSrtpMasterKeySize> master_key,
        std::span<const std::uint8_t, kSrtpMasterSaltSize> master_salt);


    // 세션 키를 지운다.
    ~SrtpSender();


    // 세션 키를 한 곳에만 두기 위해 복사를 금지한다.
    SrtpSender(const SrtpSender&) = delete;


    // 세션 키를 한 곳에만 두기 위해 복사 대입을 금지한다.
    SrtpSender& operator=(const SrtpSender&) = delete;


    // RTP 패킷 하나를 SRTP 패킷으로 만든다.
    //
    // rtp_packet: 평문 RTP 패킷(헤더와 페이로드). 순번이 직전 호출보다 1 큰 패킷이어야 한다.
    // 반환값: 헤더는 그대로, 페이로드는 암호화하고 끝에 10바이트 인증 태그를 붙인 패킷.
    //   RTP 헤더 형식이 아니거나 OpenSSL이 실패하면 오류 메시지
    //
    // 성공하면 순번에 따라 송신 ROC가 바뀐다. 실패하면 상태를 바꾸지 않는다.
    std::expected<std::vector<std::uint8_t>, std::string> Protect(
        std::span<const std::uint8_t> rtp_packet);


private:
    // EVP_CIPHER_CTX를 해제하는 삭제자.
    struct CipherContextDeleter {
        // 컨텍스트를 해제한다.
        void operator()(EVP_CIPHER_CTX* context) const { ::EVP_CIPHER_CTX_free(context); }
    };


    // 유도한 세션 키와 컨텍스트로 객체를 만든다. Create()에서만 호출한다.
    SrtpSender(const SrtpSessionKeys& session_keys,
               std::unique_ptr<EVP_CIPHER_CTX, CipherContextDeleter> cipher_context);


    // 이 송신 상태의 세션 키
    SrtpSessionKeys session_keys_;


    // 패킷마다 IV만 바꿔 다시 쓰는 AES-128 CTR 컨텍스트
    std::unique_ptr<EVP_CIPHER_CTX, CipherContextDeleter> cipher_context_;


    // 송신 ROC. 순번이 65535에서 0으로 넘어갈 때마다 1씩 오른다.
    std::uint32_t rollover_counter_ = 0;


    // 마지막으로 보호한 패킷의 순번. 아직 보호한 패킷이 없으면 비어 있다.
    std::optional<std::uint16_t> last_sequence_number_;
};


}  // namespace vms::crypto
