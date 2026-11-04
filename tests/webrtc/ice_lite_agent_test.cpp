#include "webrtc/ice_lite_agent.h"

#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "core/event_loop.h"
#include "core/network_interfaces.h"
#include "core/unique_fd.h"
#include "crypto/certificate.h"
#include "crypto/dtls_context.h"
#include "dtls_test_client.h"
#include "media/h264_file.h"
#include "media/live_video_source.h"
#include "rtp_test_support.h"
#include "srtcp_test_sender.h"
#include "srtp_test_receiver.h"
#include "test_support.h"
#include "webrtc/dtls_transport.h"
#include "webrtc/stun_message.h"

using vms::core::EventLoop;
using vms::core::UniqueFd;
using vms::crypto::Certificate;
using vms::crypto::DtlsContext;
using vms::media::H264File;
using vms::media::LiveVideoEvent;
using vms::media::LiveVideoFrame;
using vms::media::LiveVideoSource;
using vms::test::DtlsTestClient;
using vms::test::Expect;
using vms::test::AppendSyntheticNalUnit;
using vms::test::MakeSyntheticH264File;
using vms::test::ParseRtpPacket;
using vms::test::ReassembleNalUnits;
using vms::test::ReceivedRtpPacket;
using vms::test::SrtcpTestSender;
using vms::test::SrtpTestReceiver;
using vms::test::TransmittedNalUnits;
using vms::webrtc::DtlsTransport;
using vms::webrtc::BuildBindingRequest;
using vms::webrtc::IceLiteAgent;
using vms::webrtc::IceLiteAgentConfig;
using vms::webrtc::IceSessionParameters;
using vms::webrtc::IceSessionRequest;
using vms::webrtc::IceSessionState;
using vms::webrtc::ParseStunMessage;
using vms::webrtc::RtcpReceiveStats;
using vms::webrtc::StunTransactionId;
using vms::webrtc::VerifyFingerprint;
using vms::webrtc::VerifyMessageIntegrity;

using namespace std::chrono_literals;


namespace {


// DTLS를 쓰지 않는 시나리오에서 세션을 만들 때 넘기는 브라우저 fingerprint
constexpr std::string_view kTestRemoteFingerprint =
    "0E:6A:A6:4D:F1:A2:6F:95:9F:50:48:0E:6D:31:F5:BD:E5:79:D4:25:87:8B:53:45:5E:E1:DA:A8:FB:4E:50:71";


// 응답이 없다고 판단할 때까지 기다리는 시간(밀리초)
constexpr long kNoResponseWaitMilliseconds = 200;


// 여러 데이터그램을 받을 때, 이 시간 동안 더 오지 않으면 응답이 끝났다고 본다.
constexpr std::chrono::milliseconds kQuietPeriod = 100ms;


// 세션 만료 시나리오에서 쓰는 짧은 만료 시간
constexpr std::chrono::milliseconds kShortSessionTimeout = 200ms;


// DTLS 핸드셰이크에서 데이터그램을 주고받는 최대 왕복 수
constexpr int kMaxDtlsRounds = 10;


// 테스트에서 브라우저 역할을 하는 UDP 소켓과 그 주소.
struct TestClient {
    // 127.0.0.1의 임의 포트에 바인드한 소켓
    UniqueFd socket;


