#include "dtls_test_client.h"

#include <openssl/err.h>
#include <openssl/srtp.h>

#include <utility>

#include "crypto/openssl_error.h"


namespace vms::test {


namespace {


// 서버와 같은 링크 MTU. 데이터그램 크기를 실제 서버와 같은 조건으로 맞춘다.
constexpr long kLinkMtu = 1200;


// SRTP 키 재료를 꺼낼 때 쓰는 RFC 5764의 레이블
constexpr std::string_view kSrtpExporterLabel = "EXTRACTOR-dtls_srtp";


// 서버 인증서를 검증 없이 받아들이는 콜백. 서버 인증서는 테스트가 fingerprint로 직접 비교한다.
int AcceptAnyCertificate(int, X509_STORE_CTX*) { return 1; }


}  // namespace


std::expected<DtlsTestClient, std::string> DtlsTestClient::Create(bool offers_srtp) {
    auto certificate = crypto::Certificate::Generate();
    if (!certificate) {
        return std::unexpected(certificate.error());
    }

    const std::unique_ptr<SSL_CTX, decltype(&::SSL_CTX_free)> context(
        ::SSL_CTX_new(::DTLS_client_method()), &::SSL_CTX_free);
    if (context == nullptr ||
        ::SSL_CTX_use_certificate(context.get(), certificate->X509Certificate()) != 1 ||
        ::SSL_CTX_use_PrivateKey(context.get(), certificate->PrivateKey()) != 1 ||
        ::SSL_CTX_set_max_proto_version(context.get(), DTLS1_2_VERSION) != 1) {
        return std::unexpected(crypto::TakeOpenSslError("테스트 클라이언트 SSL_CTX 설정"));
    }
    if (offers_srtp && ::SSL_CTX_set_tlsext_use_srtp(context.get(), "SRTP_AES128_CM_SHA1_80") != 0) {
        return std::unexpected(crypto::TakeOpenSslError("테스트 클라이언트 SRTP 설정"));
    }
    ::SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, AcceptAnyCertificate);

    // SSL은 SSL_CTX의 참조 횟수를 올리므로 context는 이 함수가 끝나면 놓아도 된다.
    std::unique_ptr<SSL, SslDeleter> ssl(::SSL_new(context.get()));
    BIO* ssl_bio = nullptr;
    BIO* network_bio = nullptr;
    if (ssl == nullptr || ::BIO_new_bio_dgram_pair(&ssl_bio, 0, &network_bio, 0) != 1) {
        return std::unexpected(crypto::TakeOpenSslError("테스트 클라이언트 SSL 생성"));
    }
    std::unique_ptr<BIO, BioDeleter> owned_network_bio(network_bio);
    ::SSL_set_bio(ssl.get(), ssl_bio, ssl_bio);
    ::SSL_set_connect_state(ssl.get());
    ::SSL_set_options(ssl.get(), SSL_OP_NO_QUERY_MTU);
    ::DTLS_set_link_mtu(ssl.get(), kLinkMtu);

    return DtlsTestClient(*std::move(certificate), std::move(owned_network_bio), std::move(ssl));
}


DtlsTestClient::DtlsTestClient(crypto::Certificate certificate,
                               std::unique_ptr<BIO, BioDeleter> network_bio,
                               std::unique_ptr<SSL, SslDeleter> ssl)
    : certificate_(std::move(certificate)),
      network_bio_(std::move(network_bio)),
      ssl_(std::move(ssl)) {}


void DtlsTestClient::Receive(std::span<const std::uint8_t> datagram) {
    ::BIO_write(network_bio_.get(), datagram.data(), static_cast<int>(datagram.size()));
}


std::vector<std::vector<std::uint8_t>> DtlsTestClient::Advance() {
    ::ERR_clear_error();
    if (state_ == State::kHandshaking) {
        const int result = ::SSL_do_handshake(ssl_.get());
        if (result == 1) {
            state_ = State::kConnected;
        } else if (::SSL_get_error(ssl_.get(), result) != SSL_ERROR_WANT_READ) {
            state_ = State::kFailed;
        }
    } else if (state_ == State::kConnected) {
        std::array<std::uint8_t, 2048> buffer;
        while (true) {
            const int result = ::SSL_read(ssl_.get(), buffer.data(), static_cast<int>(buffer.size()));
            if (result > 0) {
                continue;
            }
            if (::SSL_get_error(ssl_.get(), result) != SSL_ERROR_WANT_READ) {
                state_ = State::kFailed;
            }
            break;
        }
    }
    ::ERR_clear_error();
    return DrainOutput();
}


std::vector<std::vector<std::uint8_t>> DtlsTestClient::SendCloseNotify() {
    ::SSL_shutdown(ssl_.get());
    ::ERR_clear_error();
    return DrainOutput();
}


std::optional<std::string> DtlsTestClient::SelectedSrtpProfile() const {
    const SRTP_PROTECTION_PROFILE* const profile = ::SSL_get_selected_srtp_profile(ssl_.get());
    if (profile == nullptr) {
        return std::nullopt;
    }
    return std::string(profile->name);
}


std::optional<std::array<std::uint8_t, 60>> DtlsTestClient::ExportSrtpKeyingMaterial() const {
    std::array<std::uint8_t, 60> material{};
    if (::SSL_export_keying_material(ssl_.get(), material.data(), material.size(),
                                     kSrtpExporterLabel.data(), kSrtpExporterLabel.size(), nullptr,
                                     0, 0) != 1) {
        ::ERR_clear_error();
        return std::nullopt;
    }
    return material;
}


std::optional<std::string> DtlsTestClient::PeerCertificateFingerprint() const {
    const std::unique_ptr<X509, decltype(&::X509_free)> peer(::SSL_get1_peer_certificate(ssl_.get()),
                                                             &::X509_free);
    if (peer == nullptr) {
        return std::nullopt;
    }
    auto fingerprint = crypto::ComputeSha256Fingerprint(peer.get());
    if (!fingerprint) {
        return std::nullopt;
    }
    return *std::move(fingerprint);
}


std::vector<std::vector<std::uint8_t>> DtlsTestClient::DrainOutput() {
    std::vector<std::vector<std::uint8_t>> datagrams;
    std::array<std::uint8_t, 4096> buffer;
    while (true) {
        const int size = ::BIO_read(network_bio_.get(), buffer.data(), static_cast<int>(buffer.size()));
        if (size <= 0) {
            break;
        }
        datagrams.emplace_back(buffer.begin(), buffer.begin() + size);
    }
    return datagrams;
}


}  // namespace vms::test
