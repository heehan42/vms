#include "crypto/srtp.h"

#include <openssl/crypto.h>

#include <algorithm>
#include <format>
#include <limits>
#include <string_view>
#include <utility>

#include "crypto/hmac_sha1.h"
#include "crypto/openssl_error.h"


namespace vms::crypto {


namespace {


// RTP 고정 헤더 크기(바이트)
constexpr std::size_t kRtpFixedHeaderSize = 12;


// 키 유도 레이블(RFC 3711 4.3.1). 레이블마다 다른 세션 키가 나온다.
enum class KeyDerivationLabel : std::uint8_t {
    kEncryptionKey = 0x00,
    kAuthenticationKey = 0x01,
    kSalt = 0x02,
};


// 빅 엔디언 16비트 값을 읽는다.
//
// bytes: 2바이트 이상인 데이터
// 반환값: 앞 2바이트의 값
std::uint16_t ReadUint16(std::span<const std::uint8_t> bytes) {
    return static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]);
}


// 빅 엔디언 32비트 값을 읽는다.
//
// bytes: 4바이트 이상인 데이터
// 반환값: 앞 4바이트의 값
std::uint32_t ReadUint32(std::span<const std::uint8_t> bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
}


// RTP 패킷 헤더의 길이를 계산한다.
//
// rtp_packet: RTP 패킷
// 반환값: CSRC 목록과 헤더 확장을 포함한 헤더 길이. 버전이 2가 아니거나 헤더가 패킷보다 길면 오류 메시지
//
// SRTP는 헤더를 평문으로 두므로, 암호화할 페이로드가 어디서 시작하는지만 알면 된다.
std::expected<std::size_t, std::string> ComputeRtpHeaderSize(std::span<const std::uint8_t> rtp_packet) {
    if (rtp_packet.size() < kRtpFixedHeaderSize) {
        return std::unexpected(std::format("RTP 패킷이 고정 헤더보다 짧음: {}바이트", rtp_packet.size()));
    }
    if ((rtp_packet[0] >> 6) != 2) {
        return std::unexpected(std::format("RTP 버전이 2가 아님: {}", rtp_packet[0] >> 6));
    }

    const std::size_t csrc_count = rtp_packet[0] & 0x0F;
    std::size_t header_size = kRtpFixedHeaderSize + 4 * csrc_count;
    const bool has_extension = (rtp_packet[0] & 0x10) != 0;
    if (has_extension) {
        // 확장 헤더는 4바이트(프로필, 32비트 단위 길이) 뒤에 본문이 온다.
        if (rtp_packet.size() < header_size + 4) {
            return std::unexpected("RTP 확장 헤더가 잘림");
        }
        const std::size_t extension_words = ReadUint16(rtp_packet.subspan(header_size + 2));
        header_size += 4 + 4 * extension_words;
    }
    if (rtp_packet.size() < header_size) {
        return std::unexpected(
            std::format("RTP 헤더({}바이트)가 패킷({}바이트)보다 김", header_size, rtp_packet.size()));
    }
    return header_size;
}


// 컨텍스트에 이미 설정한 AES-128 CTR 키로, IV만 바꿔 data에 키스트림을 XOR한다.
//
// context: EVP_aes_128_ctr와 키를 설정한 컨텍스트
// iv: 첫 블록의 카운터 값
// data: 제자리에서 바뀌는 데이터
// 반환값: 성공하면 빈 값, OpenSSL이 실패하면 오류 메시지
std::expected<void, std::string> XorKeystreamWithContext(EVP_CIPHER_CTX* context,
                                                         std::span<const std::uint8_t, kAesCmIvSize> iv,
                                                         std::span<std::uint8_t> data) {
    if (::EVP_EncryptInit_ex(context, nullptr, nullptr, nullptr, iv.data()) != 1) {
        return std::unexpected(TakeOpenSslError("AES-CM IV 설정"));
    }
    if (data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected("AES-CM 입력이 너무 김");
    }
    // CTR 모드는 블록 정렬이 없어 입력과 같은 길이를 바로 내놓으므로 EVP_EncryptFinal_ex가 필요 없다.
    int output_size = 0;
    if (::EVP_EncryptUpdate(context, data.data(), &output_size, data.data(),
                            static_cast<int>(data.size())) != 1 ||
        static_cast<std::size_t>(output_size) != data.size()) {
        return std::unexpected(TakeOpenSslError("AES-CM 키스트림 생성"));
    }
    return {};
}


