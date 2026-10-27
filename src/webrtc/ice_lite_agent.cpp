#include "webrtc/ice_lite_agent.h"

#include <sys/epoll.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <format>
#include <print>
#include <utility>

#include "core/network_interfaces.h"
#include "core/system_error.h"
#include "crypto/random.h"
#include "webrtc/stun_message.h"


namespace vms::webrtc {


namespace {


// 서버 ICE ufrag 길이. RFC 8445 최소치(4자)보다 길게 잡아 세션끼리 겹칠 가능성을 줄인다.
constexpr std::size_t kLocalUfragLength = 8;


// 서버 ICE pwd 길이. RFC 8445 최소치(22자)를 넘는다.
constexpr std::size_t kLocalPwdLength = 24;


// 한 번에 받을 최대 데이터그램 크기. STUN과 DTLS 패킷은 이보다 작다.
constexpr std::size_t kReceiveBufferSize = 4096;


// 읽기 이벤트 한 번에 처리할 최대 데이터그램 수.
//
// 패킷이 계속 들어와도 다른 소켓(HTTP 등)이 처리되도록 이 수만큼만 읽고 루프로 돌아간다.
// 루프는 level-triggered이므로 남은 패킷은 다음 대기에서 다시 알려 준다.
constexpr int kMaxPacketsPerEvent = 64;


// STUN이 아닌 패킷의 로그 중복을 막으려고 기억하는 원격 주소의 최대 수.
//
// 출발 주소를 바꿔 가며 보내는 패킷으로 메모리가 계속 늘지 않도록, 가득 차면 비우고 다시 기록한다.
constexpr std::size_t kMaxLoggedNonStunEndpoints = 256;


// host candidate의 종류 선호도. RFC 8445가 host 후보에 권장하는 최고값이다.
constexpr std::uint32_t kHostTypePreference = 126;


// RTP와 RTCP를 같은 포트로 묶으므로(rtcp-mux) component는 RTP(1) 하나뿐이다.
constexpr std::uint32_t kRtpComponentId = 1;


// 첫 바이트로 구분한 UDP 패킷의 프로토콜(RFC 7983)
enum class PacketKind {
    // ICE 연결 확인 메시지. 첫 바이트 0~3
    kStun,

    // DTLS 레코드. 첫 바이트 20~63. 이후 마일스톤에서 핸드셰이크에 쓴다.
    kDtls,

    // RTP 또는 RTCP. 첫 바이트 128~191
    kRtp,

