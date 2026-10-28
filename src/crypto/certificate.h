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


// 인증서의 SHA-256 fingerprint를 SDP 형식 문자열로 만든다.
//
// certificate: fingerprint를 계산할 인증서. 소유권은 넘어오지 않는다.
// 반환값: "AB:CD:..." 형식(대문자 16진수 32쌍, ':' 구분)의 문자열. 계산에 실패하면 오류 메시지
//
// 서버 인증서의 fingerprint와 DTLS 핸드셰이크에서 받은 브라우저 인증서의 비교에 함께 쓴다.
std::expected<std::string, std::string> ComputeSha256Fingerprint(const X509* certificate);


// 프로세스 수명 동안 쓰는 자체 서명 인증서와 그 개인 키.
//
// SDP answer의 a=fingerprint에 인증서의 SHA-256 fingerprint를 넣는다.
// DTLS 핸드셰이크도 같은 인증서와 키를 써야 fingerprint가 맞으므로, 키와 인증서를
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


    // 개인 키를 돌려준다.
    //
    // 반환값: 이 객체가 소유한 키. 소유권은 넘어가지 않으며 이 객체가 살아 있는 동안 유효하다.
    //
    // OpenSSL 함수에 넘겨 계속 보관하게 하려면 그 함수가 참조 횟수를 올려야 한다
    // (SSL_CTX_use_PrivateKey 등).
    EVP_PKEY* PrivateKey() const { return private_key_.get(); }


    // 자체 서명 인증서를 돌려준다.
    //
    // 반환값: 이 객체가 소유한 인증서. 소유권은 넘어가지 않으며 이 객체가 살아 있는 동안 유효하다.
    //
    // OpenSSL 함수에 넘겨 계속 보관하게 하려면 그 함수가 참조 횟수를 올려야 한다
    // (SSL_CTX_use_certificate 등).
    X509* X509Certificate() const { return certificate_.get(); }


private:
    // 생성을 마친 키, 인증서, fingerprint로 객체를 만든다. Generate()에서만 호출한다.
    //
    // private_key: 소유권을 넘겨받을 개인 키
    // certificate: 소유권을 넘겨받을 인증서
    // sha256_fingerprint: certificate의 SHA-256 fingerprint 문자열
    Certificate(std::unique_ptr<EVP_PKEY, OpenSslDeleter> private_key,
                std::unique_ptr<X509, OpenSslDeleter> certificate, std::string sha256_fingerprint);


    // 인증서의 개인 키. DTLS 핸드셰이크에서 쓴다.
    std::unique_ptr<EVP_PKEY, OpenSslDeleter> private_key_;


    // 자체 서명 인증서. DTLS 핸드셰이크에서 쓴다.
    std::unique_ptr<X509, OpenSslDeleter> certificate_;


    // certificate_의 SHA-256 fingerprint 문자열
    std::string sha256_fingerprint_;
};


}  // namespace vms::crypto
