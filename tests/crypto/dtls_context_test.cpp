#include "crypto/dtls_context.h"

#include <openssl/srtp.h>
#include <openssl/ssl.h>

#include <memory>
#include <string>

#include "crypto/certificate.h"
#include "test_support.h"

using vms::crypto::Certificate;
using vms::crypto::DtlsContext;
using vms::test::Expect;


namespace {


// 모든 인증서를 받아들이는 검증 콜백. 컨텍스트에 콜백이 연결되는지만 확인하는 데 쓴다.
int AcceptAnyCertificate(int, X509_STORE_CTX*) { return 1; }


// 컨텍스트를 만들고 키·인증서 일치, DTLS 1.2 고정, 피어 인증서 요구와 검증 콜백을 검사한다.
void TestCreatesServerContext(const Certificate& certificate) {
    const auto context = DtlsContext::Create(certificate, AcceptAnyCertificate);
    if (!context) {
        Expect(false, "DTLS 컨텍스트 생성이 성공해야 한다: " + context.error());
        return;
    }
    SSL_CTX* const ssl_context = context->Get();
    Expect(::SSL_CTX_check_private_key(ssl_context) == 1, "키와 인증서가 짝이어야 한다");
    Expect(::SSL_CTX_get0_certificate(ssl_context) == certificate.X509Certificate(),
           "서버 인증서가 Certificate의 인증서여야 한다");
    Expect(::SSL_CTX_get_min_proto_version(ssl_context) == DTLS1_2_VERSION &&
               ::SSL_CTX_get_max_proto_version(ssl_context) == DTLS1_2_VERSION,
           "DTLS 1.2 고정");
    Expect(::SSL_CTX_get_verify_mode(ssl_context) ==
               (SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT),
           "피어 인증서 요구");
    Expect(::SSL_CTX_get_verify_callback(ssl_context) == AcceptAnyCertificate,
           "검증 콜백 연결");
}


// 컨텍스트에서 만든 SSL이 SRTP_AES128_CM_SHA1_80 하나만 알리는지 검사한다.
void TestConfiguresSrtpProfile(const Certificate& certificate) {
    const auto context = DtlsContext::Create(certificate, AcceptAnyCertificate);
    if (!context) {
        Expect(false, "DTLS 컨텍스트 생성이 성공해야 한다: " + context.error());
        return;
    }
    const std::unique_ptr<SSL, decltype(&::SSL_free)> ssl(::SSL_new(context->Get()), &::SSL_free);
    if (ssl == nullptr) {
        Expect(false, "SSL 생성");
        return;
    }

    STACK_OF(SRTP_PROTECTION_PROFILE)* const profiles = ::SSL_get_srtp_profiles(ssl.get());
    Expect(profiles != nullptr && sk_SRTP_PROTECTION_PROFILE_num(profiles) == 1,
           "SRTP 프로파일이 하나여야 한다");
    if (profiles == nullptr || sk_SRTP_PROTECTION_PROFILE_num(profiles) != 1) {
        return;
    }
    const SRTP_PROTECTION_PROFILE* const profile = sk_SRTP_PROTECTION_PROFILE_value(profiles, 0);
    Expect(profile->id == SRTP_AES128_CM_SHA1_80 &&
               std::string(profile->name) == "SRTP_AES128_CM_SHA1_80",
           "SRTP_AES128_CM_SHA1_80 프로파일");
}


}  // namespace


int main() {
    const auto certificate = Certificate::Generate();
    if (!certificate) {
        Expect(false, "인증서 생성: " + certificate.error());
        return vms::test::ExitCode();
    }
    TestCreatesServerContext(*certificate);
    TestConfiguresSrtpProfile(*certificate);
    return vms::test::ExitCode();
}
