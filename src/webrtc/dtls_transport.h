#pragma once

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "crypto/dtls_context.h"


namespace vms::webrtc {


// UDP로 주고받는 데이터그램 하나의 바이트
using Datagram = std::vector<std::uint8_t>;


// 세션 하나의 DTLS 상태
enum class DtlsState {
    // ClientHello를 기다리거나 핸드셰이크 진행 중
    kHandshaking,

    // 핸드셰이크 완료, SRTP 키 재료 보관
    kConnected,

    // fingerprint 불일치, SRTP 미협상, 치명적 경고, 프로토콜 오류 등으로 실패. 이후 입력은 버린다.
    kFailed,

    // 브라우저의 close_notify를 받아 닫힘. 이후 입력은 버린다.
    kClosed,
};


// DTLS-SRTP로 정한 SRTP 마스터 키와 솔트(SRTP_AES128_CM_SHA1_80 기준).
//
// 서버가 DTLS 서버이므로 RFC 5764의 "client"가 브라우저(remote), "server"가 서버(local)다.
struct SrtpKeyMaterial {
    // 서버가 보내는 SRTP 패킷을 암호화할 마스터 키
    std::array<std::uint8_t, 16> local_key{};


    // 서버가 보내는 SRTP 패킷의 마스터 솔트
    std::array<std::uint8_t, 14> local_salt{};


    // 브라우저가 보내는 SRTP 패킷을 복호화할 마스터 키
    std::array<std::uint8_t, 16> remote_key{};


    // 브라우저가 보내는 SRTP 패킷의 마스터 솔트
    std::array<std::uint8_t, 14> remote_salt{};
};


// 세션 하나의 DTLS 서버 상태. 받은 데이터그램과 타이머 만료를 입력으로 받아 보낼 데이터그램을 돌려준다.
//
// 소켓과 이벤트 루프를 모른다. 데이터그램 송신과 재전송 타이머 설정은 호출자가 반환값과 NextTimeout()으로
// 처리한다. SSL과 datagram pair BIO를 소유하며, SSL이 이 객체의 주소를 보관하므로(검증 콜백용)
// std::unique_ptr로만 다루고 이동하지 않는다. 생성에 쓴 DtlsContext보다 먼저 소멸해야 한다.
class DtlsTransport {
public:
    // 세션 하나의 DTLS 서버 상태를 만든다.
    //
    // context: SSL을 만들 컨텍스트. VerifyPeerCertificate를 검증 콜백으로 연결해 만들어야 한다.
    // remote_fingerprint: offer의 브라우저 인증서 SHA-256 fingerprint. 대문자 16진수, ':' 구분
    // 반환값: kHandshaking 상태의 객체. SSL이나 BIO 생성에 실패하면 원인 메시지
    static std::expected<std::unique_ptr<DtlsTransport>, std::string> Create(
        const crypto::DtlsContext& context, std::string remote_fingerprint);


    // DtlsContext에 연결하는 피어 인증서 검증 콜백.
    //
    // preverify_ok: OpenSSL의 인증 기관 검증 결과. 브라우저 인증서는 자체 서명이므로 무시한다.
    // store: 검증 중인 인증서와 SSL을 담은 저장소
    // 반환값: 깊이 0 인증서의 fingerprint가 offer 값과 같으면 1, 다르면 0
    //
    // 0을 돌려주면 OpenSSL이 bad_certificate 경고를 보내고 핸드셰이크를 끝낸다. 불일치 원인은
    // 해당 DtlsTransport에 기록되어 FailureReason()으로 나온다.
    static int VerifyPeerCertificate(int preverify_ok, X509_STORE_CTX* store);


    // SRTP 키 재료를 지운 뒤 SSL과 BIO를 해제한다.
    ~DtlsTransport();


    // SSL이 이 객체의 주소를 보관하므로 복사를 금지한다.
    DtlsTransport(const DtlsTransport&) = delete;


    // SSL이 이 객체의 주소를 보관하므로 복사 대입을 금지한다.
    DtlsTransport& operator=(const DtlsTransport&) = delete;


    // 브라우저가 보낸 DTLS 데이터그램 하나를 처리한다.
    //
    // datagram: 첫 바이트가 DTLS 범위인 데이터그램
    // 반환값: 브라우저로 보낼 데이터그램 목록. 실패할 때 OpenSSL이 쓴 경고도 포함한다.
    //
    // kFailed, kClosed이면 아무것도 하지 않는다. 호출 뒤 State()가 바뀌었을 수 있고, 재전송 타이머를
    // NextTimeout()으로 다시 걸어야 한다.
    std::vector<Datagram> HandleDatagram(std::span<const std::uint8_t> datagram);


    // 재전송 타이머가 만료되었을 때 호출해 마지막 핸드셰이크 메시지를 다시 보낸다.
    //
    // 반환값: 재전송할 데이터그램 목록. 기한이 아직 남았거나 타이머가 없으면 비어 있다.
    //
    // 재전송 횟수 한도를 넘으면 kFailed가 된다. 호출 뒤 NextTimeout()으로 타이머를 다시 걸어야 한다.
    std::vector<Datagram> HandleTimeout();