    // 바인드한 주소와 포트
    sockaddr_in endpoint{};
};


// 127.0.0.1의 임의 포트에 바인드한 테스트 클라이언트를 만든다.
//
// 반환값: 클라이언트. 소켓 생성이나 바인드에 실패하면 소켓이 비어 있다.
TestClient OpenClient() {
    TestClient client;
    client.socket.Reset(::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    if (::bind(client.socket.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
            0 ||
        ::getsockname(client.socket.Get(), reinterpret_cast<sockaddr*>(&client.endpoint), &size) !=
            0) {
        client.socket.Reset();
    }
    return client;
}


// 서버로 패킷을 보내고, 루프를 돌려 응답이 오거나 대기 시간이 지날 때까지 기다린다.
//
// loop: 에이전트가 등록된 이벤트 루프
// client: 보낼 클라이언트
// server: 서버 candidate 주소
// packet: 보낼 데이터그램
// 반환값: 받은 응답. 대기 시간 안에 오지 않으면 빈 optional
std::optional<std::vector<std::uint8_t>> Exchange(EventLoop& loop, const TestClient& client,
                                                  const sockaddr_in& server,
                                                  std::span<const std::uint8_t> packet) {
    ::sendto(client.socket.Get(), packet.data(), packet.size(), 0,
             reinterpret_cast<const sockaddr*>(&server), sizeof(server));

    // 응답이 없는 경우에도 루프가 끝나도록 단발 타이머를 건다.
    UniqueFd timer(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
    itimerspec timeout{};
    timeout.it_value.tv_nsec = kNoResponseWaitMilliseconds * 1'000'000;
    ::timerfd_settime(timer.Get(), 0, &timeout, nullptr);

    std::optional<std::vector<std::uint8_t>> response;
    loop.Add(client.socket.Get(), EPOLLIN, [&](std::uint32_t) {
        std::array<std::uint8_t, 2048> buffer;
        const ssize_t size = ::recv(client.socket.Get(), buffer.data(), buffer.size(), 0);
        if (size > 0) {
            response.emplace(buffer.begin(), buffer.begin() + size);
            loop.Stop();
        }
    });
    loop.Add(timer.Get(), EPOLLIN, [&](std::uint32_t) { loop.Stop(); });
    loop.Run();
    loop.Remove(client.socket.Get());
    loop.Remove(timer.Get());
    return response;
}


// 테스트에서 쓰는 H.264 payload type
constexpr std::uint8_t kTestPayloadType = 102;


// 동영상 송신 시나리오의 재생 속도. 짧은 대기 안에 여러 프레임을 받도록 높게 잡는다.
constexpr int kTestVideoFps = 100;


// 송신 중 만료 시나리오의 만료 시간. 핸드셰이크(왕복마다 kQuietPeriod 대기)보다 충분히 길게 잡는다.
constexpr std::chrono::milliseconds kVideoSessionTimeout = 500ms;


// 브라우저 ufrag "brow"로 세션을 만든다.
//
// agent: 세션을 만들 에이전트
// remote_fingerprint: 브라우저 인증서 fingerprint
// video: 세션에 지정할 동영상. nullptr이면 동영상 없는 세션
// camera: 세션에 지정할 카메라 소스. nullptr이면 카메라 없는 세션
// 반환값: CreateSession의 결과
std::expected<IceSessionParameters, std::string> CreateTestSession(IceLiteAgent& agent,
                                                                   std::string_view remote_fingerprint,
                                                                   const H264File* video = nullptr,
                                                                   LiveVideoSource* camera = nullptr) {
    return agent.CreateSession(IceSessionRequest{.remote_ufrag = "brow",
                                                 .remote_fingerprint = remote_fingerprint,
                                                 .h264_payload_type = kTestPayloadType,
                                                 .video = video,
                                                 .camera = camera,
                                                 .source_label = camera != nullptr ? "camera test" : "file test.h264"});
}


// MakeSyntheticH264File과 내용이 다른 프레임 2개짜리 합성 동영상을 만든다.
//
// 반환값: SPS·PPS·500바이트 IDR 프레임, 200바이트 슬라이스 프레임
H264File MakeOtherSyntheticH264File() {
    std::vector<std::uint8_t> bytes;
    AppendSyntheticNalUnit(bytes, 0x67, 0x42, 8);
    AppendSyntheticNalUnit(bytes, 0x68, 0xCE, 4);
    AppendSyntheticNalUnit(bytes, 0x65, 0x88, 500);
    AppendSyntheticNalUnit(bytes, 0x41, 0x9A, 200);
    return *H264File::FromBytes(std::move(bytes));
}


// 에이전트의 첫 candidate를 소켓 주소로 바꾼다.
//
// agent: 시작한 에이전트
// 반환값: 첫 candidate의 주소와 포트
sockaddr_in ServerEndpoint(const IceLiteAgent& agent) {
    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_addr = *vms::core::ParseIpv4Address(agent.Candidates().front().address);
    server.sin_port = htons(agent.Candidates().front().port);
    return server;
}


// 서명한 요청에 검증 가능한 응답이 오고, nomination과 이어지는 요청을 처리하는지 검사한다.
void TestRespondsAndRecordsNomination(EventLoop& loop, IceLiteAgent& agent) {
    const auto credentials = CreateTestSession(agent, kTestRemoteFingerprint);
    const TestClient client = OpenClient();
    if (!credentials || !client.socket.IsValid()) {
        Expect(false, "세션과 클라이언트 준비");
        return;
    }
    const sockaddr_in server = ServerEndpoint(agent);
    const std::string username = credentials->ice_ufrag + ":brow";
    const StunTransactionId transaction_id = {1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3};

    const auto request = BuildBindingRequest(transaction_id, username, credentials->ice_pwd, false);
    const auto response = Exchange(loop, client, server, *request);
    const auto message = response ? ParseStunMessage(*response)
                                  : std::unexpected(vms::webrtc::StunError::kNotStunMessage);
    if (!message) {
        Expect(false, "서명한 요청에 응답이 와야 한다");
        return;
    }
    Expect(message->transaction_id == transaction_id, "요청과 같은 트랜잭션 ID");
    Expect(VerifyFingerprint(*message).has_value(), "응답 FINGERPRINT");
    Expect(VerifyMessageIntegrity(*message, credentials->ice_pwd).has_value(),
           "서버 pwd로 서명한 응답");
    Expect(message->xor_mapped_address &&
               message->xor_mapped_address->sin_addr.s_addr == client.endpoint.sin_addr.s_addr &&
               message->xor_mapped_address->sin_port == client.endpoint.sin_port,
           "XOR-MAPPED-ADDRESS가 클라이언트 주소");
    Expect(agent.FindSessionState(credentials->ice_ufrag) == IceSessionState::kWaitingForCheck,
           "USE-CANDIDATE 전에는 대기 상태");

    const auto nomination = BuildBindingRequest({4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4}, username,
                                                credentials->ice_pwd, true);
    Expect(Exchange(loop, client, server, *nomination).has_value(), "nomination 요청에도 응답");
    Expect(agent.FindSessionState(credentials->ice_ufrag) == IceSessionState::kConnected,
           "USE-CANDIDATE 뒤에는 연결됨");

    const auto consent = BuildBindingRequest({5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5}, username,
                                             credentials->ice_pwd, false);
    Expect(Exchange(loop, client, server, *consent).has_value(), "연결 뒤 동의 확인에도 응답");
}


// 틀린 비밀번호, 모르는 ufrag, 브라우저 ufrag 불일치, 임의 바이트에 응답하지 않는지 검사한다.
void TestIgnoresInvalidPackets(EventLoop& loop, IceLiteAgent& agent) {
    const auto credentials = CreateTestSession(agent, kTestRemoteFingerprint);
    const TestClient client = OpenClient();
    if (!credentials || !client.socket.IsValid()) {
        Expect(false, "세션과 클라이언트 준비");
        return;
    }
    const sockaddr_in server = ServerEndpoint(agent);
    const StunTransactionId transaction_id = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};

    const auto wrong_password = BuildBindingRequest(
        transaction_id, credentials->ice_ufrag + ":brow", "wrong-password-wrong-pass", false);
    Expect(!Exchange(loop, client, server, *wrong_password), "틀린 비밀번호에는 응답하지 않음");

    const auto unknown_session =
        BuildBindingRequest(transaction_id, "unknown0:brow", credentials->ice_pwd, false);
    Expect(!Exchange(loop, client, server, *unknown_session), "모르는 ufrag에는 응답하지 않음");

    const auto wrong_remote = BuildBindingRequest(transaction_id, credentials->ice_ufrag + ":other",
                                                  credentials->ice_pwd, false);
    Expect(!Exchange(loop, client, server, *wrong_remote), "브라우저 ufrag 불일치에는 응답하지 않음");

    const std::vector<std::uint8_t> random_bytes = {0x01, 0x99, 0x42, 0x13, 0x77, 0x00, 0x05};
    Expect(!Exchange(loop, client, server, random_bytes), "임의 바이트에는 응답하지 않음");

    const std::vector<std::uint8_t> dtls_like = {22, 0xfe, 0xfd, 0, 0, 0, 0};
    Expect(!Exchange(loop, client, server, dtls_like), "DTLS 패킷은 버림");

    // 잘못된 패킷을 받은 뒤에도 정상 요청은 계속 처리해야 한다.
    const auto valid = BuildBindingRequest(transaction_id, credentials->ice_ufrag + ":brow",
                                           credentials->ice_pwd, false);
    Expect(Exchange(loop, client, server, *valid).has_value(), "잘못된 패킷 뒤에도 정상 응답");
}


// 세션마다 서로 다른 자격 증명과 SSRC를 만들고, 주소가 없으면 시작하지 않는지 검사한다.
void TestCreatesDistinctSessionsAndRejectsEmptyConfig(EventLoop& loop, IceLiteAgent& agent,
                                                      const DtlsContext& dtls_context) {
    const auto first = CreateTestSession(agent, kTestRemoteFingerprint);
    const auto second = CreateTestSession(agent, kTestRemoteFingerprint);
    Expect(first && second && first->ice_ufrag != second->ice_ufrag &&
               first->ice_pwd != second->ice_pwd,
           "세션마다 다른 자격 증명");
    Expect(first && first->ice_ufrag.size() == 8 && first->ice_pwd.size() == 24,
           "ufrag 8자, pwd 24자");
    Expect(first && second && first->ssrc != 0 && second->ssrc != 0 && first->ssrc != second->ssrc,
           "세션마다 0이 아닌 다른 SSRC");
    Expect(!agent.FindSessionState("missing!"), "없는 세션 조회");

    Expect(!IceLiteAgent::Start(loop, IceLiteAgentConfig{.addresses = {}, .port = 0}, dtls_context),
           "주소가 없으면 시작 실패");
}


// 데이터그램이 RTP/RTCP 범위(첫 바이트 128~191)인지 확인한다.
bool IsRtpPacket(std::span<const std::uint8_t> datagram) {
    return !datagram.empty() && datagram[0] >= 128 && datagram[0] <= 191;
}


// 데이터그램을 모두 보내고, 응답이 kQuietPeriod 동안 끊길 때까지 받은 데이터그램을 모은다.
//
// loop: 에이전트가 등록된 이벤트 루프
// client: 보낼 클라이언트
// server: 서버 candidate 주소
// packets: 보낼 데이터그램
// media_packets: 주어지면 받은 RTP 패킷을 여기에 따로 모은다. 동영상은 계속 오므로 대기 시간을 늘리지 않는다.
// 반환값: 받은 순서대로의 데이터그램(RTP 제외). 응답이 없으면 비어 있다.
std::vector<std::vector<std::uint8_t>> SendAndCollect(
    EventLoop& loop, const TestClient& client, const sockaddr_in& server,
    const std::vector<std::vector<std::uint8_t>>& packets,
    std::vector<std::vector<std::uint8_t>>* media_packets = nullptr) {
    for (const auto& packet : packets) {
        ::sendto(client.socket.Get(), packet.data(), packet.size(), 0,
                 reinterpret_cast<const sockaddr*>(&server), sizeof(server));
    }

    // 데이터그램을 받을 때마다 대기 타이머를 다시 걸어, 마지막 수신 뒤 kQuietPeriod가 지나면 멈춘다.
    std::vector<std::vector<std::uint8_t>> received;
    const auto stop = [&loop] { loop.Stop(); };
    EventLoop::TimerId quiet_timer = loop.AddTimer(kQuietPeriod, stop);
    loop.Add(client.socket.Get(), EPOLLIN, [&](std::uint32_t) {
        std::array<std::uint8_t, 4096> buffer;
        ssize_t size = 0;
        bool has_received_non_media = false;
        while ((size = ::recv(client.socket.Get(), buffer.data(), buffer.size(), 0)) > 0) {
            const std::span<const std::uint8_t> datagram(buffer.data(), static_cast<std::size_t>(size));
            if (IsRtpPacket(datagram)) {
                if (media_packets != nullptr) {
                    media_packets->emplace_back(datagram.begin(), datagram.end());
                }
                continue;
            }
            received.emplace_back(datagram.begin(), datagram.end());
            has_received_non_media = true;
        }
        if (has_received_non_media) {
            loop.CancelTimer(quiet_timer);
            quiet_timer = loop.AddTimer(kQuietPeriod, stop);
        }
    });
    loop.Run();
    loop.Remove(client.socket.Get());
    loop.CancelTimer(quiet_timer);
    return received;
}


// 루프를 정해진 시간 동안 돌린다. 그동안 에이전트의 타이머와 패킷 처리가 진행된다.
//
// loop: 돌릴 이벤트 루프
// duration: 돌릴 시간
void RunLoopFor(EventLoop& loop, std::chrono::milliseconds duration) {
    loop.AddTimer(duration, [&loop] { loop.Stop(); });
    loop.Run();
}


// UDP 클라이언트로 USE-CANDIDATE가 있는 서명한 Binding 요청을 보낸다.
//
// loop: 에이전트가 등록된 이벤트 루프
// client: 보낼 클라이언트
// server: 서버 candidate 주소
// local_ufrag, local_pwd: 세션의 서버 자격 증명
// 반환값: 성공 응답을 받았으면 true
bool Nominate(EventLoop& loop, const TestClient& client, const sockaddr_in& server,
              const std::string& local_ufrag, const std::string& local_pwd) {
    const auto request =
        BuildBindingRequest({9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9}, local_ufrag + ":brow", local_pwd, true);
    return request && Exchange(loop, client, server, *request).has_value();
}


// UDP 클라이언트 위에서 테스트 DTLS 클라이언트의 핸드셰이크를 진행한다.
//
// loop: 에이전트가 등록된 이벤트 루프
// client: DTLS 데이터그램을 주고받을 UDP 클라이언트
// server: 서버 candidate 주소
// dtls_client: 테스트 DTLS 클라이언트
// media_packets: 주어지면 핸드셰이크 도중 받은 RTP 패킷을 여기에 모은다.
// 반환값: 첫 ClientHello에 대해 서버가 보낸 데이터그램 수
std::size_t RunDtlsHandshake(EventLoop& loop, const TestClient& client, const sockaddr_in& server,
                             DtlsTestClient& dtls_client,
                             std::vector<std::vector<std::uint8_t>>* media_packets = nullptr) {
    std::size_t first_response_count = 0;
    auto outgoing = dtls_client.Advance();
    for (int round = 0; round < kMaxDtlsRounds && !outgoing.empty(); ++round) {
        const auto responses = SendAndCollect(loop, client, server, outgoing, media_packets);
        if (round == 0) {
            first_response_count = responses.size();
        }
        for (const auto& response : responses) {
            dtls_client.Receive(response);
        }
        outgoing = dtls_client.Advance();
    }
    return first_response_count;
}


// USE-CANDIDATE 뒤 같은 UDP 소켓으로 핸드셰이크를 마치고, 다른 포트의 ClientHello에는 응답하지 않는지
// 검사한다.
void TestCompletesDtlsHandshakeOverUdp(EventLoop& loop, IceLiteAgent& agent,
                                       const Certificate& server_certificate) {
    auto dtls_client = DtlsTestClient::Create(true);
    if (!dtls_client) {
        Expect(false, "DTLS 클라이언트 준비: " + dtls_client.error());
        return;
    }
    const auto credentials = CreateTestSession(agent, dtls_client->Sha256Fingerprint());
    const TestClient client = OpenClient();
    const TestClient other_port_client = OpenClient();
    if (!credentials || !client.socket.IsValid() || !other_port_client.socket.IsValid()) {
        Expect(false, "세션과 클라이언트 준비");
        return;
    }
    const sockaddr_in server = ServerEndpoint(agent);

    Expect(Nominate(loop, client, server, credentials->ice_ufrag, credentials->ice_pwd),
           "USE-CANDIDATE에 응답");
    RunDtlsHandshake(loop, client, server, *dtls_client);
    Expect(dtls_client->CurrentState() == DtlsTestClient::State::kConnected,
           "nominated pair에서 DTLS 핸드셰이크 완료");
    Expect(dtls_client->PeerCertificateFingerprint() == server_certificate.Sha256Fingerprint(),
           "클라이언트가 받은 서버 인증서가 answer의 fingerprint와 같음");
    Expect(dtls_client->SelectedSrtpProfile() == "SRTP_AES128_CM_SHA1_80", "SRTP 프로파일 협상");

    auto other_dtls_client = DtlsTestClient::Create(true);
    if (!other_dtls_client) {
        Expect(false, "두 번째 DTLS 클라이언트 준비");
        return;
    }
    Expect(SendAndCollect(loop, other_port_client, server, other_dtls_client->Advance()).empty(),
           "nominated pair가 아닌 포트의 ClientHello에는 응답하지 않음");
}


// 처음과 다른 주소의 nomination 변경 뒤에도 DTLS는 처음 nomination을 받은 주소에서만 받는지 검사한다.
void TestKeepsFirstNominatedPairForDtls(EventLoop& loop, IceLiteAgent& agent) {
    auto dtls_client = DtlsTestClient::Create(true);
    auto moved_dtls_client = DtlsTestClient::Create(true);
    if (!dtls_client || !moved_dtls_client) {
        Expect(false, "DTLS 클라이언트 준비");
        return;
    }
    const auto credentials = CreateTestSession(agent, dtls_client->Sha256Fingerprint());
    const TestClient first_client = OpenClient();
    const TestClient second_client = OpenClient();
    if (!credentials || !first_client.socket.IsValid() || !second_client.socket.IsValid()) {
        Expect(false, "세션과 클라이언트 준비");
        return;
    }
    const sockaddr_in server = ServerEndpoint(agent);

    Expect(Nominate(loop, first_client, server, credentials->ice_ufrag, credentials->ice_pwd),
           "처음 nomination에 응답");
    Expect(Nominate(loop, second_client, server, credentials->ice_ufrag, credentials->ice_pwd),
           "다른 주소의 nomination 변경에도 성공 응답");
    Expect(SendAndCollect(loop, second_client, server, moved_dtls_client->Advance()).empty(),
           "변경된 주소의 ClientHello에는 응답하지 않음");

    RunDtlsHandshake(loop, first_client, server, *dtls_client);
    Expect(dtls_client->CurrentState() == DtlsTestClient::State::kConnected,
           "처음 nomination을 받은 주소에서는 핸드셰이크 완료");
}


// 요청이 없는 세션은 만료 뒤 서명한 요청에 응답하지 않는지 검사한다.
void TestExpiresIdleSession(EventLoop& loop, IceLiteAgent& agent) {
    const auto credentials = CreateTestSession(agent, kTestRemoteFingerprint);
    const TestClient client = OpenClient();
    if (!credentials || !client.socket.IsValid()) {
        Expect(false, "세션과 클라이언트 준비");
        return;
    }

    RunLoopFor(loop, kShortSessionTimeout + 100ms);
    Expect(!agent.FindSessionState(credentials->ice_ufrag), "만료된 세션은 제거됨");
    const auto request = BuildBindingRequest({6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
                                             credentials->ice_ufrag + ":brow", credentials->ice_pwd,
                                             false);
    Expect(!Exchange(loop, client, ServerEndpoint(agent), *request),
           "만료된 세션의 자격 증명으로 서명한 요청에는 응답하지 않음");
}


// 만료 시간보다 짧은 간격으로 동의 확인을 보내면 만료 시간이 여러 번 지나도 응답하는지 검사한다.
void TestKeepsSessionWithConsentChecks(EventLoop& loop, IceLiteAgent& agent) {
    const auto credentials = CreateTestSession(agent, kTestRemoteFingerprint);
    const TestClient client = OpenClient();
    if (!credentials || !client.socket.IsValid()) {
        Expect(false, "세션과 클라이언트 준비");
        return;
    }
    const sockaddr_in server = ServerEndpoint(agent);

    const auto started_at = std::chrono::steady_clock::now();
    int response_count = 0;
    while (std::chrono::steady_clock::now() - started_at < 500ms) {
        RunLoopFor(loop, 100ms);
        const auto consent = BuildBindingRequest({3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3},
                                                 credentials->ice_ufrag + ":brow",
                                                 credentials->ice_pwd, false);
        if (Exchange(loop, client, server, *consent)) {
            ++response_count;
        } else {
            Expect(false, "100ms마다 보낸 동의 확인에 응답해야 한다");
            return;
        }
    }
    Expect(response_count >= 4, "500ms 동안 동의 확인에 계속 응답");
    Expect(agent.FindSessionState(credentials->ice_ufrag).has_value(), "세션 유지");
}


// 핸드셰이크를 마친 세션이 만료되면 같은 주소의 DTLS 패킷에 응답하지 않는지 검사한다.
void TestExpiredSessionDropsDtls(EventLoop& loop, IceLiteAgent& agent) {
    auto dtls_client = DtlsTestClient::Create(true);
    auto next_dtls_client = DtlsTestClient::Create(true);
    if (!dtls_client || !next_dtls_client) {
        Expect(false, "DTLS 클라이언트 준비");
        return;
    }
    const auto credentials = CreateTestSession(agent, dtls_client->Sha256Fingerprint());
    const TestClient client = OpenClient();
    if (!credentials || !client.socket.IsValid()) {
        Expect(false, "세션과 클라이언트 준비");
        return;
    }
    const sockaddr_in server = ServerEndpoint(agent);

    Expect(Nominate(loop, client, server, credentials->ice_ufrag, credentials->ice_pwd),
           "USE-CANDIDATE에 응답");
    RunDtlsHandshake(loop, client, server, *dtls_client);
    Expect(dtls_client->CurrentState() == DtlsTestClient::State::kConnected, "만료 전 핸드셰이크 완료");

    RunLoopFor(loop, kShortSessionTimeout + 100ms);
    Expect(!agent.FindSessionState(credentials->ice_ufrag), "핸드셰이크를 마친 세션도 만료됨");
    Expect(SendAndCollect(loop, client, server, next_dtls_client->Advance()).empty(),
           "만료된 세션의 주소에서 온 DTLS 패킷에는 응답하지 않음");
}


// 소켓에 쌓인 데이터그램을 모두 읽는다. 루프에 등록하지 않은 소켓에 쓴다.
//
// client: 읽을 클라이언트
// 반환값: 받은 순서대로의 데이터그램
std::vector<std::vector<std::uint8_t>> DrainSocket(const TestClient& client) {
    std::vector<std::vector<std::uint8_t>> received;
    std::array<std::uint8_t, 4096> buffer;
    ssize_t size = 0;
    while ((size = ::recv(client.socket.Get(), buffer.data(), buffer.size(), 0)) > 0) {
        received.emplace_back(buffer.begin(), buffer.begin() + size);
    }
    return received;
}


// RTP 패킷만 고른다.
//
// datagrams: 받은 데이터그램
// 반환값: 첫 바이트가 RTP 범위인 데이터그램
std::vector<std::vector<std::uint8_t>> FilterRtpPackets(std::vector<std::vector<std::uint8_t>> datagrams) {
    std::erase_if(datagrams, [](const auto& datagram) { return !IsRtpPacket(datagram); });
    return datagrams;
}


// 동영상 송신을 받는 테스트 브라우저 하나.
struct MediaReceiver {
    // DTLS 클라이언트. close_notify를 보낼 때 쓴다.
    DtlsTestClient dtls_client;


    // UDP 소켓
    TestClient client;


    // 세션의 서버 값(자격 증명, SSRC)
    IceSessionParameters session;


    // 핸드셰이크 도중과 이후에 받은 SRTP 패킷
    std::vector<std::vector<std::uint8_t>> srtp_packets;
};


// 세션을 만들고 nomination과 DTLS 핸드셰이크를 마친 수신 측을 만든다.
//
// loop: 에이전트가 등록된 이벤트 루프
// agent: 세션을 만들 에이전트
// video: 세션에 지정할 동영상. nullptr이면 동영상 없는 세션
// camera: 세션에 지정할 카메라 소스. nullptr이면 카메라 없는 세션
// 반환값: 핸드셰이크를 마친 수신 측. 준비에 실패하면 빈 optional
std::optional<MediaReceiver> ConnectMediaReceiver(EventLoop& loop, IceLiteAgent& agent, const H264File* video,
                                                  LiveVideoSource* camera = nullptr) {
    auto dtls_client = DtlsTestClient::Create(true);
    if (!dtls_client) {
        return std::nullopt;
    }
    auto session = CreateTestSession(agent, dtls_client->Sha256Fingerprint(), video, camera);
    TestClient client = OpenClient();
    if (!session || !client.socket.IsValid()) {
        return std::nullopt;
    }
    MediaReceiver receiver{.dtls_client = *std::move(dtls_client),
                           .client = std::move(client),
                           .session = *std::move(session),
                           .srtp_packets = {}};
    const sockaddr_in server = ServerEndpoint(agent);
    if (!Nominate(loop, receiver.client, server, receiver.session.ice_ufrag, receiver.session.ice_pwd)) {
        return std::nullopt;
    }
    RunDtlsHandshake(loop, receiver.client, server, receiver.dtls_client, &receiver.srtp_packets);
    if (receiver.dtls_client.CurrentState() != DtlsTestClient::State::kConnected) {
        return std::nullopt;
    }
    return receiver;
}


// 수신 측이 받은 SRTP 패킷을 서버용 키로 복호화해 프레임별로 나눈다.
//
// receiver: 패킷을 받은 수신 측
// 반환값: 프레임마다의 RTP 패킷 목록(marker 비트로 구분). 키를 꺼내지 못하거나 복호화·해석에 실패하면 빈 optional
std::optional<std::vector<std::vector<ReceivedRtpPacket>>> DecryptFrames(const MediaReceiver& receiver) {
    // 키 재료는 클라이언트 키(16), 서버 키(16), 클라이언트 솔트(14), 서버 솔트(14) 순서다.
    const auto keying_material = receiver.dtls_client.ExportSrtpKeyingMaterial();
    if (!keying_material) {
        return std::nullopt;
    }
    const std::span<const std::uint8_t, 60> material(*keying_material);
    auto srtp_receiver = SrtpTestReceiver::Create(material.subspan<16, 16>(), material.subspan<46, 14>());
    if (!srtp_receiver) {
        return std::nullopt;
    }

    std::vector<std::vector<ReceivedRtpPacket>> frames(1);
    for (const auto& srtp_packet : receiver.srtp_packets) {
        const auto rtp_packet = srtp_receiver->Unprotect(srtp_packet);
        auto parsed = rtp_packet ? ParseRtpPacket(*rtp_packet) : std::nullopt;
        if (!parsed) {
            return std::nullopt;
        }
        const bool is_frame_end = parsed->marker;
        frames.back().push_back(*std::move(parsed));
        if (is_frame_end) {
            frames.emplace_back();
        }
    }
    frames.pop_back();
    return frames;
}


// 핸드셰이크 뒤 받은 패킷이 세션의 SSRC·payload type을 쓰고, 순번이 연속되며, 첫 5프레임이 소스 0,1,2,0,1과
// 같은지 검사한다.
void TestStreamsVideoAfterHandshake(EventLoop& loop, IceLiteAgent& agent, const H264File& video) {
    auto receiver = ConnectMediaReceiver(loop, agent, &video);
    if (!receiver) {
        Expect(false, "동영상 수신 측 연결");
        return;
    }
    RunLoopFor(loop, 150ms);
    std::ranges::move(FilterRtpPackets(DrainSocket(receiver->client)), std::back_inserter(receiver->srtp_packets));

    const auto frames = DecryptFrames(*receiver);
    if (!frames || frames->size() < 5) {
        Expect(false, "서버용 키로 복호화한 프레임이 5개 이상이어야 한다");
        return;
    }
    std::optional<std::uint16_t> previous_sequence_number;
    for (const auto& frame : *frames) {
        for (const ReceivedRtpPacket& packet : frame) {
            Expect(packet.ssrc == receiver->session.ssrc, "SSRC는 CreateSession의 값");
            Expect(packet.payload_type == kTestPayloadType, "payload type은 세션 요청의 값");
            if (previous_sequence_number) {
                Expect(packet.sequence_number == static_cast<std::uint16_t>(*previous_sequence_number + 1),
                       "순번은 빠짐없이 이어져야 한다");
            }
            previous_sequence_number = packet.sequence_number;
        }
    }
    for (std::size_t frame_number = 0; frame_number < 5; ++frame_number) {
        std::vector<std::vector<std::uint8_t>> payloads;
        for (const ReceivedRtpPacket& packet : (*frames)[frame_number]) {
            payloads.push_back(packet.payload);
        }
        Expect(ReassembleNalUnits(payloads) == TransmittedNalUnits(video.Frames()[frame_number % 3]),
               "재조립한 프레임은 소스 0,1,2,0,1과 같아야 한다");
    }
}


// 두 세션이 동시에 받고, 한 세션의 close_notify 뒤에는 그 세션만 패킷이 끊기는지 검사한다.
void TestStopsOnlyClosedSession(EventLoop& loop, IceLiteAgent& agent, const H264File& video) {
    auto closing = ConnectMediaReceiver(loop, agent, &video);
    auto continuing = ConnectMediaReceiver(loop, agent, &video);
    if (!closing || !continuing) {
        Expect(false, "두 수신 측 연결");
        return;
    }
    RunLoopFor(loop, 100ms);
    Expect(!FilterRtpPackets(DrainSocket(closing->client)).empty() &&
               !FilterRtpPackets(DrainSocket(continuing->client)).empty(),
           "두 세션 모두 패킷을 받아야 한다");

    const sockaddr_in server = ServerEndpoint(agent);
    for (const auto& datagram : closing->dtls_client.SendCloseNotify()) {
        ::sendto(closing->client.socket.Get(), datagram.data(), datagram.size(), 0,
                 reinterpret_cast<const sockaddr*>(&server), sizeof(server));
    }

    // close_notify가 처리되기 전에 보낸 패킷을 비운 뒤, 그 뒤로 오는 패킷이 있는지 본다.
    RunLoopFor(loop, 50ms);
    DrainSocket(closing->client);
    DrainSocket(continuing->client);
    RunLoopFor(loop, 200ms);
    Expect(FilterRtpPackets(DrainSocket(closing->client)).empty(), "close_notify 뒤에는 패킷이 오지 않아야 한다");
    Expect(!FilterRtpPackets(DrainSocket(continuing->client)).empty(), "다른 세션은 계속 받아야 한다");
}


// 동영상이 지정되지 않은 세션은 핸드셰이크 뒤에도 RTP를 받지 않는지 검사한다.
void TestNoMediaWithoutVideo(EventLoop& loop, IceLiteAgent& agent) {
    auto receiver = ConnectMediaReceiver(loop, agent, nullptr);
    if (!receiver) {
        Expect(false, "동영상 없는 세션의 수신 측 연결");
        return;
    }
    RunLoopFor(loop, 100ms);
    Expect(receiver->srtp_packets.empty() && FilterRtpPackets(DrainSocket(receiver->client)).empty(),
           "동영상이 없는 세션에는 RTP를 보내지 않아야 한다");
}


// 같은 에이전트의 세션마다 지정한 동영상을 보내고, 동영상이 없는 세션에는 보내지 않는지 검사한다.
void TestStreamsVideoPerSession(EventLoop& loop, IceLiteAgent& agent, const H264File& first_video,
                                const H264File& second_video) {
    auto first = ConnectMediaReceiver(loop, agent, &first_video);
    auto second = ConnectMediaReceiver(loop, agent, &second_video);
    auto without_video = ConnectMediaReceiver(loop, agent, nullptr);
    if (!first || !second || !without_video) {
        Expect(false, "세션별 동영상 수신 측 연결");
        return;
    }
    RunLoopFor(loop, 100ms);
    std::ranges::move(FilterRtpPackets(DrainSocket(first->client)), std::back_inserter(first->srtp_packets));
    std::ranges::move(FilterRtpPackets(DrainSocket(second->client)), std::back_inserter(second->srtp_packets));

    // 각 세션의 첫 프레임이 자기 동영상의 첫 프레임인지 본다. 두 동영상의 IDR 크기가 달라 구분된다.
    const auto first_frames = DecryptFrames(*first);
    const auto second_frames = DecryptFrames(*second);
    if (!first_frames || first_frames->empty() || !second_frames || second_frames->empty()) {
        Expect(false, "두 세션 모두 프레임을 받아야 한다");
        return;
    }
    const auto reassemble = [](const std::vector<ReceivedRtpPacket>& frame) {
        std::vector<std::vector<std::uint8_t>> payloads;
        for (const ReceivedRtpPacket& packet : frame) {
            payloads.push_back(packet.payload);
        }
        return ReassembleNalUnits(payloads);
    };
    Expect(reassemble(first_frames->front()) == TransmittedNalUnits(first_video.Frames().front()),
           "첫 세션은 자기 동영상의 첫 프레임부터 받는다");
    Expect(reassemble(second_frames->front()) == TransmittedNalUnits(second_video.Frames().front()),
           "둘째 세션은 다른 동영상의 첫 프레임부터 받는다");
    Expect(without_video->srtp_packets.empty() && FilterRtpPackets(DrainSocket(without_video->client)).empty(),
           "같은 에이전트라도 동영상 없는 세션에는 보내지 않는다");
}


// 송신 중인 세션이 만료되면 패킷이 더 오지 않는지 검사한다.
//
// agent: 만료 시간이 kVideoSessionTimeout인 동영상 에이전트
void TestStopsWhenSessionExpires(EventLoop& loop, IceLiteAgent& agent, const H264File& video) {
    auto receiver = ConnectMediaReceiver(loop, agent, &video);
    if (!receiver) {
        Expect(false, "만료 시나리오 수신 측 연결");
        return;
    }

    // 핸드셰이크 동안 만료 시간 일부가 지나므로, 동의 확인으로 세션을 살린 뒤 송신 중인지 확인한다.
    const sockaddr_in server = ServerEndpoint(agent);
    const auto consent = BuildBindingRequest({8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8},
                                             receiver->session.ice_ufrag + ":brow", receiver->session.ice_pwd,
                                             false);
    ::sendto(receiver->client.socket.Get(), consent->data(), consent->size(), 0,
             reinterpret_cast<const sockaddr*>(&server), sizeof(server));
    RunLoopFor(loop, 50ms);
    Expect(!FilterRtpPackets(DrainSocket(receiver->client)).empty(), "만료 전에는 패킷을 받아야 한다");

    RunLoopFor(loop, kVideoSessionTimeout + 100ms);
    Expect(!agent.FindSessionState(receiver->session.ice_ufrag), "동의 확인이 없으면 세션이 만료됨");
    DrainSocket(receiver->client);
    RunLoopFor(loop, 100ms);
    Expect(FilterRtpPackets(DrainSocket(receiver->client)).empty(), "만료 뒤에는 패킷이 오지 않아야 한다");
}


// 보고 블록 하나인 RR(보낸 쪽 SSRC 1) 뒤에 CNAME SDES가 이어진 RTCP 복합 패킷을 만든다.
//
// target_ssrc: 보고 블록의 대상 SSRC. 서버 세션의 SSRC를 넘긴다.
// 반환값: RR 32바이트와 SDES 24바이트로 된 복합 패킷. 손실률 3, 누적 손실 7, 최고 순번 70123, jitter 12
std::vector<std::uint8_t> BuildReceiverReportCompound(std::uint32_t target_ssrc) {
    std::vector<std::uint8_t> compound = {0x81, 0xC9, 0x00, 0x07, 0x00, 0x00, 0x00, 0x01};
    for (int shift = 24; shift >= 0; shift -= 8) {
        compound.push_back(static_cast<std::uint8_t>(target_ssrc >> shift));
    }
    compound.insert(compound.end(), {0x03, 0x00, 0x00, 0x07, 0x00, 0x01, 0x11, 0xEB, 0x00, 0x00, 0x00, 0x0C,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    compound.insert(compound.end(), {0x81, 0xCA, 0x00, 0x05, 0x00, 0x00, 0x00, 0x01, 0x01, 0x0A, 'b', 'r',
                                     'o', 'w', 's', 'e', 'r', '0', '0', '1', 0x00, 0x00, 0x00, 0x00});
    return compound;
}


// 수신 측의 DTLS 키 재료로 SRTCP 테스트 송신 측을 만든다.
//
// receiver: 핸드셰이크를 마친 수신 측
// use_client_key: true이면 브라우저 쪽(client) 키, false이면 서버 쪽(server) 키를 쓴다.
// 반환값: 송신 측. 키를 꺼내지 못하거나 유도에 실패하면 빈 optional
std::optional<SrtcpTestSender> CreateSrtcpSender(const MediaReceiver& receiver, bool use_client_key) {
    // 키 재료는 클라이언트 키(16), 서버 키(16), 클라이언트 솔트(14), 서버 솔트(14) 순서다.
    const auto keying_material = receiver.dtls_client.ExportSrtpKeyingMaterial();
    if (!keying_material) {
        return std::nullopt;
    }
    const std::span<const std::uint8_t, 60> material(*keying_material);
    auto sender = use_client_key ? SrtcpTestSender::Create(material.subspan<0, 16>(), material.subspan<32, 14>())
                                 : SrtcpTestSender::Create(material.subspan<16, 16>(), material.subspan<46, 14>());
    if (!sender) {
        return std::nullopt;
    }
    return *std::move(sender);
}


// 데이터그램을 서버로 보내고 에이전트가 처리할 시간 동안 루프를 돌린다. 응답은 기다리지 않는다.
//
// loop: 에이전트가 등록된 이벤트 루프
// client: 보낼 클라이언트
// server: 서버 candidate 주소
// datagrams: 보낼 데이터그램
void SendWithoutResponse(EventLoop& loop, const TestClient& client, const sockaddr_in& server,
                         const std::vector<std::vector<std::uint8_t>>& datagrams) {
    for (const auto& datagram : datagrams) {
        ::sendto(client.socket.Get(), datagram.data(), datagram.size(), 0,
                 reinterpret_cast<const sockaddr*>(&server), sizeof(server));
    }
    RunLoopFor(loop, 30ms);
}


// 두 수신 결과의 개수가 모두 같은지 확인한다.
//
// left, right: 비교할 수신 결과
// 반환값: 복합 패킷 수, 종류별 수, 원인별 실패 수가 모두 같으면 true
bool HaveSameCounts(const RtcpReceiveStats& left, const RtcpReceiveStats& right) {
    return left.compound_packet_count == right.compound_packet_count &&
           left.packet_count_by_type == right.packet_count_by_type && left.too_short_count == right.too_short_count &&
           left.authentication_failure_count == right.authentication_failure_count &&
           left.replayed_count == right.replayed_count && left.too_many_ssrcs_count == right.too_many_ssrcs_count &&
           left.crypto_failure_count == right.crypto_failure_count && left.malformed_count == right.malformed_count;
}


// DTLS가 완료된 세션이 브라우저 키로 보호한 RTCP를 해석하고, 다른 키·재전송·다른 주소·RTP를 구분하는지
// 검사한다.
void TestReceivesRtcpAfterHandshake(EventLoop& loop, IceLiteAgent& agent) {
    auto receiver = ConnectMediaReceiver(loop, agent, nullptr);
    const TestClient other_client = OpenClient();
    if (!receiver || !other_client.socket.IsValid()) {
        Expect(false, "RTCP 수신 측 연결");
        return;
    }
    auto browser_sender = CreateSrtcpSender(*receiver, true);
    auto server_key_sender = CreateSrtcpSender(*receiver, false);
    if (!browser_sender || !server_key_sender) {
        Expect(false, "SRTCP 송신 측 준비");
        return;
    }
    const std::string& local_ufrag = receiver->session.ice_ufrag;
    const sockaddr_in server = ServerEndpoint(agent);
    const std::vector<std::uint8_t> compound = BuildReceiverReportCompound(receiver->session.ssrc);

    const auto first_packet = browser_sender->Protect(compound, true);
    SendWithoutResponse(loop, receiver->client, server, {*first_packet});
    auto stats = agent.FindRtcpReceiveStats(local_ufrag);
    if (!stats) {
        Expect(false, "세션의 RTCP 수신 결과 조회");
        return;
    }
    Expect(stats->compound_packet_count == 1, "RR+SDES 하나는 복합 패킷 1개");
    Expect(stats->packet_count_by_type[201] == 1 && stats->packet_count_by_type[202] == 1, "RR 1개, SDES 1개");
    const auto& block = stats->last_receiver_report_block;
    Expect(block && block->source_ssrc == receiver->session.ssrc && block->fraction_lost == 3 &&
               block->cumulative_lost == 7 && block->extended_highest_sequence_number == 70123 && block->jitter == 12,
           "마지막 보고 블록 값이 보낸 값과 같음");

    // 서버 쪽 키로 보호한 패킷은 브라우저가 보낸 것이 아니므로 모두 인증에 실패한다. 재생 확인이 인증보다
    // 먼저이므로, 아직 받지 않은 인덱스(100~104)를 써서 인증 단계까지 가게 한다.
    std::vector<std::vector<std::uint8_t>> wrong_key_packets;
    for (std::uint32_t index = 100; index < 105; ++index) {
        wrong_key_packets.push_back(*server_key_sender->Protect(compound, true, index));
    }
    SendWithoutResponse(loop, receiver->client, server, wrong_key_packets);
    SendWithoutResponse(loop, receiver->client, server, {*browser_sender->Protect(compound, true)});
    stats = agent.FindRtcpReceiveStats(local_ufrag);
    Expect(stats->authentication_failure_count == 5, "다른 키로 보호한 패킷 5개는 인증 실패 5");
    Expect(stats->detailed_failure_log_count == 3, "상세 로그는 처음 3개만");
    Expect(stats->compound_packet_count == 2 && stats->packet_count_by_type[201] == 2,
           "인증 실패 뒤 올바른 패킷은 정상 집계");

    SendWithoutResponse(loop, receiver->client, server, {*first_packet});
    stats = agent.FindRtcpReceiveStats(local_ufrag);
    Expect(stats->replayed_count == 1 && stats->compound_packet_count == 2, "같은 패킷 재전송은 재생 1");

    const RtcpReceiveStats before = *stats;
    SendWithoutResponse(loop, other_client, server, {*browser_sender->Protect(compound, true)});
    Expect(HaveSameCounts(before, *agent.FindRtcpReceiveStats(local_ufrag)),
           "nominated pair가 아닌 주소의 RTCP는 통계를 바꾸지 않음");

    // 둘째 바이트 102는 RTP payload type이다. 브라우저 키로 보호하더라도 RTCP로 처리하지 않는다.
    std::vector<std::uint8_t> rtp_packet = *browser_sender->Protect(compound, true);
    rtp_packet[0] = 0x80;
    rtp_packet[1] = 102;
    SendWithoutResponse(loop, receiver->client, server, {rtp_packet});
    Expect(HaveSameCounts(before, *agent.FindRtcpReceiveStats(local_ufrag)), "둘째 바이트가 102인 RTP 패킷은 버림");
    Expect(agent.FindSessionState(local_ufrag) == IceSessionState::kConnected, "버린 패킷 뒤에도 세션은 연결됨");
}


// nomination은 받았지만 DTLS가 완료되지 않은 세션은 RTCP를 처리하지 않는지 검사한다.
void TestDropsRtcpBeforeDtls(EventLoop& loop, IceLiteAgent& agent) {
    const auto credentials = CreateTestSession(agent, kTestRemoteFingerprint);
    const TestClient client = OpenClient();
    auto sender = SrtcpTestSender::Create(std::array<std::uint8_t, 16>{}, std::array<std::uint8_t, 14>{});
    if (!credentials || !client.socket.IsValid() || !sender) {
        Expect(false, "세션과 클라이언트 준비");
        return;
    }
    const sockaddr_in server = ServerEndpoint(agent);
    Expect(Nominate(loop, client, server, credentials->ice_ufrag, credentials->ice_pwd), "USE-CANDIDATE에 응답");

    SendWithoutResponse(loop, client, server, {*sender->Protect(BuildReceiverReportCompound(credentials->ssrc), true)});
    const auto stats = agent.FindRtcpReceiveStats(credentials->ice_ufrag);
    Expect(stats && HaveSameCounts(*stats, RtcpReceiveStats{}), "DTLS 완료 전의 RTCP는 통계를 바꾸지 않음");
}


// 동영상을 보내는 세션에서 RTCP 실패 패킷을 받은 뒤에도 RTP 송신이 계속되는지 검사한다.
void TestVideoContinuesAfterRtcpFailures(EventLoop& loop, IceLiteAgent& agent, const H264File& video) {
    auto receiver = ConnectMediaReceiver(loop, agent, &video);
    if (!receiver) {
        Expect(false, "동영상 수신 측 연결");
        return;
    }
    auto server_key_sender = CreateSrtcpSender(*receiver, false);
    if (!server_key_sender) {
        Expect(false, "SRTCP 송신 측 준비");
        return;
    }
    const sockaddr_in server = ServerEndpoint(agent);
    const std::vector<std::uint8_t> compound = BuildReceiverReportCompound(receiver->session.ssrc);
    SendWithoutResponse(loop, receiver->client, server,
                        {*server_key_sender->Protect(compound, true), {0x81, 0xC9, 0x00, 0x01}});
    const auto stats = agent.FindRtcpReceiveStats(receiver->session.ice_ufrag);
    Expect(stats && stats->authentication_failure_count == 1 && stats->too_short_count == 1,
           "인증 실패와 길이 부족을 각각 셈");

    DrainSocket(receiver->client);
    RunLoopFor(loop, 100ms);
    Expect(!FilterRtpPackets(DrainSocket(receiver->client)).empty(), "실패 패킷 뒤에도 RTP를 계속 받음");
}




// 카메라 세션 시나리오의 짧은 카메라 대기 시간
constexpr std::chrono::milliseconds kShortCameraWaitTimeout = 300ms;


// 카메라 세션 시나리오용 에이전트를 시작한다.
//
// loop: 에이전트를 등록할 루프
// dtls_context: DTLS 컨텍스트
// 반환값: 카메라 대기 시간이 짧은 에이전트. 시작에 실패하면 nullptr
//
// 세션이 카메라 소스를 구독하므로, 소스를 먼저 만들고 에이전트를 나중에 만들어 먼저 소멸하게 한다.
std::unique_ptr<IceLiteAgent> StartCameraAgent(EventLoop& loop, const DtlsContext& dtls_context) {
    auto agent = IceLiteAgent::Start(loop,
                                     IceLiteAgentConfig{.addresses = {*vms::core::ParseIpv4Address("127.0.0.1")},
                                                        .port = 0,
                                                        .camera_wait_timeout = kShortCameraWaitTimeout},
                                     dtls_context);
    return agent ? *std::move(agent) : nullptr;
}


// 합성 동영상의 프레임을 카메라 프레임처럼 소스에 넣는다.
//
// source: 프레임을 넣을 소스
// video: 합성 동영상. 0번 프레임만 키프레임이다.
// frame_index: 넣을 프레임 위치
// camera_timestamp: 카메라 RTP 타임스탬프
void PublishCameraFrame(LiveVideoSource& source, const H264File& video, std::size_t frame_index,
                        std::uint32_t camera_timestamp) {
    source.PublishFrame(LiveVideoFrame{.frame = video.Frames()[frame_index],
                                       .rtp_timestamp = camera_timestamp,
                                       .is_keyframe = frame_index == 0});
}


// 수신 측 소켓에 쌓인 SRTP 패킷을 모아 프레임으로 복호화한다.
//
// receiver: 수신 측
// 반환값: 지금까지 받은 프레임. 복호화에 실패하면 빈 optional
std::optional<std::vector<std::vector<ReceivedRtpPacket>>> CollectFrames(MediaReceiver& receiver) {
    std::ranges::move(FilterRtpPackets(DrainSocket(receiver.client)), std::back_inserter(receiver.srtp_packets));
    return DecryptFrames(receiver);
}


// 프레임의 RTP 페이로드를 NAL 목록으로 재조립한다.
//
// frame: 프레임 하나의 RTP 패킷
// 반환값: 재조립한 NAL 목록
std::optional<std::vector<std::vector<std::uint8_t>>> ReassembleFrame(const std::vector<ReceivedRtpPacket>& frame) {
    std::vector<std::vector<std::uint8_t>> payloads;
    for (const ReceivedRtpPacket& packet : frame) {
        payloads.push_back(packet.payload);
    }
    return ReassembleNalUnits(payloads);
}


// DTLS 완료 뒤 키프레임부터 받고, 같은 소스를 두 세션이 함께 받으며, 카메라 타임스탬프 간격이 유지되는지 검사한다.
void TestStreamsCameraFromKeyframe(EventLoop& loop, const DtlsContext& dtls_context) {
    const H264File video = MakeSyntheticH264File();
    LiveVideoSource camera;
    camera.PublishEvent(LiveVideoEvent::kConnected);
    auto agent = StartCameraAgent(loop, dtls_context);
    auto first = agent ? ConnectMediaReceiver(loop, *agent, nullptr, &camera) : std::nullopt;
    auto second = agent ? ConnectMediaReceiver(loop, *agent, nullptr, &camera) : std::nullopt;
    if (!first || !second) {
        Expect(false, "카메라 수신 측 둘 연결");
        return;
    }
    Expect(camera.SubscriberCount() == 2, "DTLS 완료 뒤 두 세션이 소스를 구독");

    PublishCameraFrame(camera, video, 1, 1000);
    PublishCameraFrame(camera, video, 0, 4000);
    PublishCameraFrame(camera, video, 1, 7000);
    PublishCameraFrame(camera, video, 2, 10000);
    for (MediaReceiver* receiver : {&*first, &*second}) {
        const auto frames = CollectFrames(*receiver);
        if (!frames || frames->size() != 3) {
            Expect(false, "키프레임부터 세 프레임을 받아야 한다");
            continue;
        }
        Expect(ReassembleFrame((*frames)[0]) == TransmittedNalUnits(video.Frames()[0]), "첫 프레임은 키프레임");
        Expect(ReassembleFrame((*frames)[2]) == TransmittedNalUnits(video.Frames()[2]), "이후 프레임은 도착 순서대로");
        Expect(static_cast<std::uint32_t>((*frames)[1].front().timestamp - (*frames)[0].front().timestamp) == 3000 &&
                   static_cast<std::uint32_t>((*frames)[2].front().timestamp - (*frames)[1].front().timestamp) == 3000,
               "카메라 타임스탬프 간격 유지");
        Expect((*frames)[0].front().ssrc == receiver->session.ssrc, "SSRC는 세션 값");
    }
}


// 카메라가 끊겼다가 대기 시간 안에 다시 연결되면 세션이 남고 다음 키프레임부터 이어서 받는지 검사한다.
void TestCameraReconnectsWithinWait(EventLoop& loop, const DtlsContext& dtls_context) {
    const H264File video = MakeSyntheticH264File();
    LiveVideoSource camera;
    camera.PublishEvent(LiveVideoEvent::kConnected);
    auto agent = StartCameraAgent(loop, dtls_context);
    auto receiver = agent ? ConnectMediaReceiver(loop, *agent, nullptr, &camera) : std::nullopt;
    if (!receiver) {
        Expect(false, "카메라 수신 측 연결");
        return;
    }
    PublishCameraFrame(camera, video, 0, 3000);

    camera.PublishEvent(LiveVideoEvent::kDisconnected);
    RunLoopFor(loop, kShortCameraWaitTimeout / 2);
    camera.PublishEvent(LiveVideoEvent::kConnected);
    RunLoopFor(loop, kShortCameraWaitTimeout);
    Expect(agent->FindSessionState(receiver->session.ice_ufrag).has_value(), "대기 시간 안에 연결되면 세션 유지");

    PublishCameraFrame(camera, video, 1, 999);
    PublishCameraFrame(camera, video, 0, 1000);
    const auto frames = CollectFrames(*receiver);
    if (!frames || frames->size() != 2) {
        Expect(false, "재연결 뒤 키프레임부터 이어서 받아야 한다");
        return;
    }
    Expect(ReassembleFrame((*frames)[1]) == TransmittedNalUnits(video.Frames()[0]), "재연결 뒤 첫 프레임은 키프레임");
    Expect((*frames)[1].front().sequence_number == static_cast<std::uint16_t>((*frames)[0].back().sequence_number + 1),
           "순번은 이어짐");
    const auto skipped = static_cast<std::uint32_t>((*frames)[1].front().timestamp - (*frames)[0].front().timestamp);
    Expect(skipped >= 90000 * 4 / 10, "타임스탬프는 끊긴 시간만큼 건너뜀");
}


// 카메라가 대기 시간 동안 돌아오지 않으면 close_notify를 보내고 세션을 제거하며 구독을 해지하는지 검사한다.
void TestCameraWaitTimeout(EventLoop& loop, const DtlsContext& dtls_context) {
    const H264File video = MakeSyntheticH264File();
    LiveVideoSource camera;
    camera.PublishEvent(LiveVideoEvent::kConnected);
    auto agent = StartCameraAgent(loop, dtls_context);
    auto receiver = agent ? ConnectMediaReceiver(loop, *agent, nullptr, &camera) : std::nullopt;
    if (!receiver) {
        Expect(false, "카메라 수신 측 연결");
        return;
    }
    PublishCameraFrame(camera, video, 0, 3000);
    DrainSocket(receiver->client);

    camera.PublishEvent(LiveVideoEvent::kDisconnected);
    RunLoopFor(loop, kShortCameraWaitTimeout + 100ms);
    Expect(!agent->FindSessionState(receiver->session.ice_ufrag), "대기 시간이 지나면 세션 제거");
    Expect(camera.SubscriberCount() == 0, "세션 제거 뒤 구독 해지");

    for (const auto& datagram : DrainSocket(receiver->client)) {
        if (!IsRtpPacket(datagram)) {
            receiver->dtls_client.Receive(datagram);
        }
    }
    receiver->dtls_client.Advance();
    Expect(receiver->dtls_client.CurrentState() == DtlsTestClient::State::kClosed, "브라우저가 close_notify를 받음");

    camera.PublishEvent(LiveVideoEvent::kConnected);
    PublishCameraFrame(camera, video, 0, 6000);
    Expect(FilterRtpPackets(DrainSocket(receiver->client)).empty(), "제거된 세션에는 보내지 않음");
}


// 끊긴 카메라로 DTLS가 완료되면 대기를 시작해, 연결되지 않으면 세션을 제거하는지 검사한다.
void TestCameraDisconnectedAtHandshake(EventLoop& loop, const DtlsContext& dtls_context) {
    LiveVideoSource camera;
    auto agent = StartCameraAgent(loop, dtls_context);
    auto receiver = agent ? ConnectMediaReceiver(loop, *agent, nullptr, &camera) : std::nullopt;
    if (!receiver) {
        Expect(false, "카메라 수신 측 연결");
        return;
    }
    Expect(agent->FindSessionState(receiver->session.ice_ufrag).has_value() && camera.SubscriberCount() == 1,
           "끊긴 카메라여도 DTLS 완료 뒤 세션은 유지하고 구독");
    RunLoopFor(loop, kShortCameraWaitTimeout + 100ms);
    Expect(!agent->FindSessionState(receiver->session.ice_ufrag), "카메라가 오지 않으면 대기 시간 뒤 세션 제거");
    Expect(camera.SubscriberCount() == 0, "세션 제거 뒤 구독 해지");
}


}  // namespace


int main() {
    const auto certificate = Certificate::Generate();
    const auto dtls_context =
        certificate ? DtlsContext::Create(*certificate, DtlsTransport::VerifyPeerCertificate)
                    : std::unexpected(certificate.error());
    auto loop = EventLoop::Create();
    if (!dtls_context || !loop) {
        Expect(false, "DTLS 컨텍스트와 루프 생성");
        return vms::test::ExitCode();
    }

    const in_addr loopback = *vms::core::ParseIpv4Address("127.0.0.1");
    auto agent = IceLiteAgent::Start(*loop, IceLiteAgentConfig{.addresses = {loopback}, .port = 0},
                                     *dtls_context);
    if (!agent) {
        Expect(false, "에이전트 시작: " + agent.error());
        return vms::test::ExitCode();
    }
    Expect((*agent)->Candidates().size() == 1 && (*agent)->Candidates().front().port != 0,
           "실제 포트를 담은 candidate");
    Expect((*agent)->Candidates().front().priority == 2130706431u, "첫 주소의 우선순위");

    TestRespondsAndRecordsNomination(*loop, **agent);
    TestIgnoresInvalidPackets(*loop, **agent);
    TestCreatesDistinctSessionsAndRejectsEmptyConfig(*loop, **agent, *dtls_context);
    TestCompletesDtlsHandshakeOverUdp(*loop, **agent, *certificate);
    TestKeepsFirstNominatedPairForDtls(*loop, **agent);
    TestNoMediaWithoutVideo(*loop, **agent);
    TestReceivesRtcpAfterHandshake(*loop, **agent);
    TestDropsRtcpBeforeDtls(*loop, **agent);

    // 동영상 송신 시나리오는 합성 동영상을 세션에 지정하고 높은 fps로 보내는 에이전트로 검사한다.
    const H264File video = MakeSyntheticH264File();
    const H264File other_video = MakeOtherSyntheticH264File();
    auto video_agent = IceLiteAgent::Start(
        *loop, IceLiteAgentConfig{.addresses = {loopback}, .port = 0, .video_fps = kTestVideoFps}, *dtls_context);
    if (!video_agent) {
        Expect(false, "동영상 에이전트 시작: " + video_agent.error());
        return vms::test::ExitCode();
    }
    TestStreamsVideoAfterHandshake(*loop, **video_agent, video);
    TestStopsOnlyClosedSession(*loop, **video_agent, video);
    TestStreamsVideoPerSession(*loop, **video_agent, video, other_video);
    TestVideoContinuesAfterRtcpFailures(*loop, **video_agent, video);

    // 세션 만료 시나리오는 만료 시간을 짧게 둔 별도 에이전트로 검사한다.
    auto short_timeout_agent = IceLiteAgent::Start(
        *loop,
        IceLiteAgentConfig{
            .addresses = {loopback}, .port = 0, .session_timeout = kShortSessionTimeout},
        *dtls_context);
    if (!short_timeout_agent) {
        Expect(false, "만료 시간이 짧은 에이전트 시작: " + short_timeout_agent.error());
        return vms::test::ExitCode();
    }
    TestExpiresIdleSession(*loop, **short_timeout_agent);
    TestKeepsSessionWithConsentChecks(*loop, **short_timeout_agent);
    TestExpiredSessionDropsDtls(*loop, **short_timeout_agent);

    auto short_timeout_video_agent = IceLiteAgent::Start(*loop,
                                                         IceLiteAgentConfig{.addresses = {loopback},
                                                                            .port = 0,
                                                                            .session_timeout = kVideoSessionTimeout,
                                                                            .video_fps = kTestVideoFps},
                                                         *dtls_context);
    if (!short_timeout_video_agent) {
        Expect(false, "만료 시간이 짧은 동영상 에이전트 시작: " + short_timeout_video_agent.error());
        return vms::test::ExitCode();
    }
    TestStopsWhenSessionExpires(*loop, **short_timeout_video_agent, video);

    TestStreamsCameraFromKeyframe(*loop, *dtls_context);
    TestCameraReconnectsWithinWait(*loop, *dtls_context);
    TestCameraWaitTimeout(*loop, *dtls_context);
    TestCameraDisconnectedAtHandshake(*loop, *dtls_context);

    // 에이전트는 루프, DTLS 컨텍스트, 동영상보다 먼저 소멸해야 한다.
    short_timeout_video_agent->reset();
    short_timeout_agent->reset();
    video_agent->reset();
    agent->reset();
    return vms::test::ExitCode();
}
