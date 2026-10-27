#pragma once

#include <netinet/in.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/event_loop.h"
#include "core/unique_fd.h"
#include "webrtc/ice_candidate.h"


namespace vms::webrtc {


// ICE-lite 에이전트 실행 설정.
struct IceLiteAgentConfig {
    // UDP 소켓을 바인드하고 candidate로 알릴 IPv4 주소. 우선순위가 높은 것부터 나열한다.
    std::vector<in_addr> addresses;


    // 모든 주소에 공통으로 쓸 UDP 포트. 0이면 주소마다 커널이 고른 포트를 쓴다(테스트용).
    std::uint16_t port = 50000;
};


// 세션을 만들 때 서버가 새로 정한 ICE 자격 증명. SDP answer에 넣는다.
struct IceCredentials {
    // 서버 ICE ufrag(8자). 브라우저가 STUN USERNAME 앞부분에 넣어 세션을 가리킨다.
    std::string ice_ufrag;


    // 서버 ICE pwd(24자). STUN MESSAGE-INTEGRITY 키다.
    std::string ice_pwd;
};


// ICE 세션의 연결 상태
enum class IceSessionState {
    // answer를 보냈지만 브라우저가 아직 쌍을 지명하지 않음
    kWaitingForCheck,

    // 브라우저가 USE-CANDIDATE로 쌍을 지명함
    kConnected,
};


// UDP 포트에서 STUN Binding 요청을 검증하고 응답하는 ICE-lite 에이전트.
//
// 서버는 ICE-lite이므로 항상 controlled 역할이다. 브라우저가 보낸 연결 확인 요청에 응답하고
// 브라우저의 지명을 받아들이기만 하며, 먼저 요청을 보내지 않는다.
// 설정한 주소마다 UDP 소켓 하나를 소유하고, 모든 세션이 이 소켓들을 함께 쓴다. 응답은 요청이 도착한
// 소켓에서 보내 출발 주소가 요청의 목적지와 같게 한다.
// 루프 핸들러가 이 객체의 주소를 캡처하므로 std::unique_ptr로만 다루며 이동하지 않는다.
// 연결된 EventLoop보다 먼저 소멸해야 한다.
class IceLiteAgent {
public:
    // 주소마다 UDP 소켓을 열어 루프에 등록하고 패킷을 받기 시작한다.
    //
    // loop: 소켓을 등록할 이벤트 루프. 반환된 에이전트보다 오래 살아 있어야 한다.
    // config: 주소 목록과 포트. 주소가 하나 이상이어야 한다.
    // 반환값: 시작된 에이전트. 주소가 없거나, 어느 한 주소라도 소켓 생성·바인드·등록에 실패하면
    //   실패한 주소와 원인을 담은 오류 메시지
    static std::expected<std::unique_ptr<IceLiteAgent>, std::string> Start(core::EventLoop& loop,
                                                                           IceLiteAgentConfig config);


    // 모든 소켓의 루프 등록을 해제하고 닫는다.
    ~IceLiteAgent();


    // 핸들러가 캡처한 주소를 유지해야 하므로 복사를 금지한다.
    IceLiteAgent(const IceLiteAgent&) = delete;


    // 핸들러가 캡처한 주소를 유지해야 하므로 복사 대입을 금지한다.
    IceLiteAgent& operator=(const IceLiteAgent&) = delete;


    // offer 하나에 대한 ICE 세션을 만든다.
    //
    // remote_ufrag: offer의 브라우저 ice-ufrag. STUN USERNAME 뒷부분과 비교한다.
    // 반환값: 서버 자격 증명. 난수 생성에 실패하면 오류 메시지
    //
    // 세션은 서버 ufrag로 구분하며, 표에 있는 ufrag와 겹치지 않게 만든다. 세션 만료는 아직 없으므로
    // 만든 세션은 에이전트가 소멸할 때까지 남는다.
    std::expected<IceCredentials, std::string> CreateSession(std::string_view remote_ufrag);


    // answer에 넣을 host candidate 목록을 돌려준다.
    //
    // 반환값: 설정 순서대로의 candidate. 실제로 바인드한 포트를 담으며, 이 객체가 살아 있는 동안 유효하다.
    const std::vector<IceCandidate>& Candidates() const { return candidates_; }


