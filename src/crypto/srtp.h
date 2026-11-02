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
#include <string_view>
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


// 마스터 키·솔트에서 유도한 SRTP 또는 SRTCP 세션 키(RFC 3711 4.3).
//
// 레이블은 SRTP가 0x00~0x02, SRTCP가 0x03~0x05이며, 키의 크기와 쓰임은 같다.
struct SrtpSessionKeys {
    // 페이로드를 암호화하는 AES-128 키(SRTP 레이블 0x00, SRTCP 레이블 0x03)
    std::array<std::uint8_t, kSrtpMasterKeySize> encryption_key{};


    // 인증 태그를 만드는 HMAC-SHA1 키(SRTP 레이블 0x01, SRTCP 레이블 0x04)
    std::array<std::uint8_t, kSrtpAuthenticationKeySize> authentication_key{};


    // 패킷 IV를 만드는 세션 솔트(SRTP 레이블 0x02, SRTCP 레이블 0x05)
    std::array<std::uint8_t, kSrtpMasterSaltSize> salt{};
};


// RFC 3711의 AES-CM 키 유도 함수로 SRTP(RTP용) 세션 키를 만든다(레이블 0x00~0x02).
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


// RFC 3711의 AES-CM 키 유도 함수로 SRTCP(RTCP용) 세션 키를 만든다(레이블 0x03~0x05).
//
// master_key: DTLS-SRTP로 정한 마스터 키. 수신에는 브라우저 쪽(DTLS client) 키를 넘긴다.
// master_salt: master_key와 같은 쪽의 마스터 솔트
// 반환값: 세션 키. OpenSSL이 실패하면 오류 메시지
//
// 키 유도 주기는 0이다. 반환값은 비밀 값이므로 호출자가 다 쓴 뒤 OPENSSL_cleanse로 지워야 한다.
std::expected<SrtpSessionKeys, std::string> DeriveSrtcpSessionKeys(
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


// EVP_CIPHER_CTX를 해제하는 삭제자. SRTP 송신 측과 SRTCP 수신 측이 AES 컨텍스트를 소유할 때 쓴다.
struct CipherContextDeleter {
    // 컨텍스트를 해제한다.
    void operator()(EVP_CIPHER_CTX* context) const { ::EVP_CIPHER_CTX_free(context); }
};


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


// SRTCP 패킷을 받아들이지 않은 원인
enum class SrtcpUnprotectFailure {
    // RTCP 헤더 8바이트, 인덱스 4바이트, 태그 10바이트의 합보다 짧음
    kTooShort,

    // 인증 태그가 맞지 않음. 다른 키로 보호했거나 전송 중 바뀐 패킷이다.
    kAuthenticationFailed,

    // 이미 받았거나 재생 방지 창보다 오래된 SRTCP 인덱스
    kReplayed,

    // 재생 방지 창을 둘 SSRC 수가 상한에 이르러 새 SSRC를 받아들일 수 없음
    kTooManySsrcs,

    // OpenSSL의 HMAC이나 복호화가 실패함
    kCryptoFailure,
};


// SRTCP 패킷을 받아들이지 않은 원인과 로그에 남길 패킷 정보.
struct SrtcpUnprotectError {
    // 거부 원인
    SrtcpUnprotectFailure failure = SrtcpUnprotectFailure::kTooShort;


    // 패킷 헤더의 보낸 쪽 SSRC. 패킷이 8바이트보다 짧으면 비어 있다.
    std::optional<std::uint32_t> ssrc;


    // 패킷 꼬리의 31비트 SRTCP 인덱스. 길이 부족으로 꼬리를 정할 수 없으면 비어 있다.
    std::optional<std::uint32_t> srtcp_index;


    // kCryptoFailure일 때 OpenSSL 오류 메시지. 그 밖의 원인에서는 비어 있다.
    std::string detail;
};


// 거부 원인을 로그용 문구로 바꾼다.
//
// failure: 거부 원인
// 반환값: "인증 실패" 같은 정적 문자열
std::string_view SrtcpUnprotectFailureName(SrtcpUnprotectFailure failure);


// 상대가 보낸 SRTCP 패킷을 SRTP_AES128_CM_SHA1_80으로 검증·복호화하는 수신 측 상태(RFC 3711 3.4).
//
// SRTCP 세션 키, 재사용하는 AES 컨텍스트, SSRC별 재생 방지 창(최대 kMaxSsrcs개)을 소유한다.
// 재생 방지 창은 인증과 복호화를 모두 통과한 패킷으로만 만들고 갱신하므로, 키가 없는 상대가 창을
// 채우거나 앞으로 밀 수 없다. 세션 키는 소멸할 때 지운다. OpenSSL 컨텍스트를 가지므로
// std::unique_ptr로만 다룬다.
class SrtcpReceiver {
public:
    // 재생 방지 창을 두는 SSRC의 최대 수. 브라우저는 RTCP를 SSRC 한두 개로 보낸다.
    static constexpr std::size_t kMaxSsrcs = 8;


    // SSRC마다 가장 큰 인덱스 아래로 기억하는 인덱스 수
    static constexpr std::uint32_t kReplayWindowSize = 64;


    // 마스터 키와 솔트로 SRTCP 세션 키를 유도하고 복호화에 쓸 AES 컨텍스트를 만든다.
    //
    // master_key: 상대가 보내는 패킷용 마스터 키(서버에서는 DTLS client 쪽 키). 유도에만 쓴다.
    // master_salt: master_key와 같은 쪽의 마스터 솔트
    // 반환값: 재생 방지 창이 빈 수신 상태. 키 유도나 컨텍스트 생성이 실패하면 오류 메시지
    static std::expected<std::unique_ptr<SrtcpReceiver>, std::string> Create(
        std::span<const std::uint8_t, kSrtpMasterKeySize> master_key,
        std::span<const std::uint8_t, kSrtpMasterSaltSize> master_salt);


    // 세션 키를 지운다.
    ~SrtcpReceiver();


    // 세션 키를 한 곳에만 두기 위해 복사를 금지한다.
    SrtcpReceiver(const SrtcpReceiver&) = delete;


    // 세션 키를 한 곳에만 두기 위해 복사 대입을 금지한다.
    SrtcpReceiver& operator=(const SrtcpReceiver&) = delete;


    // SRTCP 패킷 하나를 검증하고, E 비트가 1이면 복호화한다.
    //
    // srtcp_packet: 받은 SRTCP 패킷
    // 반환값: 인덱스 단어와 인증 태그를 뗀 평문 RTCP 복합 패킷. 받아들이지 않으면 원인과 패킷 정보
    //
    // 길이 확인, 재생 확인(상태 변경 없음), 태그 비교, 복호화 순서로 처리한다. 모두 통과한 경우에만
    // 그 SSRC의 재생 방지 창을 만들거나 갱신하며, 실패하면 상태를 바꾸지 않는다.
    std::expected<std::vector<std::uint8_t>, SrtcpUnprotectError> Unprotect(
        std::span<const std::uint8_t> srtcp_packet);


private:
    // SSRC 하나의 재생 방지 창(RFC 3711 3.3.2).
    struct ReplayWindow {
        // 이 창이 맡은 보낸 쪽 SSRC
        std::uint32_t ssrc = 0;


        // 지금까지 받아들인 가장 큰 SRTCP 인덱스
        std::uint32_t highest_index = 0;


        // 비트 k가 1이면 인덱스 highest_index - k를 받았다. 비트 0은 highest_index 자신이다.
        std::uint64_t received_bitmap = 0;
    };


    // 유도한 세션 키와 컨텍스트로 객체를 만든다. Create()에서만 호출한다.
    SrtcpReceiver(const SrtpSessionKeys& session_keys,
                  std::unique_ptr<EVP_CIPHER_CTX, CipherContextDeleter> cipher_context);


    // SSRC의 재생 방지 창을 찾는다.
    //
    // ssrc: 보낸 쪽 SSRC
    // 반환값: 창을 가리키는 포인터. 아직 창이 없으면 nullptr. 다음 창 추가 전까지 유효하다.
    ReplayWindow* FindReplayWindow(std::uint32_t ssrc);


    // 이 수신 상태의 SRTCP 세션 키
    SrtpSessionKeys session_keys_;


    // 패킷마다 IV만 바꿔 다시 쓰는 AES-128 CTR 컨텍스트
    std::unique_ptr<EVP_CIPHER_CTX, CipherContextDeleter> cipher_context_;


    // SSRC별 재생 방지 창. 최대 kMaxSsrcs개이며 선형 탐색한다.
    std::vector<ReplayWindow> replay_windows_;
};


}  // namespace vms::crypto
