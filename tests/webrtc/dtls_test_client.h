#pragma once

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "crypto/certificate.h"


namespace vms::test {


// 테스트에서 브라우저 역할을 하는 DTLS 클라이언트.
//
// OpenSSL DTLS_client_method로 만든 SSL과 datagram pair BIO로 구성하며, 소켓을 모른다.
// 테스트 코드가 Receive()로 서버의 데이터그램을 넣고, Advance()가 돌려준 데이터그램을 서버에 전달한다.
// 자체 서명 인증서를 소유하고 서버 인증서는 검증 없이 받아들인다. SSL이 이 객체를 가리키지 않으므로
// 이동할 수 있다.
class DtlsTestClient {
public:
    // 클라이언트의 핸드셰이크 진행 상태
    enum class State {
        // 핸드셰이크 진행 중
        kHandshaking,

        // 핸드셰이크 완료
        kConnected,

        // 핸드셰이크나 레코드 처리 오류
        kFailed,
    };


    // 클라이언트를 만든다.
    //
    // offers_srtp: true이면 ClientHello의 use_srtp 확장에 SRTP_AES128_CM_SHA1_80을 넣는다.
    // 반환값: 만든 클라이언트. 인증서 생성이나 OpenSSL 설정이 실패하면 원인 메시지
    static std::expected<DtlsTestClient, std::string> Create(bool offers_srtp);


    // 받은 데이터그램을 처리하기 전에 클라이언트 쪽 BIO에 넣는다.
    //
    // datagram: 서버가 보낸 데이터그램 하나
    //
    // 넣기만 하며, 처리는 다음 Advance()에서 한다.
    void Receive(std::span<const std::uint8_t> datagram);


    // 핸드셰이크를 진행하거나(완료 전) 받은 레코드를 읽고, 서버로 보낼 데이터그램을 돌려준다.
    //
    // 반환값: 보낼 데이터그램 목록. 처음 호출하면 ClientHello가 나온다.
    std::vector<std::vector<std::uint8_t>> Advance();


    // close_notify를 보내고 그 데이터그램을 돌려준다.
    //
    // 반환값: close_notify 데이터그램 목록
    std::vector<std::vector<std::uint8_t>> SendCloseNotify();


    // 현재 상태를 돌려준다.
    State CurrentState() const { return state_; }


    // 클라이언트 인증서의 SHA-256 fingerprint를 돌려준다.
    //
    // 반환값: "AB:CD:..." 형식 문자열. 이 객체가 살아 있는 동안 유효하다.
    const std::string& Sha256Fingerprint() const { return certificate_.Sha256Fingerprint(); }


    // 협상된 SRTP 프로파일 이름을 돌려준다.
    //
    // 반환값: 프로파일 이름. 핸드셰이크 전이거나 협상하지 않았으면 빈 optional
    std::optional<std::string> SelectedSrtpProfile() const;


    // RFC 5764 방식으로 SRTP 키 재료 60바이트를 꺼낸다.
    //
    // 반환값: 클라이언트 키(16), 서버 키(16), 클라이언트 솔트(14), 서버 솔트(14) 순서의 바이트.
    //   핸드셰이크 전이거나 실패하면 빈 optional
    std::optional<std::array<std::uint8_t, 60>> ExportSrtpKeyingMaterial() const;


    // 핸드셰이크에서 받은 서버 인증서의 SHA-256 fingerprint를 돌려준다.
    //
    // 반환값: "AB:CD:..." 형식 문자열. 서버 인증서를 받지 않았으면 빈 optional
    std::optional<std::string> PeerCertificateFingerprint() const;


private:
    // SSL을 SSL_free로 해제하는 삭제자. SSL에 붙은 클라이언트 쪽 BIO도 함께 해제된다.
    struct SslDeleter {
        // SSL을 해제한다.
        void operator()(SSL* ssl) const { ::SSL_free(ssl); }
    };


    // BIO를 BIO_free로 해제하는 삭제자.
    struct BioDeleter {
        // BIO를 해제한다.
        void operator()(BIO* bio) const { ::BIO_free(bio); }
    };


    // 구성을 마친 인증서, BIO, SSL로 객체를 만든다. Create()에서만 호출한다.
    DtlsTestClient(crypto::Certificate certificate, std::unique_ptr<BIO, BioDeleter> network_bio,
                   std::unique_ptr<SSL, SslDeleter> ssl);


    // 네트워크 쪽 BIO에 쌓인 데이터그램을 모두 꺼낸다.
    //
    // 반환값: SSL이 쓴 순서대로의 데이터그램
    std::vector<std::vector<std::uint8_t>> DrainOutput();


    // 클라이언트 인증서와 키. ssl_이 참조 횟수를 올려 쓰지만 함께 보관한다.
    crypto::Certificate certificate_;


    // 테스트 코드가 데이터그램을 주고받는 datagram pair BIO의 한쪽 끝
    std::unique_ptr<BIO, BioDeleter> network_bio_;


    // DTLS 클라이언트 SSL. datagram pair BIO의 다른 쪽 끝을 소유한다.
    std::unique_ptr<SSL, SslDeleter> ssl_;


    // 핸드셰이크 진행 상태
    State state_ = State::kHandshaking;
};


}  // namespace vms::test
