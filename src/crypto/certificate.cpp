#include "crypto/certificate.h"

#include <openssl/bn.h>
#include <openssl/ec.h>

#include <array>
#include <format>
#include <utility>

#include "crypto/openssl_error.h"


namespace vms::crypto {


namespace {


// 인증서 유효 기간의 시작을 생성 시각보다 앞당기는 시간(초). 시계가 조금 어긋나도 유효하게 한다.
constexpr long kValidityBackdateSeconds = 24 * 60 * 60;


// 인증서 유효 기간(초). 서버를 다시 시작할 때마다 새로 만들므로 길 필요가 없다.
constexpr long kValiditySeconds = 30L * 24 * 60 * 60;


// 인증서 일련번호에 쓸 난수의 비트 수. 양수로 해석되도록 64비트보다 작게 잡는다.
constexpr int kSerialNumberBits = 63;


// 인증서에 난수 일련번호를 넣는다.
//
// certificate: 일련번호를 넣을 인증서
// 반환값: OpenSSL 호출이 모두 성공하면 true
bool SetRandomSerialNumber(X509* certificate) {
    const std::unique_ptr<BIGNUM, decltype(&::BN_free)> serial(::BN_new(), &::BN_free);
    return serial != nullptr &&
           ::BN_rand(serial.get(), kSerialNumberBits, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) == 1 &&
           ::BN_to_ASN1_INTEGER(serial.get(), ::X509_get_serialNumber(certificate)) != nullptr;
}


// 인증서의 주체와 발급자 이름을 CN=vms로 정한다. 자체 서명이므로 둘이 같다.
//
// certificate: 이름을 넣을 인증서
// 반환값: OpenSSL 호출이 모두 성공하면 true
bool SetSelfSignedName(X509* certificate) {
    X509_NAME* name = ::X509_get_subject_name(certificate);
    return ::X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                        reinterpret_cast<const unsigned char*>("vms"), -1, -1,
                                        0) == 1 &&
           ::X509_set_issuer_name(certificate, name) == 1;
}


// 인증서의 SHA-256 fingerprint를 SDP 형식 문자열로 만든다.
//
// certificate: fingerprint를 계산할 인증서
// 반환값: "AB:CD:..." 형식 문자열. 계산에 실패하면 오류 메시지
std::expected<std::string, std::string> ComputeSha256Fingerprint(const X509* certificate) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (::X509_digest(certificate, ::EVP_sha256(), digest.data(), &digest_size) != 1) {
        return std::unexpected(TakeOpenSslError("인증서 fingerprint 계산"));
    }

    std::string fingerprint;
    for (unsigned int index = 0; index < digest_size; ++index) {
        if (index > 0) {
            fingerprint += ':';
        }
        fingerprint += std::format("{:02X}", digest[index]);
    }
    return fingerprint;
}


}  // namespace


std::expected<Certificate, std::string> Certificate::Generate() {
    std::unique_ptr<EVP_PKEY, OpenSslDeleter> private_key(::EVP_EC_gen("P-256"));
    if (private_key == nullptr) {
        return std::unexpected(TakeOpenSslError("ECDSA P-256 키 생성"));
    }

    std::unique_ptr<X509, OpenSslDeleter> certificate(::X509_new());
    const bool is_filled =
        certificate != nullptr && ::X509_set_version(certificate.get(), X509_VERSION_3) == 1 &&
        SetRandomSerialNumber(certificate.get()) &&
        ::X509_gmtime_adj(::X509_getm_notBefore(certificate.get()), -kValidityBackdateSeconds) !=
            nullptr &&
        ::X509_gmtime_adj(::X509_getm_notAfter(certificate.get()), kValiditySeconds) != nullptr &&
        ::X509_set_pubkey(certificate.get(), private_key.get()) == 1 &&
        SetSelfSignedName(certificate.get());
    if (!is_filled) {
        return std::unexpected(TakeOpenSslError("인증서 내용 설정"));
    }
    if (::X509_sign(certificate.get(), private_key.get(), ::EVP_sha256()) <= 0) {
        return std::unexpected(TakeOpenSslError("인증서 서명"));
    }

    auto fingerprint = ComputeSha256Fingerprint(certificate.get());
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    return Certificate(std::move(private_key), std::move(certificate), *std::move(fingerprint));
}


Certificate::Certificate(std::unique_ptr<EVP_PKEY, OpenSslDeleter> private_key,
                         std::unique_ptr<X509, OpenSslDeleter> certificate,
                         std::string sha256_fingerprint)
    : private_key_(std::move(private_key)),
      certificate_(std::move(certificate)),
      sha256_fingerprint_(std::move(sha256_fingerprint)) {}


}  // namespace vms::crypto