    // 세션의 연결 상태를 조회한다.
    //
    // local_ufrag: CreateSession이 돌려준 서버 ufrag
    // 반환값: 세션 상태. 세션이 없으면 빈 optional
    std::optional<IceSessionState> FindSessionState(std::string_view local_ufrag) const;


private:
    // 주소 하나에 바인드한 UDP 소켓.
    struct BoundSocket {
        // 바인드한 주소와 실제 포트
        sockaddr_in local_endpoint{};


        // UDP 소켓. 에이전트가 소멸할 때 닫힌다.
        core::UniqueFd socket;
    };


    // 브라우저가 지명한 candidate 쌍.
    struct NominatedPair {
        // 요청이 도착한 서버 소켓의 위치(sockets_ 안의 순번)
        std::size_t socket_index = 0;


        // 요청을 보낸 브라우저 주소와 포트
        sockaddr_in remote_endpoint{};
    };


    // offer 하나에 대한 ICE 세션.
    struct Session {
        // 서버 ICE pwd. MESSAGE-INTEGRITY 검증과 응답 서명에 쓴다.
        std::string local_pwd;


        // offer의 브라우저 ice-ufrag
        std::string remote_ufrag;


        // 연결 상태
        IceSessionState state = IceSessionState::kWaitingForCheck;


        // 처음 지명된 쌍. kConnected일 때만 값이 있다.
        //
        // 이후 마일스톤에서 미디어를 보낼 때는 검증된 USE-CANDIDATE마다 갱신해야 한다.
        std::optional<NominatedPair> nominated_pair;
    };


    // 바인드한 소켓으로 에이전트를 만든다. Start()에서만 호출한다.
    //
    // loop: 소켓을 등록할 이벤트 루프
    // sockets: 소유권을 넘겨받을 소켓 목록
    IceLiteAgent(core::EventLoop& loop, std::vector<BoundSocket> sockets);


    // 소켓에 대기 중인 패킷을 정해진 수까지 읽어 처리한다. 소켓이 읽기 가능할 때 호출된다.
    //
    // socket_index: 읽을 소켓의 위치
    void ReceivePackets(std::size_t socket_index);


    // 받은 패킷 하나를 첫 바이트로 분류해 처리한다.
    //
    // socket_index: 패킷이 도착한 소켓의 위치
    // packet: 받은 데이터그램
    // remote_endpoint: 보낸 쪽 주소
    void HandlePacket(std::size_t socket_index, std::span<const std::uint8_t> packet,
                      const sockaddr_in& remote_endpoint);


    // STUN 패킷을 검증하고, 통과한 Binding 요청에 성공 응답을 보낸다.
    //
    // socket_index: 패킷이 도착한 소켓의 위치
    // packet: 첫 바이트가 STUN 범위인 데이터그램
    // remote_endpoint: 보낸 쪽 주소
    //
    // 검증에 실패하면 응답하지 않고 원인을 로그에 남긴다. USE-CANDIDATE가 처음 오면 세션을 연결됨으로 바꾼다.
    void HandleStunPacket(std::size_t socket_index, std::span<const std::uint8_t> packet,
                          const sockaddr_in& remote_endpoint);


    // 소켓을 등록한 이벤트 루프
    core::EventLoop& loop_;


    // 주소별 UDP 소켓. 설정 주소 순서이며 candidates_와 같은 위치끼리 대응한다.
    std::vector<BoundSocket> sockets_;


    // answer에 넣을 host candidate. sockets_와 같은 순서다.
    std::vector<IceCandidate> candidates_;


    // ICE 세션 표. 키는 서버 ufrag다.
    std::unordered_map<std::string, Session> sessions_;


    // STUN이 아닌 패킷을 버렸다고 이미 로그에 남긴 원격 주소. 같은 주소의 반복 로그를 막는다.
    //
    // 키는 IPv4 주소(상위 32비트)와 포트(하위 16비트)를 합친 값이다. 크기가 상한에 이르면 비운다.
    std::unordered_set<std::uint64_t> logged_non_stun_endpoints_;
};


}  // namespace vms::webrtc
