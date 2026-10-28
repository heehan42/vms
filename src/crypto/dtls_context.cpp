#include "crypto/dtls_context.h"

#include <utility>

#include "crypto/openssl_error.h"


namespace vms::crypto {


std::expected<DtlsContext, std::string> DtlsContext::Create(const Certificate& certificate,
                                                            SSL_verify_cb peer_verify_callback) {
    std::unique_ptr<SSL_CTX, SslContextDeleter> context(::SSL_CTX_new(::DTLS_server_method()));
    if (context == nullptr) {
        return std::unexpected(TakeOpenSslError("DTLS SSL_CTX 생성"));
    }

    // WebRTC가 요구하는 DTLS 1.2로 고정한다. 브라우저가 1.3을 제안해도 1.2로 협상된다.
    if (::SSL_CTX_set_min_proto_version(context.get(), DTLS1_2_VERSION) != 1 ||
        ::SSL_CTX_set_max_proto_version(context.get(), DTLS1_2_VERSION) != 1) {
        return std::unexpected(TakeOpenSslError("DTLS 버전 설정"));
    }

    // answer의 a=fingerprint를 이 인증서로 계산했으므로, 핸드셰이크에서도 같은 인증서를 보내야 한다.
    if (::SSL_CTX_use_certificate(context.get(), certificate.X509Certificate()) != 1 ||
        ::SSL_CTX_use_PrivateKey(context.get(), certificate.PrivateKey()) != 1 ||
        ::SSL_CTX_check_private_key(context.get()) != 1) {
        return std::unexpected(TakeOpenSslError("DTLS 인증서와 키 설정"));
    }

    // 다른 OpenSSL 설정 함수와 달리 성공하면 0을 돌려준다.
    if (::SSL_CTX_set_tlsext_use_srtp(context.get(), kSrtpProfileName) != 0) {
        return std::unexpected(TakeOpenSslError("SRTP 프로파일 설정"));
    }

    ::SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                         peer_verify_callback);
    return DtlsContext(std::move(context));
}


DtlsContext::DtlsContext(std::unique_ptr<SSL_CTX, SslContextDeleter> context)
    : context_(std::move(context)) {}


}  // namespace vms::crypto
