#pragma once

#include <netinet/in.h>

#include <chrono>
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
#include "crypto/dtls_context.h"
#include "webrtc/dtls_transport.h"
#include "webrtc/ice_candidate.h"


namespace vms::webrtc {


// ICE-lite 에이전트 실행 설정.
struct IceLiteAgentConfig {
    // UDP 소켓을 바인드하고 candidate로 알릴 IPv4 주소. 우선순위가 높은 것부터 나열한다.
    std::vector<in_addr> addresses;


    // 모든 주소에 공통으로 쓸 UDP 포트. 0이면 주소마다 커널이 고른 포트를 쓴다(테스트용).
    std::uint16_t port = 50000;


    // 세션을 만든 뒤, 또는 마지막 유효 Binding 요청 뒤 이 시간 동안 유효한 요청이 없으면 세션을 제거한다.
    //
    // 명령행 인자로는 노출하지 않으며, 테스트가 짧은 값을 쓰기 위해 둔다.
    std::chrono::milliseconds session_timeout = std::chrono::seconds(30);
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
    // answer를 보냈지만 브라우저가 아직 candidate pair를 nomination하지 않음
    kWaitingForCheck,

    // 브라우저가 USE-CANDIDATE로 candidate pair를 nomination함
    kConnected,
};


// UDP 포트에서 STUN Binding 요청을 검증하고 응답하며, nominated pair 위의 DTLS 패킷을 세션의
// DTLS 처리로 넘기는 ICE-lite 에이전트.
//
// 서버는 ICE-lite이므로 항상 controlled 역할이다. 브라우저가 보낸 연결 확인 요청에 응답하고
// 브라우저의 nomination을 받아들이기만 하며, 먼저 요청을 보내지 않는다.
// 설정한 주소마다 UDP 소켓 하나를 소유하고, 모든 세션이 이 소켓들을 함께 쓴다. 응답은 요청이 도착한
// 소켓에서 보내 출발 주소가 요청의 목적지와 같게 한다.
// 세션마다 DTLS 재전송 타이머와 만료 타이머를 루프에 건다.
// 루프 핸들러와 타이머가 이 객체의 주소를 캡처하므로 std::unique_ptr로만 다루며 이동하지 않는다.
// 연결된 EventLoop와 DtlsContext보다 먼저 소멸해야 한다.
class IceLiteAgent {
public:
    // 주소마다 UDP 소켓을 열어 루프에 등록하고 패킷을 받기 시작한다.
    //
    // loop: 소켓과 타이머를 등록할 이벤트 루프. 반환된 에이전트보다 오래 살아 있어야 한다.
    // config: 주소 목록, 포트, 세션 만료 시간. 주소가 하나 이상이어야 한다.
    // dtls_context: 세션마다 DtlsTransport를 만들 컨텍스트. 참조만 하므로 반환된 에이전트보다 오래
    //   살아 있어야 한다.
    // 반환값: 시작된 에이전트. 주소가 없거나, 어느 한 주소라도 소켓 생성·바인드·등록에 실패하면
    //   실패한 주소와 원인을 담은 오류 메시지
    static std::expected<std::unique_ptr<IceLiteAgent>, std::string> Start(
        core::EventLoop& loop, IceLiteAgentConfig config, const crypto::DtlsContext& dtls_context);


    // 모든 세션의 타이머를 취소하고, 모든 소켓의 루프 등록을 해제하고 닫는다.
    ~IceLiteAgent();


    // 핸들러가 캡처한 주소를 유지해야 하므로 복사를 금지한다.
    IceLiteAgent(const IceLiteAgent&) = delete;


    // 핸들러가 캡처한 주소를 유지해야 하므로 복사 대입을 금지한다.
    IceLiteAgent& operator=(const IceLiteAgent&) = delete;


    // offer 하나에 대한 ICE 세션을 만든다.
    //
    // remote_ufrag: offer의 브라우저 ice-ufrag. STUN USERNAME 뒷부분과 비교한다.
    // remote_fingerprint: offer의 브라우저 인증서 SHA-256 fingerprint(대문자 정규화). DTLS 핸드셰이크에서
    //   받은 브라우저 인증서와 비교한다.
    // 반환값: 서버 자격 증명. 난수 생성에 실패하면 오류 메시지
    //
    // 세션은 서버 ufrag로 구분하며, 세션 테이블에 있는 ufrag와 겹치지 않게 만든다. 만든 세션은
    // config.session_timeout 동안 유효한 Binding 요청이 없으면 제거된다.
    std::expected<IceCredentials, std::string> CreateSession(std::string_view remote_ufrag,
                                                             std::string_view remote_fingerprint);


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


    // 브라우저가 nomination한 candidate pair.
    struct NominatedPair {
        // 요청이 도착한 서버 소켓의 위치(sockets_ 안의 순번)
        std::size_t socket_index = 0;


