#pragma once

#include <openssl/ssl.h>

#include <expected>
#include <memory>
#include <string>

#include "crypto/certificate.h"


namespace vms::crypto {


// SRTP 키 협상에 쓰는 프로파일 이름. WebRTC 필수 프로파일이며 이것 하나만 알린다.
inline constexpr const char* kSrtpProfileName = "SRTP_AES128_CM_SHA1_80";


// DTLS 서버로 동작하는 모든 세션이 함께 쓰는 SSL_CTX 설정.
//
// DTLS 1.2 고정, 서버 인증서와 키, SRTP 프로파일, 피어 인증서 요구와 검증 콜백을 한 번 정해 두고,
// 세션마다 SSL_new(Get())으로 이 설정을 물려받은 SSL을 만든다. 생성 뒤에는 읽기만 한다.
// SSL_CTX를 소유하며 이동만 가능하다. SSL 객체도 SSL_CTX의 참조 횟수를 올리지만, 이 객체로 SSL을 만든
// DtlsTransport와 이 객체를 참조하는 IceLiteAgent가 먼저 소멸하도록 둔다.
class DtlsContext {
public:
    // DTLS 서버용 SSL_CTX를 만든다.
    //
    // certificate: 핸드셰이크에서 보낼 서버 인증서와 키. SSL_CTX가 참조 횟수를 올려 보관하므로
    //   생성한 뒤에는 certificate의 수명과 무관하다.
    // peer_verify_callback: 피어(브라우저) 인증서를 확인하는 콜백. 깊이마다 호출되며 0을 돌려주면
    //   핸드셰이크가 bad_certificate 경고로 실패한다.
    // 반환값: 만든 컨텍스트. OpenSSL 설정이 하나라도 실패하면 원인 메시지
    //
    // 피어 인증서가 없으면 핸드셰이크가 실패하도록 SSL_VERIFY_FAIL_IF_NO_PEER_CERT도 함께 건다.
    static std::expected<DtlsContext, std::string> Create(const Certificate& certificate,
                                                          SSL_verify_cb peer_verify_callback);


    // SSL_new에 넘길 SSL_CTX를 돌려준다.
    //
    // 반환값: 이 객체가 소유한 SSL_CTX. 소유권은 넘어가지 않으며 이 객체가 살아 있는 동안 유효하다.
    SSL_CTX* Get() const { return context_.get(); }


private:
    // SSL_CTX를 SSL_CTX_free로 해제하는 삭제자.
    struct SslContextDeleter {
        // SSL_CTX의 참조 횟수를 내린다.
        void operator()(SSL_CTX* context) const { ::SSL_CTX_free(context); }
    };


    // 설정을 마친 SSL_CTX로 객체를 만든다. Create()에서만 호출한다.
    //
    // context: 소유권을 넘겨받을 SSL_CTX
    explicit DtlsContext(std::unique_ptr<SSL_CTX, SslContextDeleter> context);


    // DTLS 서버 설정을 담은 SSL_CTX
    std::unique_ptr<SSL_CTX, SslContextDeleter> context_;
};


}  // namespace vms::crypto
