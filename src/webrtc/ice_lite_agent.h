#pragma once

#include <netinet/in.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
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
#include "crypto/srtp.h"
#include "media/h264_file.h"
#include "media/live_video_source.h"
#include "webrtc/dtls_transport.h"
#include "webrtc/ice_candidate.h"
#include "webrtc/ice_listen_address.h"
#include "webrtc/live_video_stream_sender.h"
#include "webrtc/rtcp_packet.h"
#include "webrtc/video_stream_sender.h"


namespace vms::webrtc {


// ICE-lite 에이전트 실행 설정.
struct IceLiteAgentConfig {
    // 소켓 구성과 candidate 구성. 소켓은 bind address마다 하나 열고, candidate는 announced address마다 하나 만든다.
    //
    // 하나 이상이어야 하며, 항목마다 announced_addresses가 비어 있으면 안 된다. candidate의 priority는 목록 순서와
    // 각 항목의 announced address 순서를 이어서 센 순번대로 높다.
    std::vector<IceListenAddress> addresses;


    // 모든 주소에 공통으로 쓸 UDP 포트. 0이면 주소마다 커널이 고른 포트를 쓴다(테스트용).
    std::uint16_t port = 50000;


    // 세션을 만든 뒤, 또는 마지막 유효 Binding 요청 뒤 이 시간 동안 유효한 요청이 없으면 세션을 제거한다.
    //
    // 명령행 인자로는 노출하지 않으며, 테스트가 짧은 값을 쓰기 위해 둔다.
    std::chrono::milliseconds session_timeout = std::chrono::seconds(30);


    // 동영상 재생 속도(초당 프레임 수). 1 이상이어야 하며, 파일 동영상이 지정된 세션에 공통으로 쓴다.
    int video_fps = 30;


    // 카메라 세션이 DTLS 완료 뒤 끊긴 카메라를 기다리는 최대 시간. 지나면 close_notify를 보내고 세션을 제거한다.
    //
    // 명령행 인자로는 노출하지 않으며, 테스트가 짧은 값을 쓰기 위해 둔다.
    std::chrono::milliseconds camera_wait_timeout = std::chrono::seconds(30);
};


// offer 하나로 세션을 만들 때 필요한 브라우저 쪽 값.
struct IceSessionRequest {
    // offer의 브라우저 ice-ufrag. STUN USERNAME 뒷부분과 비교한다.
    std::string_view remote_ufrag;


    // offer의 브라우저 인증서 SHA-256 fingerprint(대문자 정규화). DTLS 핸드셰이크에서 받은 브라우저
    // 인증서와 비교한다.
    std::string_view remote_fingerprint;


    // answer에서 정한 H.264 payload type(0~127). 이 세션이 보내는 RTP 헤더에 넣는다.
    std::uint8_t h264_payload_type = 0;


    // DTLS가 완료되면 이 세션에 첫 프레임부터 보낼 파일 동영상. camera와 함께 지정하지 않는다.
    //
    // 참조만 하므로 에이전트보다 오래 살아 있어야 한다. 보통 VideoLibrary의 항목을 가리킨다.
    const media::H264File* video = nullptr;


    // DTLS가 완료되면 이 세션이 구독할 카메라 소스. video와 함께 지정하지 않는다. 둘 다 nullptr이면 미디어를 보내지 않는다.
    //
    // 참조만 하므로 에이전트보다 오래 살아 있어야 한다. 보통 CameraList 항목의 소스를 가리킨다.
    media::LiveVideoSource* camera = nullptr;


    // 소스 종류와 이름을 담은 로그용 문자열. 예: "file a.h264", "camera front". 세션이 복사해 보관한다.
    std::string_view source_label;
};


// 세션을 만들 때 서버가 새로 정한 값. SDP answer에 넣는다.
struct IceSessionParameters {
    // 서버 ICE ufrag(8자). 브라우저가 STUN USERNAME 앞부분에 넣어 세션을 가리킨다.
    std::string ice_ufrag;


    // 서버 ICE pwd(24자). STUN MESSAGE-INTEGRITY 키다.
    std::string ice_pwd;


    // 이 세션의 송신 SSRC. 0이 아니며 다른 세션과 겹치지 않는다. answer의 a=ssrc에 넣는다.
    std::uint32_t ssrc = 0;
};


// ICE 세션의 연결 상태
enum class IceSessionState {
    // answer를 보냈지만 브라우저가 아직 candidate pair를 nomination하지 않음
    kWaitingForCheck,