    // 다음 재전송까지 남은 시간을 돌려준다.
    //
    // 반환값: 밀리초로 올림한 남은 시간(이미 지났으면 0). 재전송을 기다리는 메시지가 없거나 상태가
    //   kFailed, kClosed이면 빈 optional
    std::optional<std::chrono::milliseconds> NextTimeout() const;


    // 현재 상태를 돌려준다.
    DtlsState State() const { return state_; }


    // kFailed가 된 원인을 돌려준다.
    //
    // 반환값: 원인 문구. kFailed가 아니면 빈 문자열이며, 이 객체가 살아 있는 동안 유효하다.
    const std::string& FailureReason() const { return failure_reason_; }


    // 협상된 SRTP 프로파일 이름을 돌려준다.
    //
    // 반환값: 정적 문자열. kConnected가 아니면 빈 문자열
    std::string_view SelectedSrtpProfileName() const;


    // 핸드셰이크로 정한 SRTP 키 재료를 돌려준다.
    //
    // 반환값: kConnected가 된 뒤에만 값이 있다. 이 객체가 살아 있는 동안 유효하다.
    const std::optional<SrtpKeyMaterial>& SrtpKeys() const { return srtp_keys_; }


private:
    // SSL을 SSL_free로 해제하는 삭제자. SSL에 붙은 SSL 쪽 BIO도 함께 해제된다.
    struct SslDeleter {
        // SSL을 해제한다.
        void operator()(SSL* ssl) const { ::SSL_free(ssl); }
    };


    // BIO를 BIO_free로 해제하는 삭제자.
    struct BioDeleter {
        // BIO를 해제한다.
        void operator()(BIO* bio) const { ::BIO_free(bio); }
    };


    // 구성을 마친 SSL과 network BIO로 객체를 만든다. Create()에서만 호출한다.
    DtlsTransport(std::unique_ptr<SSL, SslDeleter> ssl,
                  std::unique_ptr<BIO, BioDeleter> network_bio, std::string remote_fingerprint);


    // 핸드셰이크를 한 단계 진행하고, 끝났으면 완료 판정을 한다.
    void ContinueHandshake();


    // 핸드셰이크 뒤에 받은 레코드를 처리한다. 응용 데이터는 버리고 close_notify면 kClosed가 된다.
    //
    // 지금은 브라우저가 서버의 Flight 6(ChangeCipherSpec·Finished)을 못 받아
    // Flight 5(Certificate·ClientKeyExchange·CertificateVerify·ChangeCipherSpec·Finished)를 재전송한 경우와
    // close_notify를 처리하려고 둔다. 앞의 경우 OpenSSL이 읽는 과정에서 Flight 6을 다시 쓰므로, 호출자는
    // DrainOutput()으로 보내기만 하면 된다.
    // WebRTC에서 DTLS 응용 데이터는 DataChannel의 SCTP 패킷뿐이며, answer에 DataChannel(m=application)을
    // 넣지 않으므로 지금은 오지 않는다. 이후 DataChannel을 구현하면 응용 데이터를 버리는 대신 SCTP 스택에
    // 넘기는 자리가 이 함수다. 스택이 만든 응답은 SSL_write로 암호화해 DrainOutput()으로 보낸다.
    void ReadRecords();


    // 핸드셰이크 완료 뒤 SRTP 프로파일을 확인하고 키 재료를 꺼내 kConnected 또는 kFailed로 바꾼다.
    void CompleteHandshake();


    // kFailed로 바꾸고 원인을 기록한다.
    //
    // reason: 실패 원인 문구
    void Fail(std::string reason);


    // OpenSSL 호출이 실패했을 때의 원인을 만든다.
    //
    // operation: 실패한 작업 설명
    // 반환값: 검증 콜백이 기록한 원인이 있으면 그것, 없으면 오류 큐의 내용. 오류 큐는 비운다.
    std::string TakeFailureCause(std::string_view operation);


    // network BIO에 쌓인 데이터그램을 모두 꺼낸다.
    //
    // 반환값: SSL이 쓴 순서대로의 데이터그램
    std::vector<Datagram> DrainOutput();


    // 브라우저와 데이터그램을 주고받는 datagram pair BIO의 한쪽 끝.
    // 다른 쪽 끝은 ssl_이 소유한다. ssl_보다 먼저 선언해 ssl_이 먼저 해제되게 한다.
    std::unique_ptr<BIO, BioDeleter> network_bio_;


    // DTLS 서버 SSL. app_data가 이 객체를 가리킨다.
    std::unique_ptr<SSL, SslDeleter> ssl_;


    // offer의 브라우저 인증서 fingerprint(대문자)
    std::string remote_fingerprint_;


    // 현재 상태
    DtlsState state_ = DtlsState::kHandshaking;


    // kFailed가 된 원인
    std::string failure_reason_;


    // 검증 콜백이 fingerprint 불일치 등으로 거부한 원인. 핸드셰이크 실패 원인을 만들 때 우선한다.
    std::string peer_verification_failure_;


    // kConnected가 된 뒤의 SRTP 키 재료. 소멸할 때 OPENSSL_cleanse로 지운다.
    std::optional<SrtpKeyMaterial> srtp_keys_;
};


}  // namespace vms::webrtc