    // 위 범위에 속하지 않는 패킷. 버린다.
    kUnknown,
};


// 데이터그램의 첫 바이트로 프로토콜을 구분한다.
//
// first_byte: 데이터그램의 첫 바이트
// 반환값: 0~3은 STUN, 20~63은 DTLS, 128~191은 RTP/RTCP, 그 밖은 알 수 없음
PacketKind ClassifyPacket(std::uint8_t first_byte) {
    if (first_byte <= 3) {
        return PacketKind::kStun;
    }
    if (first_byte >= 20 && first_byte <= 63) {
        return PacketKind::kDtls;
    }
    if (first_byte >= 128 && first_byte <= 191) {
        return PacketKind::kRtp;
    }
    return PacketKind::kUnknown;
}


// 프로토콜 이름을 로그용 문구로 바꾼다.
//
// kind: 프로토콜 구분
// 반환값: 정적 문자열
std::string_view PacketKindName(PacketKind kind) {
    switch (kind) {
        case PacketKind::kStun:
            return "STUN";
        case PacketKind::kDtls:
            return "DTLS";
        case PacketKind::kRtp:
            return "RTP/RTCP";
        case PacketKind::kUnknown:
            return "알 수 없는 프로토콜";
    }
    return "알 수 없는 프로토콜";
}


// RFC 8445 공식으로 host candidate의 우선순위를 계산한다.
//
// address_index: 설정 주소 목록에서의 순번. 앞선 주소일수록 우선순위가 높다.
// 반환값: 2^24 * 종류 선호도 + 2^8 * 로컬 선호도 + (256 - component ID)
std::uint32_t ComputeHostPriority(std::size_t address_index) {
    const auto local_preference = static_cast<std::uint32_t>(65535 - address_index);
    return (kHostTypePreference << 24) | (local_preference << 8) | (256 - kRtpComponentId);
}


// 원격 주소를 집합의 키로 바꾼다.
//
// endpoint: 원격 주소와 포트
// 반환값: 주소를 상위 32비트, 포트를 하위 16비트에 담은 값
std::uint64_t EndpointKey(const sockaddr_in& endpoint) {
    return (static_cast<std::uint64_t>(endpoint.sin_addr.s_addr) << 16) | endpoint.sin_port;
}


// 주소와 포트에 바인드한 논블로킹 UDP 소켓을 만들고, 실제로 바인드한 주소를 조회한다.
//
// address: 바인드할 IPv4 주소
// port: 바인드할 포트. 0이면 커널이 고른다.
// local_endpoint: 성공하면 실제 바인드한 주소와 포트를 받는다.
// 반환값: 소켓. 실패하면 시스템 오류 코드
std::expected<core::UniqueFd, std::error_code> OpenUdpSocket(in_addr address, std::uint16_t port,
                                                            sockaddr_in& local_endpoint) {
    core::UniqueFd socket(::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!socket.IsValid()) {
        return std::unexpected(core::LastSystemError());
    }

    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_addr = address;
    bind_address.sin_port = htons(port);
    if (::bind(socket.Get(), reinterpret_cast<const sockaddr*>(&bind_address),
               sizeof(bind_address)) != 0) {
        return std::unexpected(core::LastSystemError());
    }

    socklen_t address_size = sizeof(local_endpoint);
    if (::getsockname(socket.Get(), reinterpret_cast<sockaddr*>(&local_endpoint), &address_size) !=
        0) {
        return std::unexpected(core::LastSystemError());
    }
    return socket;
}


}  // namespace


std::expected<std::unique_ptr<IceLiteAgent>, std::string> IceLiteAgent::Start(
    core::EventLoop& loop, IceLiteAgentConfig config) {
    if (config.addresses.empty()) {
        return std::unexpected("ICE 주소가 하나도 없습니다");
    }

    std::vector<BoundSocket> sockets;
    for (const in_addr& address : config.addresses) {
        BoundSocket bound;
        auto socket = OpenUdpSocket(address, config.port, bound.local_endpoint);
        if (!socket) {
            return std::unexpected(std::format("UDP 소켓 바인드 실패 ({}:{}): {}",
                                               core::FormatIpv4Address(address), config.port,
                                               socket.error().message()));
        }
        bound.socket = *std::move(socket);
        sockets.push_back(std::move(bound));
    }

    // 핸들러가 에이전트 주소를 캡처하므로, 주소가 고정되는 힙 객체를 만든 뒤 등록한다.
    // 등록 도중 실패하면 소멸자가 이미 등록한 소켓만 해제한다.
    std::unique_ptr<IceLiteAgent> agent(new IceLiteAgent(loop, std::move(sockets)));
    for (std::size_t index = 0; index < agent->sockets_.size(); ++index) {
        IceLiteAgent* const agent_address = agent.get();
        const std::error_code add_error =
            loop.Add(agent->sockets_[index].socket.Get(), EPOLLIN,
                     [agent_address, index](std::uint32_t) { agent_address->ReceivePackets(index); });
        if (add_error) {
            return std::unexpected(std::format(
                "UDP 소켓 등록 실패 ({}): {}",
                core::FormatIpv4Endpoint(agent->sockets_[index].local_endpoint), add_error.message()));
        }
    }
    return agent;
}


IceLiteAgent::IceLiteAgent(core::EventLoop& loop, std::vector<BoundSocket> sockets)
    : loop_(loop), sockets_(std::move(sockets)) {
    for (std::size_t index = 0; index < sockets_.size(); ++index) {
        const sockaddr_in& endpoint = sockets_[index].local_endpoint;
        candidates_.push_back(IceCandidate{
            .foundation = static_cast<std::uint32_t>(index + 1),
            .priority = ComputeHostPriority(index),
            .address = core::FormatIpv4Address(endpoint.sin_addr),
            .port = ntohs(endpoint.sin_port),
        });
    }
}


IceLiteAgent::~IceLiteAgent() {
    // 등록되지 않은 fd의 Remove는 아무것도 하지 않으므로, 등록 도중 실패한 경우에도 안전하다.
    for (const BoundSocket& bound : sockets_) {
        loop_.Remove(bound.socket.Get());
    }
}


std::expected<IceCredentials, std::string> IceLiteAgent::CreateSession(
    std::string_view remote_ufrag) {
    std::string local_ufrag;
    do {
        auto generated = crypto::GenerateIceCharString(kLocalUfragLength);
        if (!generated) {
            return std::unexpected(generated.error());
        }
        local_ufrag = *std::move(generated);
    } while (sessions_.contains(local_ufrag));

    auto local_pwd = crypto::GenerateIceCharString(kLocalPwdLength);
    if (!local_pwd) {
        return std::unexpected(local_pwd.error());
    }

    sessions_.emplace(local_ufrag, Session{.local_pwd = *local_pwd,
                                           .remote_ufrag = std::string(remote_ufrag),
                                           .state = IceSessionState::kWaitingForCheck,
                                           .nominated_pair = std::nullopt});
    return IceCredentials{.ice_ufrag = std::move(local_ufrag), .ice_pwd = *std::move(local_pwd)};
}


std::optional<IceSessionState> IceLiteAgent::FindSessionState(std::string_view local_ufrag) const {
    const auto found = sessions_.find(std::string(local_ufrag));
    if (found == sessions_.end()) {
        return std::nullopt;
    }
    return found->second.state;
}


void IceLiteAgent::ReceivePackets(std::size_t socket_index) {
    std::array<std::uint8_t, kReceiveBufferSize> buffer;
    for (int received_count = 0; received_count < kMaxPacketsPerEvent; ++received_count) {
        sockaddr_in remote_endpoint{};
        socklen_t address_size = sizeof(remote_endpoint);
        const ssize_t received_size =
            ::recvfrom(sockets_[socket_index].socket.Get(), buffer.data(), buffer.size(), 0,
                       reinterpret_cast<sockaddr*>(&remote_endpoint), &address_size);
        if (received_size < 0) {
            if (errno == EINTR) {
                continue;
            }
            // EAGAIN이면 대기 중인 패킷을 모두 읽은 것이다. 그 밖의 오류(이전 송신의 ICMP 오류 등)는
            // 이 패킷만의 문제이므로 기록만 하고 다음 이벤트를 기다린다.
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::println(stderr, "UDP 수신 오류 ({}): {}",
                             core::FormatIpv4Endpoint(sockets_[socket_index].local_endpoint),
                             core::LastSystemError().message());
            }
            return;
        }
        if (received_size == 0) {
            continue;
        }
        HandlePacket(socket_index,
                     std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(received_size)),
                     remote_endpoint);
    }
}