    // 브라우저가 USE-CANDIDATE로 candidate pair를 nomination함
    kConnected,
};


// 세션 하나가 브라우저에게서 받은 RTCP의 수신 결과와 버린 패킷 수.
//
// 세션과 함께 만들어지고 사라지며, 세션을 제거할 때 요약 로그로 남긴다.
struct RtcpReceiveStats {
    // 검증·복호화·해석을 모두 통과한 RTCP 복합 패킷 수
    std::uint64_t compound_packet_count = 0;


    // 통과한 복합 패킷 안의 RTCP 패킷 수. 키는 패킷 종류 번호(PT)다.
    std::map<std::uint8_t, std::uint64_t> packet_count_by_type;


    // 마지막으로 받은 RR의 마지막 보고 블록. 아직 보고 블록이 담긴 RR을 받지 않았으면 비어 있다.
    std::optional<RtcpReportBlock> last_receiver_report_block;


    // 길이 부족으로 버린 패킷 수
    std::uint64_t too_short_count = 0;


    // SRTCP 인증 실패로 버린 패킷 수
    std::uint64_t authentication_failure_count = 0;


    // 재생 방지 창에서 거부해 버린 패킷 수
    std::uint64_t replayed_count = 0;


    // 재생 방지 창을 둘 SSRC 수를 넘어 버린 패킷 수
    std::uint64_t too_many_ssrcs_count = 0;


    // OpenSSL 연산 실패로 버린 패킷 수
    std::uint64_t crypto_failure_count = 0;


    // 복호화 뒤 RTCP 형식 오류로 버린 패킷 수
    std::uint64_t malformed_count = 0;


    // 원인과 패킷 정보를 상세 로그로 남긴 실패 수. 세션마다 처음 몇 개만 남긴다.
    std::uint64_t detailed_failure_log_count = 0;
};


// UDP 포트에서 STUN Binding 요청을 검증하고 응답하며, nominated pair 위의 DTLS 패킷을 세션의
// DTLS 처리로, DTLS가 완료된 세션의 RTCP 패킷을 세션의 RTCP 처리로 넘기는 ICE-lite 에이전트.
//
// 서버는 ICE-lite이므로 항상 controlled 역할이다. 브라우저가 보낸 연결 확인 요청에 응답하고
// 브라우저의 nomination을 받아들이기만 하며, 먼저 요청을 보내지 않는다.
// 설정한 bind address마다 UDP 소켓 하나를 소유하고, 모든 세션이 이 소켓들을 함께 쓴다. 응답은 요청이 도착한
// 소켓에서 보내 출발 주소가 요청의 목적지와 같게 한다.
// 세션마다 DTLS 재전송 타이머와 만료 타이머를 루프에 건다. 파일 동영상이 지정된 세션은 DTLS가 완료되면
// VideoStreamSender를 만들고 프레임 타이머로 nominated pair에 SRTP 패킷을 보낸다. 카메라가 지정된 세션은 DTLS가
// 완료되면 LiveVideoStreamSender를 만들고 카메라 소스를 구독해, 프레임이 올 때마다 보낸다. 카메라가 끊겨 있으면
// camera_wait_timeout 동안 기다리고, 그래도 돌아오지 않으면 close_notify를 보내고 세션을 제거한다.
// 루프 핸들러와 타이머가 이 객체의 주소를 캡처하므로 std::unique_ptr로만 다루며 이동하지 않는다.
// 연결된 EventLoop와 DtlsContext보다 먼저 소멸해야 한다.
class IceLiteAgent {
public:
    // bind address마다 UDP 소켓을 열어 루프에 등록하고 패킷을 받기 시작한다.
    //
    // loop: 소켓과 타이머를 등록할 이벤트 루프. 반환된 에이전트보다 오래 살아 있어야 한다.
    // config: 주소 목록, 포트, 세션 만료 시간, 재생 속도. 주소가 하나 이상이어야 하고, 항목마다 announced address가
    //   하나 이상이어야 한다.
    // dtls_context: 세션마다 DtlsTransport를 만들 컨텍스트. 참조만 하므로 반환된 에이전트보다 오래
    //   살아 있어야 한다.
    // 반환값: 시작된 에이전트. 주소가 없거나, announced address가 빈 항목이 있거나, 어느 한 bind address라도
    //   소켓 생성·바인드·등록에 실패하면 실패한 주소와 원인을 담은 오류 메시지
    static std::expected<std::unique_ptr<IceLiteAgent>, std::string> Start(
        core::EventLoop& loop, IceLiteAgentConfig config, const crypto::DtlsContext& dtls_context);