        // 요청을 보낸 브라우저 주소와 포트
        sockaddr_in remote_endpoint{};
    };


    // nominated pair 테이블의 키. 서버 주소(소켓 순번)와 브라우저 주소·포트로 pair 하나를 가리킨다.
    //
    // 같은 브라우저 주소라도 도착한 서버 소켓이 다르면 다른 pair이고, 같은 PC의 탭 두 개는 IP가 같고
    // 포트만 다르므로 세 값을 모두 쓴다.
    struct NominatedPairKey {
        // 패킷이 도착한 서버 소켓의 위치(sockets_ 안의 순번)
        std::size_t socket_index = 0;


        // 브라우저 IPv4 주소(네트워크 바이트 순서)
        std::uint32_t remote_address = 0;


        // 브라우저 포트(네트워크 바이트 순서)
        std::uint16_t remote_port = 0;


        // 세 값이 모두 같으면 같은 pair다.
        bool operator==(const NominatedPairKey&) const = default;
    };


    // NominatedPairKey의 해시.
    struct NominatedPairKeyHash {
        // 세 값을 하나의 64비트 값으로 섞어 해시를 만든다.
        std::size_t operator()(const NominatedPairKey& key) const;
    };


    // offer 하나에 대한 ICE 세션.
    struct Session {
        // 서버 ICE pwd. MESSAGE-INTEGRITY 검증과 응답 서명에 쓴다.
        std::string local_pwd;


        // offer의 브라우저 ice-ufrag
        std::string remote_ufrag;


        // offer의 브라우저 인증서 SHA-256 fingerprint. DTLS 핸드셰이크의 브라우저 인증서와 비교한다.
        std::string remote_fingerprint;


        // 연결 상태
        IceSessionState state = IceSessionState::kWaitingForCheck;


        // 처음 nomination을 받은 pair. kConnected일 때만 값이 있으며 DTLS 송수신 경로로 쓴다.
        //
        // 이후 마일스톤에서 nomination 변경을 따라가려면 검증된 USE-CANDIDATE마다 이 값과
        // nominated pair 테이블의 키를 함께 바꿔야 한다.
        std::optional<NominatedPair> nominated_pair;


        // 이 세션의 DTLS 상태. 처음 nomination을 받을 때 만들며, 생성에 실패하면 비어 있다.
        //
        // nominated pair 테이블에 항목이 있는 세션은 항상 값이 있다.
        std::unique_ptr<DtlsTransport> dtls_transport;


        // 세션을 만든 시각 또는 마지막 유효 Binding 요청을 받은 시각. 만료 판정 기준이다.
        std::chrono::steady_clock::time_point last_activity_time;


        // 걸려 있는 DTLS 재전송 타이머. 없으면 0
        core::EventLoop::TimerId retransmission_timer_id = 0;


        // 걸려 있는 세션 만료 타이머. 세션이 있는 동안 항상 하나가 걸려 있다.
        core::EventLoop::TimerId expiry_timer_id = 0;
    };


    // 세션 테이블의 항목을 가리키는 반복자
    using SessionIterator = std::unordered_map<std::string, Session>::iterator;


    // 바인드한 소켓으로 에이전트를 만든다. Start()에서만 호출한다.
    //
    // loop: 소켓과 타이머를 등록할 이벤트 루프
    // sockets: 소유권을 넘겨받을 소켓 목록
    // session_timeout: 세션 만료 시간
    // dtls_context: DtlsTransport를 만들 컨텍스트
    IceLiteAgent(core::EventLoop& loop, std::vector<BoundSocket> sockets,
                 std::chrono::milliseconds session_timeout, const crypto::DtlsContext& dtls_context);


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
    // 검증에 실패하면 응답하지 않고 원인을 로그에 남긴다. 유효한 요청이면 세션의 마지막 요청 시각을
    // 갱신하고, USE-CANDIDATE가 처음 오면 세션을 연결됨으로 바꾼다.
    void HandleStunPacket(std::size_t socket_index, std::span<const std::uint8_t> packet,
                          const sockaddr_in& remote_endpoint);


    // 세션이 처음 nomination을 받았을 때 연결됨으로 바꾸고 DTLS 경로를 정한다.
    //
    // session: nomination을 받은 세션
    // socket_index: USE-CANDIDATE 요청이 도착한 소켓의 위치
    // remote_endpoint: 요청을 보낸 브라우저 주소
    //
    // DtlsTransport를 만든 뒤 nominated pair 테이블에 넣는다. 생성에 실패하면 테이블에 넣지 않으므로
    // 그 세션의 DTLS 패킷은 nominated pair가 아닌 주소의 패킷처럼 버려진다.
    void RecordNomination(SessionIterator session, std::size_t socket_index,
                          const sockaddr_in& remote_endpoint);


