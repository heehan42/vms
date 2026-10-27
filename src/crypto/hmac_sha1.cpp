#include "crypto/hmac_sha1.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>

#include <memory>

#include "crypto/openssl_error.h"


namespace vms::crypto {


namespace {


// OpenSSL의 HMAC 알고리즘 구현을 돌려준다.
//
// 반환값: HMAC 구현. 찾지 못하면 nullptr
//
// 알고리즘 조회(fetch)는 비용이 있어 연결 확인 요청마다 하지 않도록 처음 한 번만 하고,
// 프로세스가 끝날 때까지 해제하지 않고 재사용한다.
EVP_MAC* HmacAlgorithm() {
    static EVP_MAC* const algorithm = ::EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    return algorithm;
}


}  // namespace


std::expected<HmacSha1Digest, std::string> ComputeHmacSha1(
    std::string_view key, std::span<const std::span<const std::uint8_t>> segments) {
    EVP_MAC* const algorithm = HmacAlgorithm();
    if (algorithm == nullptr) {
        return std::unexpected(TakeOpenSslError("HMAC 알고리즘 조회"));
    }
    const std::unique_ptr<EVP_MAC_CTX, decltype(&::EVP_MAC_CTX_free)> context(
        ::EVP_MAC_CTX_new(algorithm), &::EVP_MAC_CTX_free);
    if (context == nullptr) {
        return std::unexpected(TakeOpenSslError("HMAC 컨텍스트 생성"));
    }

    // 키 포인터가 nullptr이면 이전 키를 재사용하라는 뜻이 되므로, 빈 키도 유효한 주소를 넘긴다.
    static constexpr unsigned char kEmptyKey[1] = {0};
    const auto* key_data =
        key.empty() ? kEmptyKey : reinterpret_cast<const unsigned char*>(key.data());
    char digest_name[] = "SHA1";
    const OSSL_PARAM parameters[] = {
        ::OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest_name, 0),
        ::OSSL_PARAM_construct_end(),
    };
    if (::EVP_MAC_init(context.get(), key_data, key.size(), parameters) != 1) {
        return std::unexpected(TakeOpenSslError("HMAC-SHA1 초기화"));
    }
    for (const std::span<const std::uint8_t> segment : segments) {
        if (::EVP_MAC_update(context.get(), segment.data(), segment.size()) != 1) {
            return std::unexpected(TakeOpenSslError("HMAC-SHA1 계산"));
        }
    }

    HmacSha1Digest digest{};
    std::size_t digest_size = 0;
    if (::EVP_MAC_final(context.get(), digest.data(), &digest_size, digest.size()) != 1 ||
        digest_size != kHmacSha1Size) {
        return std::unexpected(TakeOpenSslError("HMAC-SHA1 결과 생성"));
    }
    return digest;
}


std::expected<HmacSha1Digest, std::string> ComputeHmacSha1(std::string_view key,
                                                           std::span<const std::uint8_t> data) {
    const std::array<std::span<const std::uint8_t>, 1> segments = {data};
    return ComputeHmacSha1(key, segments);
}


}  // namespace vms::crypto