    // 모든 세션의 타이머를 취소하고(송신 중인 세션은 송신 중지 로그를 남긴다), 모든 소켓의 루프 등록을
    // 해제하고 닫는다.
    ~IceLiteAgent();


    // 핸들러가 캡처한 주소를 유지해야 하므로 복사를 금지한다.
    IceLiteAgent(const IceLiteAgent&) = delete;


    // 핸들러가 캡처한 주소를 유지해야 하므로 복사 대입을 금지한다.
    IceLiteAgent& operator=(const IceLiteAgent&) = delete;


    // offer 하나에 대한 ICE 세션을 만든다.
    //
    // request: 브라우저 ufrag, fingerprint, H.264 payload type. 호출 동안만 참조한다.
    // 반환값: 서버 자격 증명과 송신 SSRC. 난수 생성에 실패하면 오류 메시지
    //
    // 세션은 서버 ufrag로 구분하며, 세션 테이블에 있는 ufrag·SSRC와 겹치지 않게 만든다. SSRC와 payload
    // type은 동영상이 없어도 정해 answer 형식이 설정과 무관하게 같게 한다. 만든 세션은
    // config.session_timeout 동안 유효한 Binding 요청이 없으면 제거된다.
    std::expected<IceSessionParameters, std::string> CreateSession(const IceSessionRequest& request);


    // answer에 넣을 host candidate 목록을 돌려준다.
    //
    // 반환값: announced address마다 하나인 candidate. 설정 순서대로이며, 각 candidate의 포트는 그 announced address가
    //   속한 소켓이 실제로 바인드한 포트다. 이 객체가 살아 있는 동안 유효하다.
    const std::vector<IceCandidate>& Candidates() const { return candidates_; }


    // 세션의 연결 상태를 조회한다.
    //
    // local_ufrag: CreateSession이 돌려준 서버 ufrag
    // 반환값: 세션 상태. 세션이 없으면 빈 optional
    std::optional<IceSessionState> FindSessionState(std::string_view local_ufrag) const;


    // 세션의 RTCP 수신 결과를 조회한다.
    //
    // local_ufrag: CreateSession이 돌려준 서버 ufrag
    // 반환값: 조회 시점의 수신 결과 복사본. 세션이 없으면 빈 optional
    std::optional<RtcpReceiveStats> FindRtcpReceiveStats(std::string_view local_ufrag) const;


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


        // 이 세션이 보내는 RTP의 SSRC. answer의 a=ssrc 값이다.
        std::uint32_t ssrc = 0;


        // 이 세션이 보내는 RTP의 H.264 payload type. answer에서 정한 값이다.
        std::uint8_t h264_payload_type = 0;


        // DTLS가 완료되면 보낼 파일 동영상. nullptr이면 파일을 보내지 않는다. 에이전트보다 오래 산다.
        const media::H264File* video = nullptr;


        // DTLS가 완료되면 구독할 카메라 소스. nullptr이면 카메라를 보내지 않는다. 에이전트보다 오래 산다.
        media::LiveVideoSource* camera = nullptr;


        // 소스 종류와 이름을 담은 로그용 문자열. 소스가 없으면 비어 있다.
        std::string source_label;


        // 동영상 송신 상태. DTLS가 완료되고 동영상이 지정되어 있을 때 만들며, 송신을 멈추면 지운다.
        //
        // 값이 있는 동안 frame_timer_id의 타이머가 걸려 있거나 그 핸들러가 실행 중이다.
        std::unique_ptr<VideoStreamSender> video_stream_sender;


        // 걸려 있는 프레임 타이머. 없으면 0
        core::EventLoop::TimerId frame_timer_id = 0;


        // 카메라 송신 상태. DTLS가 완료되고 카메라가 지정되어 있을 때 만들며, 송신을 멈추면 지운다.
        //
        // 값이 있는 동안 camera_subscription이 걸려 있다.
        std::unique_ptr<LiveVideoStreamSender> live_stream_sender;


        // 카메라 소스의 구독 ID. live_stream_sender와 함께 생기고 사라진다.
        std::optional<media::LiveVideoSource::SubscriptionId> camera_subscription;


        // 걸려 있는 카메라 대기 타이머. 카메라가 끊긴 동안만 걸려 있으며, 없으면 0
        core::EventLoop::TimerId camera_wait_timer_id = 0;