    // DTLS 패킷을 nominated pair 테이블로 세션을 찾아 그 세션의 DtlsTransport로 넘긴다.
    //
    // socket_index: 패킷이 도착한 소켓의 위치
    // packet: 첫 바이트가 DTLS 범위인 데이터그램
    // remote_endpoint: 보낸 쪽 주소
    //
    // nominated pair가 아닌 주소의 패킷은 버리고 주소별 첫 패킷만 로그를 남긴다. DTLS가 실패했거나
    // 닫힌 세션의 패킷은 로그 없이 버린다.
    void HandleDtlsPacket(std::size_t socket_index, std::span<const std::uint8_t> packet,
                          const sockaddr_in& remote_endpoint);


    // 세션의 DTLS 재전송 타이머가 만료되었을 때 재전송한다.
    //
    // local_ufrag: 타이머를 건 세션의 서버 ufrag. 그 사이 세션이 제거되었으면 아무것도 하지 않는다.
    void HandleRetransmissionTimeout(const std::string& local_ufrag);


    // DtlsTransport를 호출한 뒤의 공통 처리를 한다. 출력을 nominated pair로 보내고, 재전송 타이머를
    // 다시 걸고, 상태가 바뀌었으면 로그를 남긴다.
    //
    // session: DtlsTransport를 가진 세션
    // datagrams: DtlsTransport가 돌려준 데이터그램
    // previous_state: 호출 전 DTLS 상태
    void FinishDtlsStep(SessionIterator session, const std::vector<Datagram>& datagrams,
                        DtlsState previous_state);


    // 세션 만료 타이머가 실행되었을 때, 마지막 요청 뒤 만료 시간이 지났으면 세션을 제거하고 아니면
    // 남은 시간으로 다시 건다.
    //
    // local_ufrag: 타이머를 건 세션의 서버 ufrag
    //
    // 요청마다 타이머를 다시 걸지 않고 실행될 때 남은 시간을 확인하므로, 몇 초마다 오는 동의 확인
    // 요청이 타이머 테이블을 바꾸지 않는다.
    void HandleExpiryTimeout(const std::string& local_ufrag);


    // 세션의 만료 타이머를 건다.
    //
    // local_ufrag: 세션의 서버 ufrag
    // delay: 지금부터 만료를 확인할 때까지의 시간
    // 반환값: 건 타이머의 ID
    core::EventLoop::TimerId ScheduleExpiryCheck(const std::string& local_ufrag,
                                                 std::chrono::milliseconds delay);


    // 세션을 제거하고 로그를 남긴다.
    //
    // session: 제거할 세션. 호출 뒤에는 무효다.
    //
    // 타이머 취소, nominated pair 테이블 항목 삭제, 세션 삭제 순서로 한다. 세션을 지우면 DtlsTransport의
    // SSL이 함께 해제된다.
    void RemoveSession(SessionIterator session);


    // STUN이 아닌 패킷을 버렸다는 로그를 원격 주소마다 처음 한 번만 남긴다.
    //
    // protocol_name: 버린 패킷의 프로토콜 이름
    // socket_index: 패킷이 도착한 소켓의 위치
    // remote_endpoint: 보낸 쪽 주소
    void LogDroppedPacket(std::string_view protocol_name, std::size_t socket_index,
                          const sockaddr_in& remote_endpoint);


    // 소켓과 타이머를 등록한 이벤트 루프
    core::EventLoop& loop_;


    // 세션의 DtlsTransport를 만들 컨텍스트
    const crypto::DtlsContext& dtls_context_;


    // 세션 만료 시간
    std::chrono::milliseconds session_timeout_;


    // 주소별 UDP 소켓. 설정 주소 순서이며 candidates_와 같은 위치끼리 대응한다.
    std::vector<BoundSocket> sockets_;


    // answer에 넣을 host candidate. sockets_와 같은 순서다.
    std::vector<IceCandidate> candidates_;


    // ICE 세션 테이블. 키는 서버 ufrag다.
    std::unordered_map<std::string, Session> sessions_;


    // nominated pair 테이블. DTLS 패킷에는 세션 표시가 없으므로 도착 소켓과 출발 주소로 세션을 찾는다.
    //
    // 값은 서버 ufrag이며, 세션마다 처음 nomination을 받은 pair 하나만 넣는다. 세션을 제거할 때 지운다.
    std::unordered_map<NominatedPairKey, std::string, NominatedPairKeyHash> nominated_pairs_;


    // STUN이 아닌 패킷을 버렸다고 이미 로그에 남긴 원격 주소. 같은 주소의 반복 로그를 막는다.
    //
    // 키는 IPv4 주소(상위 32비트)와 포트(하위 16비트)를 합친 값이다. 크기가 상한에 이르면 비운다.
    std::unordered_set<std::uint64_t> logged_non_stun_endpoints_;
};


}  // namespace vms::webrtc
