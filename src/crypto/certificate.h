#pragma once

#include <openssl/evp.h>
#include <openssl/x509.h>

#include <expected>
#include <memory>
#include <string>


namespace vms::crypto {


// OpenSSL 객체를 해제 함수로 정리하는 삭제자.
struct OpenSslDeleter {
    // 키를 해제한다.
    void operator()(EVP_PKEY* key) const { ::EVP_PKEY_free(key); }


    // 인증서를 해제한다.
    void operator()(X509* certificate) const { ::X509_free(certificate); }
};


// 프로세스 수명 동안 쓰는 자체 서명 인증서와 그 개인 키.
//
// SDP answer의 a=fingerprint에 인증서의 SHA-256 fingerprint를 넣는다.
// 이후 마일스톤의 DTLS 핸드셰이크도 같은 인증서와 키를 써야 fingerprint가 맞으므로, 키와 인증서를
// 이 객체가 소유하고 보관한다. 이동만 가능하다.
class Certificate {
public:
    // ECDSA P-256 키를 만들고, 그 키로 서명한 자체 서명 인증서를 만든다.
    //
    // 반환값: 생성한 인증서. OpenSSL 호출이 실패하면 오류 메시지
    //
    // CN은 "vms", 유효 기간은 생성 1일 전부터 30일, 일련번호는 난수, 서명 해시는 SHA-256이다.
    static std::expected<Certificate, std::string> Generate();


    // 인증서의 SHA-256 fingerprint를 돌려준다.
    //
    // 반환값: "AB:CD:..." 형식(대문자 16진수 32쌍, ':' 구분)의 문자열. 이 객체가 살아 있는 동안 유효하다.
    const std::string& Sha256Fingerprint() const { return sha256_fingerprint_; }


private:
    // 생성을 마친 키, 인증서, fingerprint로 객체를 만든다. Generate()에서만 호출한다.
    //
    // private_key: 소유권을 넘겨받을 개인 키
    // certificate: 소유권을 넘겨받을 인증서
    // sha256_fingerprint: certificate의 SHA-256 fingerprint 문자열
    Certificate(std::unique_ptr<EVP_PKEY, OpenSslDeleter> private_key,
                std::unique_ptr<X509, OpenSslDeleter> certificate, std::string sha256_fingerprint);


    // 인증서의 개인 키. 이후 마일스톤의 DTLS 핸드셰이크에서 쓴다.
    std::unique_ptr<EVP_PKEY, OpenSslDeleter> private_key_;


    // 자체 서명 인증서. 이후 마일스톤의 DTLS 핸드셰이크에서 쓴다.
    std::unique_ptr<X509, OpenSslDeleter> certificate_;


    // certificate_의 SHA-256 fingerprint 문자열
    std::string sha256_fingerprint_;
};


}  // namespace vms::crypto
