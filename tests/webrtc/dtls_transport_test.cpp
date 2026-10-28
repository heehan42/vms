#include "webrtc/dtls_transport.h"

#include <array>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "crypto/certificate.h"
#include "crypto/dtls_context.h"
#include "dtls_test_client.h"
#include "test_support.h"

using vms::crypto::Certificate;
using vms::crypto::DtlsContext;
using vms::test::DtlsTestClient;
using vms::test::Expect;
using vms::webrtc::Datagram;
using vms::webrtc::DtlsState;
using vms::webrtc::DtlsTransport;

using namespace std::chrono_literals;


namespace {


// 데이터그램을 주고받는 최대 왕복 수. DTLS 1.2 핸드셰이크는 두 번의 왕복으로 끝난다.
constexpr int kMaxRounds = 10;


// 클라이언트가 보낼 데이터그램이 없어질 때까지 서버와 클라이언트 사이에서 데이터그램을 전달한다.
//
// server: 서버 쪽 DTLS 상태
// client: 테스트 클라이언트
// from_client: 처음 서버에 넣을 클라이언트 데이터그램
void Relay(DtlsTransport& server, DtlsTestClient& client, std::vector<Datagram> from_client) {
    for (int round = 0; round < kMaxRounds && !from_client.empty(); ++round) {
        for (const Datagram& datagram : from_client) {
            for (const Datagram& response : server.HandleDatagram(datagram)) {
                client.Receive(response);
            }
        }
        from_client = client.Advance();
    }
}


// 클라이언트의 ClientHello부터 시작해 핸드셰이크를 끝까지 진행한다.
//
// server: 서버 쪽 DTLS 상태
// client: 테스트 클라이언트
void RunHandshake(DtlsTransport& server, DtlsTestClient& client) {
    Relay(server, client, client.Advance());
}


// 정상 핸드셰이크에서 양쪽 프로파일, 키 재료, 서버 인증서 fingerprint가 맞는지 검사한다.
void TestCompletesHandshake(const Certificate& certificate, const DtlsContext& context) {
    auto client = DtlsTestClient::Create(true);
    auto server = client ? DtlsTransport::Create(context, client->Sha256Fingerprint())
                         : std::unexpected(client.error());
    if (!server) {
        Expect(false, "클라이언트와 서버 준비: " + server.error());
        return;
    }

    RunHandshake(**server, *client);
    Expect((*server)->State() == DtlsState::kConnected, "서버 kConnected");
    Expect(client->CurrentState() == DtlsTestClient::State::kConnected, "클라이언트 핸드셰이크 완료");
    Expect((*server)->SelectedSrtpProfileName() == "SRTP_AES128_CM_SHA1_80", "서버 프로파일");
    Expect(client->SelectedSrtpProfile() == "SRTP_AES128_CM_SHA1_80", "클라이언트 프로파일");
    Expect(client->PeerCertificateFingerprint() == certificate.Sha256Fingerprint(),
           "클라이언트가 받은 서버 인증서가 answer의 fingerprint와 같음");
    Expect(!(*server)->NextTimeout(), "완료 후 재전송 타이머 없음");

    // 클라이언트(브라우저) 키, 서버 키, 클라이언트 솔트, 서버 솔트 순서다.
    const auto material = client->ExportSrtpKeyingMaterial();
    const auto& keys = (*server)->SrtpKeys();
    if (!material || !keys) {
        Expect(false, "양쪽 키 재료가 있어야 한다");
        return;
    }
    Expect(std::equal(keys->remote_key.begin(), keys->remote_key.end(), material->begin()),
           "브라우저 키");
    Expect(std::equal(keys->local_key.begin(), keys->local_key.end(), material->begin() + 16),
           "서버 키");
    Expect(std::equal(keys->remote_salt.begin(), keys->remote_salt.end(), material->begin() + 32),
           "브라우저 솔트");
    Expect(std::equal(keys->local_salt.begin(), keys->local_salt.end(), material->begin() + 46),
           "서버 솔트");
}


// offer와 다른 인증서로 핸드셰이크하면 양쪽이 실패하고, 이어서 다른 세션은 정상 완료되는지 검사한다.
void TestRejectsFingerprintMismatch(const Certificate& certificate, const DtlsContext& context) {
    auto client = DtlsTestClient::Create(true);
    // offer에 서버 인증서의 fingerprint가 적힌 것처럼 꾸며 클라이언트 인증서와 다르게 만든다.
    auto server = client ? DtlsTransport::Create(context, certificate.Sha256Fingerprint())
                         : std::unexpected(client.error());
    if (!server) {
        Expect(false, "클라이언트와 서버 준비: " + server.error());
        return;
    }

    RunHandshake(**server, *client);
    Expect((*server)->State() == DtlsState::kFailed, "fingerprint 불일치면 서버 kFailed");
    Expect((*server)->FailureReason().contains("fingerprint 불일치"),
           "실패 원인: " + (*server)->FailureReason());
    Expect(client->CurrentState() == DtlsTestClient::State::kFailed,
           "클라이언트도 경고를 받아 핸드셰이크 오류");
    Expect(!(*server)->SrtpKeys(), "실패하면 키 재료 없음");

    auto next_client = DtlsTestClient::Create(true);
    auto next_server = next_client ? DtlsTransport::Create(context, next_client->Sha256Fingerprint())
                                   : std::unexpected(next_client.error());
    if (!next_server) {
        Expect(false, "두 번째 세션 준비: " + next_server.error());
        return;
    }
    RunHandshake(**next_server, *next_client);
    Expect((*next_server)->State() == DtlsState::kConnected, "실패한 세션 뒤 다른 세션은 완료");
}


// use_srtp를 제안하지 않은 클라이언트와의 핸드셰이크를 실패로 처리하는지 검사한다.
void TestRejectsClientWithoutSrtp(const DtlsContext& context) {
    auto client = DtlsTestClient::Create(false);
    auto server = client ? DtlsTransport::Create(context, client->Sha256Fingerprint())
                         : std::unexpected(client.error());
    if (!server) {
        Expect(false, "클라이언트와 서버 준비: " + server.error());
        return;
    }

    RunHandshake(**server, *client);
    Expect((*server)->State() == DtlsState::kFailed, "SRTP 미협상이면 kFailed");
    Expect((*server)->FailureReason().contains("SRTP"), "실패 원인: " + (*server)->FailureReason());
    Expect(client->CurrentState() != DtlsTestClient::State::kConnected,
           "서버의 Flight 6을 보내지 않아 클라이언트는 완료되지 않음");
}


// 서버의 Flight 4(ServerHello~ServerHelloDone)를 버리면 약 1초 뒤 재전송으로 핸드셰이크를 마치는지 검사한다.
void TestRetransmitsLostFlight(const DtlsContext& context) {
    auto client = DtlsTestClient::Create(true);
    auto server = client ? DtlsTransport::Create(context, client->Sha256Fingerprint())
                         : std::unexpected(client.error());
    if (!server) {
        Expect(false, "클라이언트와 서버 준비: " + server.error());
        return;
    }

    bool has_dropped_flight = false;
    for (const Datagram& client_hello : client->Advance()) {
        has_dropped_flight = !(*server)->HandleDatagram(client_hello).empty() || has_dropped_flight;
    }
    Expect(has_dropped_flight, "ClientHello에 대한 Flight 4가 있어야 한다");
    Expect((*server)->HandleTimeout().empty(), "기한 전에는 재전송하지 않음");

    const auto timeout = (*server)->NextTimeout();
    if (!timeout) {
        Expect(false, "Flight 4를 보낸 뒤 재전송 타이머가 있어야 한다");
        return;
    }
    Expect(*timeout > 900ms && *timeout <= 1000ms, "첫 재전송 기한은 약 1초");

    std::this_thread::sleep_for(*timeout);
    const std::vector<Datagram> retransmitted = (*server)->HandleTimeout();
    // OpenSSL은 처음에는 메시지 여러 개를 데이터그램 하나에 담지만, 재전송할 때는 메시지마다 나눠
    // 보낼 수 있으므로 데이터그램 수는 비교하지 않는다. 같은 메시지인지는 핸드셰이크 완료로 확인한다.
    Expect(!retransmitted.empty(), "기한이 지나면 Flight 4를 재전송");
    for (const Datagram& datagram : retransmitted) {
        client->Receive(datagram);
    }
    Relay(**server, *client, client->Advance());

    Expect((*server)->State() == DtlsState::kConnected, "재전송으로 핸드셰이크 완료");
    Expect(client->CurrentState() == DtlsTestClient::State::kConnected, "클라이언트도 완료");
    Expect(!(*server)->NextTimeout(), "완료 후 재전송 타이머 없음");
}


// close_notify를 받으면 kClosed가 되고 이후 데이터그램에는 출력이 없는지 검사한다.
void TestClosesOnCloseNotify(const DtlsContext& context) {
    auto client = DtlsTestClient::Create(true);
    auto server = client ? DtlsTransport::Create(context, client->Sha256Fingerprint())
                         : std::unexpected(client.error());
    if (!server) {
        Expect(false, "클라이언트와 서버 준비: " + server.error());
        return;
    }
    RunHandshake(**server, *client);
    if ((*server)->State() != DtlsState::kConnected) {
        Expect(false, "close_notify 전 핸드셰이크 완료");
        return;
    }

    const std::vector<Datagram> close_notify = client->SendCloseNotify();
    Expect(!close_notify.empty(), "클라이언트가 close_notify를 보냄");
    for (const Datagram& datagram : close_notify) {
        (*server)->HandleDatagram(datagram);
    }
    Expect((*server)->State() == DtlsState::kClosed, "close_notify 뒤 kClosed");
    Expect(close_notify.empty() || (*server)->HandleDatagram(close_notify.front()).empty(),
           "닫힌 뒤 데이터그램에는 출력 없음");
    Expect(!(*server)->NextTimeout(), "닫힌 뒤 재전송 타이머 없음");
}


}  // namespace


int main() {
    const auto certificate = Certificate::Generate();
    const auto context =
        certificate ? DtlsContext::Create(*certificate, DtlsTransport::VerifyPeerCertificate)
                    : std::unexpected(certificate.error());
    if (!context) {
        Expect(false, "서버 인증서와 DTLS 컨텍스트: " + context.error());
        return vms::test::ExitCode();
    }

    TestCompletesHandshake(*certificate, *context);
    TestRejectsFingerprintMismatch(*certificate, *context);
    TestRejectsClientWithoutSrtp(*context);
    TestRetransmitsLostFlight(*context);
    TestClosesOnCloseNotify(*context);
    return vms::test::ExitCode();
}