// AES-128 CTR 키를 설정한 새 컨텍스트를 만든다.
//
// key: AES-128 키
// 반환값: IV를 정하기 전의 컨텍스트. OpenSSL이 실패하면 오류 메시지
std::expected<std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)>, std::string>
CreateAesCtrContext(std::span<const std::uint8_t, kSrtpMasterKeySize> key) {
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)> context(::EVP_CIPHER_CTX_new(),
                                                                              &::EVP_CIPHER_CTX_free);
    if (context == nullptr) {
        return std::unexpected(TakeOpenSslError("AES 컨텍스트 생성"));
    }
    if (::EVP_EncryptInit_ex(context.get(), ::EVP_aes_128_ctr(), nullptr, key.data(), nullptr) != 1) {
        return std::unexpected(TakeOpenSslError("AES-128 CTR 키 설정"));
    }
    return context;
}


// 레이블 하나에 대한 세션 키를 유도한다(RFC 3711 4.3.1, 키 유도 주기 0).
//
// context: 마스터 키를 설정한 AES-128 CTR 컨텍스트
// master_salt: 마스터 솔트
// label: 만들 세션 키의 종류
// output: 유도한 키를 받을 버퍼. 크기만큼 키스트림을 만든다.
// 반환값: 성공하면 빈 값, OpenSSL이 실패하면 오류 메시지
std::expected<void, std::string> DeriveSessionKey(EVP_CIPHER_CTX* context,
                                                  std::span<const std::uint8_t, kSrtpMasterSaltSize> master_salt,
                                                  KeyDerivationLabel label, std::span<std::uint8_t> output) {
    // x = 마스터 솔트 XOR (레이블 << 48)이고 IV = x << 16이다. 112비트 솔트에서 레이블은 앞에서 8번째
    // 바이트에 놓이며, 키 유도 주기가 0이므로 인덱스 항은 없다.
    std::array<std::uint8_t, kAesCmIvSize> iv{};
    std::ranges::copy(master_salt, iv.begin());
    iv[7] ^= std::to_underlying(label);

    std::ranges::fill(output, 0);
    return XorKeystreamWithContext(context, iv, output);
}


}  // namespace


std::expected<SrtpSessionKeys, std::string> DeriveSrtpSessionKeys(
    std::span<const std::uint8_t, kSrtpMasterKeySize> master_key,
    std::span<const std::uint8_t, kSrtpMasterSaltSize> master_salt) {
    auto context = CreateAesCtrContext(master_key);
    if (!context) {
        return std::unexpected(context.error());
    }

    SrtpSessionKeys keys;
    const auto derive = [&](KeyDerivationLabel label, std::span<std::uint8_t> output) {
        return DeriveSessionKey(context->get(), master_salt, label, output);
    };
    if (auto derived = derive(KeyDerivationLabel::kEncryptionKey, keys.encryption_key); !derived) {
        return std::unexpected(derived.error());
    }
    if (auto derived = derive(KeyDerivationLabel::kAuthenticationKey, keys.authentication_key); !derived) {
        ::OPENSSL_cleanse(&keys, sizeof(keys));
        return std::unexpected(derived.error());
    }
    if (auto derived = derive(KeyDerivationLabel::kSalt, keys.salt); !derived) {
        ::OPENSSL_cleanse(&keys, sizeof(keys));
        return std::unexpected(derived.error());
    }
    return keys;
}


std::expected<void, std::string> XorAesCmKeystream(std::span<const std::uint8_t, kSrtpMasterKeySize> key,
                                                   std::span<const std::uint8_t, kAesCmIvSize> iv,
                                                   std::span<std::uint8_t> data) {
    auto context = CreateAesCtrContext(key);
    if (!context) {
        return std::unexpected(context.error());
    }
    return XorKeystreamWithContext(context->get(), iv, data);
}