void IceLiteAgent::HandlePacket(std::size_t socket_index, std::span<const std::uint8_t> packet,
                                const sockaddr_in& remote_endpoint) {
    const PacketKind kind = ClassifyPacket(packet[0]);
    if (kind == PacketKind::kStun) {
        HandleStunPacket(socket_index, packet, remote_endpoint);
        return;
    }

    // DTLS·RTP는 아직 처리하지 않는다. 브라우저가 재전송하므로 주소마다 처음 한 번만 기록한다.
    if (logged_non_stun_endpoints_.size() >= kMaxLoggedNonStunEndpoints) {
        logged_non_stun_endpoints_.clear();
    }
    if (logged_non_stun_endpoints_.insert(EndpointKey(remote_endpoint)).second) {
        std::println(stderr, "{} 패킷 버림 ({} -> {}), 같은 주소의 이후 패킷은 기록하지 않음",
                     PacketKindName(kind), core::FormatIpv4Endpoint(remote_endpoint),
                     core::FormatIpv4Endpoint(sockets_[socket_index].local_endpoint));
    }
}


void IceLiteAgent::HandleStunPacket(std::size_t socket_index, std::span<const std::uint8_t> packet,
                                    const sockaddr_in& remote_endpoint) {
    const BoundSocket& bound = sockets_[socket_index];
    const std::string remote_text = core::FormatIpv4Endpoint(remote_endpoint);

    // 검증 실패 원인을 남기고 응답 없이 버린다.
    const auto reject = [&](std::string_view reason) {
        std::println(stderr, "STUN 요청 무시 ({}): {}", remote_text, reason);
    };

    const auto message = ParseStunMessage(packet);
    if (!message) {
        reject(StunErrorDescription(message.error()));
        return;
    }
    if (const auto fingerprint = VerifyFingerprint(*message); !fingerprint) {
        reject(StunErrorDescription(fingerprint.error()));
        return;
    }
    // ICE-lite 서버는 요청을 보내지 않으므로 응답 등 다른 메시지가 올 이유가 없다.
    if (message->type != std::to_underlying(StunMessageType::kBindingRequest)) {
        return;
    }

    // USERNAME은 "서버ufrag:브라우저ufrag"이며, 앞부분으로 세션을 찾는다.
    if (!message->username) {
        reject("USERNAME 없음");
        return;
    }
    const std::string_view username = *message->username;
    const std::size_t colon = username.find(':');
    if (colon == std::string_view::npos) {
        reject(std::format("USERNAME 형식이 아님: {}", username));
        return;
    }
    const auto found = sessions_.find(std::string(username.substr(0, colon)));
    if (found == sessions_.end()) {
        reject(std::format("알 수 없는 세션: {}", username));
        return;
    }
    Session& session = found->second;
    if (username.substr(colon + 1) != session.remote_ufrag) {
        reject(std::format("브라우저 ufrag 불일치: {}", username));
        return;
    }
    if (const auto integrity = VerifyMessageIntegrity(*message, session.local_pwd); !integrity) {
        reject(StunErrorDescription(integrity.error()));
        return;
    }

    const auto response =
        BuildBindingSuccessResponse(message->transaction_id, remote_endpoint, session.local_pwd);
    if (!response) {
        reject(StunErrorDescription(response.error()));
        return;
    }
    // 요청이 도착한 소켓으로 보내야 응답의 출발 주소가 브라우저가 보낸 목적지와 같아진다.
    const ssize_t sent_size =
        ::sendto(bound.socket.Get(), response->data(), response->size(), 0,
                 reinterpret_cast<const sockaddr*>(&remote_endpoint), sizeof(remote_endpoint));
    if (sent_size < 0) {
        std::println(stderr, "STUN 응답 송신 실패 ({}): {}", remote_text,
                     core::LastSystemError().message());
        return;
    }

    // 동의 확인 요청에도 계속 응답하지만, 연결 수립은 처음 지명될 때만 기록한다.
    if (message->has_use_candidate && session.state == IceSessionState::kWaitingForCheck) {
        session.state = IceSessionState::kConnected;
        session.nominated_pair =
            NominatedPair{.socket_index = socket_index, .remote_endpoint = remote_endpoint};
        std::println(stderr, "ICE 연결됨: 세션 {}, 브라우저 {} <-> 서버 {}", found->first,
                     remote_text, core::FormatIpv4Endpoint(bound.local_endpoint));
    }
}


}  // namespace vms::webrtc