        // 동영상 패킷의 송신 실패를 이미 로그에 남겼는지 여부. 세션마다 첫 실패만 남긴다.
        bool has_logged_send_failure = false;


        // 브라우저가 보내는 SRTCP를 검증·복호화하는 수신 상태. DTLS가 kConnected가 될 때 브라우저 쪽
        // (remote) 키로 만들고, kFailed·kClosed가 되면 지운다.
        //
        // 동영상 지정 여부와 관계없이 만들며, 값이 있는 세션만 RTCP를 처리한다. 생성에 실패하면 비어 있어
        // 그 세션의 RTCP는 nominated pair가 아닌 주소의 패킷처럼 버려진다.
        std::unique_ptr<crypto::SrtcpReceiver> srtcp_receiver;


        // 이 세션이 받은 RTCP의 수신 결과와 버린 패킷 수
        RtcpReceiveStats rtcp_stats;
    };


    // 세션 테이블의 항목을 가리키는 반복자
    using SessionIterator = std::unordered_map<std::string, Session>::iterator;


    // 바인드한 소켓으로 에이전트를 만든다. Start()에서만 호출한다.
    //
    // loop: 소켓과 타이머를 등록할 이벤트 루프
    // sockets: 소유권을 넘겨받을 소켓 목록
    // config: announced address 목록, 세션 만료 시간, 재생 속도. addresses는 sockets와 같은 순서여야 한다.
    //   포트는 쓰지 않는다.
    // dtls_context: DtlsTransport를 만들 컨텍스트
    IceLiteAgent(core::EventLoop& loop, std::vector<BoundSocket> sockets, const IceLiteAgentConfig& config,
                 const crypto::DtlsContext& dtls_context);


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


    // RTCP 패킷을 nominated pair 테이블로 세션을 찾아 SRTCP 검증·복호화와 RTCP 해석을 하고 기록한다.
    //
    // socket_index: 패킷이 도착한 소켓의 위치
    // packet: 첫 바이트가 RTP/RTCP 범위이고 둘째 바이트가 RTCP 범위인 데이터그램
    // remote_endpoint: 보낸 쪽 주소
    //
    // nominated pair가 아니거나 SRTCP 수신 상태가 없는(DTLS 미완료·실패·닫힘) 세션의 패킷은 버리고
    // 주소별 첫 패킷만 로그를 남긴다. 검증이나 해석에 실패하면 원인별 개수를 세고 처음 몇 개만 상세
    // 로그를 남긴다. 성공하면 수신 결과를 갱신하고 세션의 첫 복합 패킷만 상세 로그를 남긴다.
    // RTCP 수신은 세션 만료 판정에 영향을 주지 않는다.
    void HandleRtcpPacket(std::size_t socket_index, std::span<const std::uint8_t> packet,
                          const sockaddr_in& remote_endpoint);


    // 세션이 RTCP 패킷 하나를 버린 원인을 세고, 세션의 처음 몇 개 실패만 상세 로그로 남긴다.
    //
    // session: 패킷을 받은 세션
    // reason: 로그에 남길 원인 문구
    // ssrc: 패킷의 보낸 쪽 SSRC. 읽을 수 없었으면 빈 optional
    // srtcp_index: 패킷의 SRTCP 인덱스. 읽을 수 없었으면 빈 optional
    // packet_size: 받은 데이터그램 길이(바이트)
    //
    // 호출 전에 원인별 개수는 호출자가 올려 둔다.
    void LogRtcpFailure(SessionIterator session, std::string_view reason, std::optional<std::uint32_t> ssrc,
                        std::optional<std::uint32_t> srtcp_index, std::size_t packet_size);


    // 세션의 RTCP 수신 요약을 로그에 남긴다. 받은 복합 패킷도 실패도 없으면 남기지 않는다.
    //
    // session: 요약할 세션
    void LogRtcpSummary(SessionIterator session);


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
    //
    // kConnected가 되면 SRTCP 수신 상태를 만들고 동영상 송신을 시작한다. kClosed나 kFailed가 되면
    // 송신을 멈추고 SRTCP 수신 상태를 지운다.
    void FinishDtlsStep(SessionIterator session, const std::vector<Datagram>& datagrams,
                        DtlsState previous_state);