std::expected<std::unique_ptr<SrtpSender>, std::string> SrtpSender::Create(
    std::span<const std::uint8_t, kSrtpMasterKeySize> master_key,
    std::span<const std::uint8_t, kSrtpMasterSaltSize> master_salt) {
    auto session_keys = DeriveSrtpSessionKeys(master_key, master_salt);
    if (!session_keys) {
        return std::unexpected(session_keys.error());
    }

    // 키는 생성할 때 한 번만 설정하고, 패킷마다 IV만 바꿔 키 스케줄 계산을 반복하지 않는다.
    auto context = CreateAesCtrContext(session_keys->encryption_key);
    if (!context) {
        ::OPENSSL_cleanse(&*session_keys, sizeof(*session_keys));
        return std::unexpected(context.error());
    }
    std::unique_ptr<SrtpSender> sender(new SrtpSender(
        *session_keys, std::unique_ptr<EVP_CIPHER_CTX, CipherContextDeleter>(context->release())));
    ::OPENSSL_cleanse(&*session_keys, sizeof(*session_keys));
    return sender;
}


SrtpSender::SrtpSender(const SrtpSessionKeys& session_keys,
                       std::unique_ptr<EVP_CIPHER_CTX, CipherContextDeleter> cipher_context)
    : session_keys_(session_keys), cipher_context_(std::move(cipher_context)) {}


SrtpSender::~SrtpSender() { ::OPENSSL_cleanse(&session_keys_, sizeof(session_keys_)); }


std::expected<std::vector<std::uint8_t>, std::string> SrtpSender::Protect(
    std::span<const std::uint8_t> rtp_packet) {
    const auto header_size = ComputeRtpHeaderSize(rtp_packet);
    if (!header_size) {
        return std::unexpected(header_size.error());
    }
    const std::uint16_t sequence_number = ReadUint16(rtp_packet.subspan(2));
    const std::uint32_t ssrc = ReadUint32(rtp_packet.subspan(8));

    // 송신 순번은 1씩 늘기만 하므로, 직전보다 작아졌다면 65535에서 0으로 넘어간 것이다.
    std::uint32_t rollover_counter = rollover_counter_;
    if (last_sequence_number_ && sequence_number < *last_sequence_number_) {
        ++rollover_counter;
    }
    const std::uint64_t packet_index = (static_cast<std::uint64_t>(rollover_counter) << 16) | sequence_number;

    // IV = (세션 솔트 << 16) XOR (SSRC << 64) XOR (인덱스 << 16).
    // 128비트 값에서 SSRC는 4~7번째 바이트, 48비트 인덱스는 8~13번째 바이트에 놓인다.
    std::array<std::uint8_t, kAesCmIvSize> iv{};
    std::ranges::copy(session_keys_.salt, iv.begin());
    for (int byte = 0; byte < 4; ++byte) {
        iv[4 + byte] ^= static_cast<std::uint8_t>(ssrc >> (24 - 8 * byte));
    }
    for (int byte = 0; byte < 6; ++byte) {
        iv[8 + byte] ^= static_cast<std::uint8_t>(packet_index >> (40 - 8 * byte));
    }

    std::vector<std::uint8_t> srtp_packet;
    srtp_packet.reserve(rtp_packet.size() + kSrtpAuthenticationTagSize);
    srtp_packet.assign(rtp_packet.begin(), rtp_packet.end());
    if (auto encrypted = XorKeystreamWithContext(cipher_context_.get(), iv,
                                                 std::span(srtp_packet).subspan(*header_size));
        !encrypted) {
        return std::unexpected(encrypted.error());
    }

    // 인증 태그는 암호화한 패킷 뒤에 ROC를 이어 붙인 데이터의 HMAC이다. ROC는 패킷에 넣지 않는다.
    const std::array<std::uint8_t, 4> rollover_counter_bytes = {
        static_cast<std::uint8_t>(rollover_counter >> 24), static_cast<std::uint8_t>(rollover_counter >> 16),
        static_cast<std::uint8_t>(rollover_counter >> 8), static_cast<std::uint8_t>(rollover_counter)};
    const std::array<std::span<const std::uint8_t>, 2> authenticated_segments = {
        std::span<const std::uint8_t>(srtp_packet), rollover_counter_bytes};
    const std::string_view authentication_key(
        reinterpret_cast<const char*>(session_keys_.authentication_key.data()),
        session_keys_.authentication_key.size());
    const auto tag = ComputeHmacSha1(authentication_key, authenticated_segments);
    if (!tag) {
        return std::unexpected(tag.error());
    }
    srtp_packet.insert(srtp_packet.end(), tag->begin(), tag->begin() + kSrtpAuthenticationTagSize);

    rollover_counter_ = rollover_counter;
    last_sequence_number_ = sequence_number;
    return srtp_packet;
}


}  // namespace vms::crypto
