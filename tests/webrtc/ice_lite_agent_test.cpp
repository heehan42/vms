#include "webrtc/ice_lite_agent.h"

#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "core/event_loop.h"
#include "core/network_interfaces.h"
#include "core/unique_fd.h"
#include "test_support.h"
#include "webrtc/stun_message.h"

using vms::core::EventLoop;
using vms::core::UniqueFd;
using vms::test::Expect;
using vms::webrtc::BuildBindingRequest;
using vms::webrtc::IceLiteAgent;
using vms::webrtc::IceLiteAgentConfig;
using vms::webrtc::IceSessionState;
using vms::webrtc::ParseStunMessage;
using vms::webrtc::StunTransactionId;
using vms::webrtc::VerifyFingerprint;
using vms::webrtc::VerifyMessageIntegrity;


namespace {


// 응답이 없다고 판단할 때까지 기다리는 시간(밀리초)
constexpr long kNoResponseWaitMilliseconds = 200;


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


// 서명한 요청에 검증 가능한 응답이 오고, 지명과 이어지는 요청을 처리하는지 검사한다.
void TestRespondsAndRecordsNomination(EventLoop& loop, IceLiteAgent& agent) {
    const auto credentials = agent.CreateSession("brow");
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
    Expect(Exchange(loop, client, server, *nomination).has_value(), "지명 요청에도 응답");
    Expect(agent.FindSessionState(credentials->ice_ufrag) == IceSessionState::kConnected,
           "USE-CANDIDATE 뒤에는 연결됨");

    const auto consent = BuildBindingRequest({5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5}, username,
                                             credentials->ice_pwd, false);
    Expect(Exchange(loop, client, server, *consent).has_value(), "연결 뒤 동의 확인에도 응답");
}


// 틀린 비밀번호, 모르는 ufrag, 브라우저 ufrag 불일치, 임의 바이트에 응답하지 않는지 검사한다.
void TestIgnoresInvalidPackets(EventLoop& loop, IceLiteAgent& agent) {
    const auto credentials = agent.CreateSession("brow");
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


// 세션마다 서로 다른 자격 증명을 만들고, 주소가 없으면 시작하지 않는지 검사한다.
void TestCreatesDistinctSessionsAndRejectsEmptyConfig(EventLoop& loop, IceLiteAgent& agent) {
    const auto first = agent.CreateSession("brow");
    const auto second = agent.CreateSession("brow");
    Expect(first && second && first->ice_ufrag != second->ice_ufrag &&
               first->ice_pwd != second->ice_pwd,
           "세션마다 다른 자격 증명");
    Expect(first && first->ice_ufrag.size() == 8 && first->ice_pwd.size() == 24,
           "ufrag 8자, pwd 24자");
    Expect(!agent.FindSessionState("missing!"), "없는 세션 조회");

    Expect(!IceLiteAgent::Start(loop, IceLiteAgentConfig{.addresses = {}, .port = 0}),
           "주소가 없으면 시작 실패");
}


}  // namespace


int main() {
    auto loop = EventLoop::Create();
    auto agent = loop ? IceLiteAgent::Start(
                            *loop, IceLiteAgentConfig{
                                       .addresses = {*vms::core::ParseIpv4Address("127.0.0.1")},
                                       .port = 0})
                      : std::unexpected(std::string("루프 생성 실패"));
    if (!agent) {
        Expect(false, "에이전트 시작: " + agent.error());
        return vms::test::ExitCode();
    }
    Expect((*agent)->Candidates().size() == 1 && (*agent)->Candidates().front().port != 0,
           "실제 포트를 담은 candidate");
    Expect((*agent)->Candidates().front().priority == 2130706431u, "첫 주소의 우선순위");

    TestRespondsAndRecordsNomination(*loop, **agent);
    TestIgnoresInvalidPackets(*loop, **agent);
    TestCreatesDistinctSessionsAndRejectsEmptyConfig(*loop, **agent);

    // 에이전트는 루프보다 먼저 소멸해야 한다.
    agent->reset();
    return vms::test::ExitCode();
}