    // DTLS가 완료된 세션의 송신을 시작한다. 세션에 소스가 지정되지 않았으면 아무것도 하지 않는다.
    //
    // session: kConnected가 된 DtlsTransport를 가진 세션
    //
    // 파일이면 서버용 SRTP 키로 VideoStreamSender를 만들고 0ms 프레임 타이머를 건다. 카메라이면
    // LiveVideoStreamSender를 만들어 카메라 소스를 구독하고, 카메라가 끊겨 있으면 대기 타이머를 건다. 생성에
    // 실패하면 로그만 남기며 DTLS는 연결된 채로 둔다.
    void StartVideoStream(SessionIterator session);


    // 카메라 소스가 넘긴 프레임을 세션의 송신 상태로 패킷화해 nominated pair로 보낸다.
    //
    // local_ufrag: 구독한 세션의 서버 ufrag
    // frame: 구독 콜백이 받은 프레임. 호출 동안만 유효하다.
    //
    // SRTP 보호가 실패하면 그 세션의 송신을 멈추고 구독을 해지한다(구독 콜백 안에서 해지해도 안전하다).
    void HandleCameraFrame(const std::string& local_ufrag, const media::LiveVideoFrame& frame);


    // 카메라 소스의 상태 변화를 세션의 송신 상태에 반영하고, 끊기면 대기를 시작하고 연결되면 끝낸다.
    //
    // local_ufrag: 구독한 세션의 서버 ufrag
    // event: 구독 콜백이 받은 상태 변화
    void HandleCameraEvent(const std::string& local_ufrag, media::LiveVideoEvent event);


    // 카메라 대기 타이머를 걸고 "대기 시작"을 로그에 남긴다. 이미 걸려 있으면 아무것도 하지 않는다.
    //
    // session: 카메라 세션
    void StartCameraWait(SessionIterator session);


    // 카메라 대기 시간이 지났을 때 close_notify를 nominated pair로 보내고 세션을 제거한다.
    //
    // local_ufrag: 타이머를 건 세션의 서버 ufrag. 그 사이 세션이 제거되었으면 아무것도 하지 않는다.
    void HandleCameraWaitTimeout(const std::string& local_ufrag);


    // SRTP 패킷 목록을 세션의 nominated pair로 보낸다.
    //
    // session: 송신 중인 세션
    // packets: 순서대로 보낼 패킷
    //
    // 실패한 패킷은 다시 보내지 않고 버리며, 세션마다 첫 실패만 로그에 남긴다.
    void SendMediaPackets(SessionIterator session, const std::vector<std::vector<std::uint8_t>>& packets);


    // 프레임 타이머가 실행되었을 때 예정된 프레임을 nominated pair로 보내고 다음 프레임 타이머를 건다.
    //
    // local_ufrag: 타이머를 건 세션의 서버 ufrag. 그 사이 세션이 제거되었거나 송신을 멈췄으면 아무것도
    //   하지 않는다.
    //
    // 송신에 실패한 패킷은 다시 보내지 않고 버린다. SRTP 보호가 실패하면 그 세션의 송신을 멈춘다.
    void HandleFrameTimer(const std::string& local_ufrag);


    // 세션의 송신을 멈추고 로그를 남긴다. 송신 중이 아니면 아무것도 하지 않는다.
    //
    // session: 송신을 멈출 세션
    // reason: 로그에 남길 중지 원인
    //
    // 파일이면 프레임 타이머를 취소하고 VideoStreamSender를 지운다. 카메라이면 구독을 해지하고 대기 타이머를 취소한 뒤
    // LiveVideoStreamSender를 지운다. 세션 자체는 그대로 둔다.
    void StopVideoStream(SessionIterator session, std::string_view reason);


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
    // reason: 로그에 남길 제거 원인
    //
    // 송신 중지, 타이머 취소, nominated pair 테이블 항목 삭제, RTCP 수신 요약 로그, 세션 삭제 순서로 한다. 세션을 지우면
    // DtlsTransport의 SSL이 함께 해제된다.
    void RemoveSession(SessionIterator session, std::string_view reason);


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


    // 동영상 재생 속도(초당 프레임 수)
    int video_fps_;


    // 카메라 세션이 끊긴 카메라를 기다리는 최대 시간
    std::chrono::milliseconds camera_wait_timeout_;


    // bind address별 UDP 소켓. 설정 순서다.
    std::vector<BoundSocket> sockets_;


    // answer에 넣을 host candidate. announced address마다 하나이며 설정 순서다. 한 소켓에 여러 candidate가 대응할 수
    // 있다. 패킷은 announced address와 관계없이 bind address의 소켓에 도착하므로 연결 경로는 소켓 순번으로 식별한다.
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
