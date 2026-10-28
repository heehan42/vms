#include "webrtc/dtls_transport.h"

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/srtp.h>

#include <algorithm>
#include <format>
#include <utility>

#include "crypto/certificate.h"
#include "crypto/openssl_error.h"


namespace vms::webrtc {


namespace {


// DTLS가 핸드셰이크 메시지를 조각낼 데이터그램 최대 크기.
//
// datagram pair BIO는 경로 MTU를 알 수 없으므로 직접 정한다. 터널·VPN·IPv6 헤더로 경로 MTU가
// 줄어든 환경에서도 지나가도록 WebRTC 구현들이 흔히 쓰는 값이다.
constexpr long kLinkMtu = 1200;


// SRTP 키 재료를 꺼낼 때 쓰는 RFC 5764의 레이블
constexpr std::string_view kSrtpExporterLabel = "EXTRACTOR-dtls_srtp";


// 레코드를 읽어 버릴 때 쓰는 버퍼 크기. 데이터그램 하나보다 크다.
constexpr std::size_t kReadBufferSize = 4096;


}  // namespace


std::expected<std::unique_ptr<DtlsTransport>, std::string> DtlsTransport::Create(
    const crypto::DtlsContext& context, std::string remote_fingerprint) {
    ::ERR_clear_error();
    std::unique_ptr<SSL, SslDeleter> ssl(::SSL_new(context.Get()));
    if (ssl == nullptr) {
        return std::unexpected(crypto::TakeOpenSslError("DTLS SSL 생성"));
    }

    BIO* ssl_bio = nullptr;
    BIO* network_bio = nullptr;
    if (::BIO_new_bio_dgram_pair(&ssl_bio, 0, &network_bio, 0) != 1) {
        return std::unexpected(crypto::TakeOpenSslError("datagram pair BIO 생성"));
    }
    std::unique_ptr<BIO, BioDeleter> owned_network_bio(network_bio);
    // 읽기와 쓰기에 같은 BIO를 넘기면 SSL이 그 참조 하나를 소유한다.
    ::SSL_set_bio(ssl.get(), ssl_bio, ssl_bio);
    ::SSL_set_accept_state(ssl.get());
    ::SSL_set_options(ssl.get(), SSL_OP_NO_QUERY_MTU);
    ::DTLS_set_link_mtu(ssl.get(), kLinkMtu);

    std::unique_ptr<DtlsTransport> transport(new DtlsTransport(
        std::move(ssl), std::move(owned_network_bio), std::move(remote_fingerprint)));
    SSL_set_app_data(transport->ssl_.get(), transport.get());
    return transport;
}


int DtlsTransport::VerifyPeerCertificate(int, X509_STORE_CTX* store) {
    auto* const ssl = static_cast<SSL*>(
        ::X509_STORE_CTX_get_ex_data(store, ::SSL_get_ex_data_X509_STORE_CTX_idx()));
    auto* const transport =
        ssl != nullptr ? static_cast<DtlsTransport*>(SSL_get_app_data(ssl)) : nullptr;
    if (transport == nullptr) {
        return 0;
    }

    // 자체 서명 인증서이므로 인증 기관 검증 결과는 보지 않는다. 브라우저는 인증서 하나만 보내지만,
    // 체인이 붙어 와도 신원은 깊이 0인 인증서의 fingerprint로만 정한다.
    if (::X509_STORE_CTX_get_error_depth(store) != 0) {
        return 1;
    }
    const auto fingerprint =
        crypto::ComputeSha256Fingerprint(::X509_STORE_CTX_get_current_cert(store));
    if (!fingerprint) {
        transport->peer_verification_failure_ = fingerprint.error();
        return 0;
    }
    if (*fingerprint != transport->remote_fingerprint_) {
        transport->peer_verification_failure_ =
            std::format("브라우저 인증서 fingerprint 불일치 (받음 {}, offer {})", *fingerprint,
                        transport->remote_fingerprint_);
        return 0;
    }
    return 1;
}


DtlsTransport::DtlsTransport(std::unique_ptr<SSL, SslDeleter> ssl,
                             std::unique_ptr<BIO, BioDeleter> network_bio,
                             std::string remote_fingerprint)
    : network_bio_(std::move(network_bio)),
      ssl_(std::move(ssl)),
      remote_fingerprint_(std::move(remote_fingerprint)) {}


DtlsTransport::~DtlsTransport() {
    if (srtp_keys_) {
        ::OPENSSL_cleanse(&*srtp_keys_, sizeof(*srtp_keys_));
    }
}


std::vector<Datagram> DtlsTransport::HandleDatagram(std::span<const std::uint8_t> datagram) {
    if (state_ == DtlsState::kFailed || state_ == DtlsState::kClosed) {
        return {};
    }

    // 다른 세션의 남은 오류가 이 세션의 실패 원인에 섞이지 않도록 비우고 시작한다.
    ::ERR_clear_error();
    if (::BIO_write(network_bio_.get(), datagram.data(), static_cast<int>(datagram.size())) <= 0) {
        // 처리하지 않은 데이터그램이 BIO에 남는 일은 없으므로 실제로는 생기지 않는다.
        // 생기더라도 브라우저가 재전송하므로 이 데이터그램만 버린다.
        ::ERR_clear_error();
        return {};
    }

    if (state_ == DtlsState::kHandshaking) {
        ContinueHandshake();
    } else {
        ReadRecords();
    }
    return DrainOutput();
}


std::vector<Datagram> DtlsTransport::HandleTimeout() {
    if (state_ != DtlsState::kHandshaking) {
        return {};
    }

    ::ERR_clear_error();
    // DTLSv1_handle_timeout은 기한이 지났을 때만 마지막으로 보낸 flight를 network BIO에 다시 쓰고 다음
    // 간격을 두 배로 늘린다. 기한 전이면 아무것도 쓰지 않으므로 DrainOutput()은 빈 목록을 돌려준다.
    if (::DTLSv1_handle_timeout(ssl_.get()) < 0) {
        Fail(TakeFailureCause("DTLS 재전송"));
    }
    return DrainOutput();
}


std::optional<std::chrono::milliseconds> DtlsTransport::NextTimeout() const {
    if (state_ == DtlsState::kFailed || state_ == DtlsState::kClosed) {
        return std::nullopt;
    }

    timeval remaining{};
    if (::DTLSv1_get_timeout(ssl_.get(), &remaining) != 1) {
        return std::nullopt;
    }
    const auto remaining_duration =
        std::chrono::seconds(remaining.tv_sec) + std::chrono::microseconds(remaining.tv_usec);
    return std::chrono::ceil<std::chrono::milliseconds>(remaining_duration);
}


std::string_view DtlsTransport::SelectedSrtpProfileName() const {
    if (state_ != DtlsState::kConnected) {
        return {};
    }
    const SRTP_PROTECTION_PROFILE* const profile = ::SSL_get_selected_srtp_profile(ssl_.get());
    return profile != nullptr ? profile->name : std::string_view();
}


void DtlsTransport::ContinueHandshake() {
    const int result = ::SSL_do_handshake(ssl_.get());
    if (result == 1) {
        CompleteHandshake();
        return;
    }

    // WANT_READ는 브라우저의 다음 메시지를 기다린다는 뜻이다.
    if (::SSL_get_error(ssl_.get(), result) != SSL_ERROR_WANT_READ) {
        Fail(TakeFailureCause("DTLS 핸드셰이크"));
    }
}


void DtlsTransport::ReadRecords() {
    std::array<std::uint8_t, kReadBufferSize> buffer;
    while (true) {
        // SSL_read는 network BIO에 넣은 데이터그램에서 레코드를 차례로 처리한다. 핸드셰이크와 경고
        // 레코드는 내부에서 처리하고, 응용 데이터 레코드를 만나면 그 하나만 복호화해 buffer에 담고 길이를
        // 반환한다. 그래서 Flight 5 재전송처럼 핸드셰이크 레코드만 있으면 양수 없이 바로 WANT_READ로
        // 끝나고, SCTP 패킷을 담은 응용 데이터 레코드가 여러 개 있으면 호출마다 하나씩 양수를 반환한다.
        const int result = ::SSL_read(ssl_.get(), buffer.data(), static_cast<int>(buffer.size()));
        if (result > 0) {
            // buffer의 앞 result 바이트는 복호화된 SCTP 패킷이다. 넘길 SCTP 스택이 아직 없어 버린다.
            continue;
        }

        const int error = ::SSL_get_error(ssl_.get(), result);
        // WANT_READ는 받은 데이터그램의 레코드를 모두 처리해 다음 데이터그램을 기다린다는 뜻이다.
        if (error == SSL_ERROR_WANT_READ) {
            return;
        }
        if (error == SSL_ERROR_ZERO_RETURN) {
            state_ = DtlsState::kClosed;
            return;
        }
        Fail(TakeFailureCause("DTLS 레코드 처리"));
        return;
    }
}


void DtlsTransport::CompleteHandshake() {
    // OpenSSL은 use_srtp 협상이 없어도 핸드셰이크를 성공으로 끝내므로 여기서 직접 확인한다.
    // 이미 쓴 ChangeCipherSpec·Finished를 보내면 브라우저는 연결된 것으로 보므로 버린다.
    const SRTP_PROTECTION_PROFILE* const profile = ::SSL_get_selected_srtp_profile(ssl_.get());
    if (profile == nullptr || profile->id != SRTP_AES128_CM_SHA1_80) {
        DrainOutput();
        Fail("SRTP 프로파일 SRTP_AES128_CM_SHA1_80이 협상되지 않음");
        return;
    }

    std::array<std::uint8_t, 60> material{};
    if (::SSL_export_keying_material(ssl_.get(), material.data(), material.size(),
                                     kSrtpExporterLabel.data(), kSrtpExporterLabel.size(), nullptr,
                                     0, 0) != 1) {
        DrainOutput();
        Fail(crypto::TakeOpenSslError("SRTP 키 재료 추출"));
        return;
    }

    // RFC 5764 순서: client 키, server 키, client 솔트, server 솔트. client가 브라우저다.
    SrtpKeyMaterial& keys = srtp_keys_.emplace();
    std::span<const std::uint8_t> remaining(material);
    const auto take = [&remaining](std::span<std::uint8_t> destination) {
        std::ranges::copy(remaining.first(destination.size()), destination.begin());
        remaining = remaining.subspan(destination.size());
    };
    take(keys.remote_key);
    take(keys.local_key);
    take(keys.remote_salt);
    take(keys.local_salt);
    ::OPENSSL_cleanse(material.data(), material.size());

    state_ = DtlsState::kConnected;
}


void DtlsTransport::Fail(std::string reason) {
    state_ = DtlsState::kFailed;
    failure_reason_ = std::move(reason);
}


std::string DtlsTransport::TakeFailureCause(std::string_view operation) {
    if (peer_verification_failure_.empty()) {
        return crypto::TakeOpenSslError(operation);
    }
    ::ERR_clear_error();
    return peer_verification_failure_;
}


std::vector<Datagram> DtlsTransport::DrainOutput() {
    std::vector<Datagram> datagrams;
    std::array<std::uint8_t, kReadBufferSize> buffer;
    while (true) {
        const int size =
            ::BIO_read(network_bio_.get(), buffer.data(), static_cast<int>(buffer.size()));
        if (size <= 0) {
            break;
        }
        datagrams.emplace_back(buffer.begin(), buffer.begin() + size);
    }
    // 읽을 데이터그램이 없으면 BIO_read가 재시도 오류를 남기므로 비운다.
    ::ERR_clear_error();
    return datagrams;
}


}  // namespace vms::webrtc
