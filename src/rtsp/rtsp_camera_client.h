#pragma once

#include <netinet/in.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/event_loop.h"
#include "core/unique_fd.h"
#include "media/live_video_source.h"
#include "rtsp/h264_rtp_depacketizer.h"
#include "rtsp/rtsp_message.h"
#include "rtsp/rtsp_url.h"


namespace vms::rtsp {


// 카메라 하나의 RTSP 클라이언트 설정.
//
// 시간 값은 명세의 기본값을 갖고, 테스트가 짧은 값을 쓰기 위해 둔다.
struct RtspCameraClientConfig {
    // 로그에 쓸 카메라 이름
    std::string camera_name;


    // 해석한 카메라 URL
    RtspUrl url;


    // 시작할 때 url.host를 해석한 IPv4 주소. 제어 연결 대상이자 UDP 출발 주소 검사 기준이다.
    in_addr address{};


    // 연결 시도와 요청마다 응답이 끝나기를 기다리는 시간
    std::chrono::milliseconds response_timeout = std::chrono::seconds(5);


    // PLAY 성공 뒤 RTP가 이 시간 동안 없으면 끊긴 것으로 본다.
    std::chrono::milliseconds rtp_timeout = std::chrono::seconds(3);


    // 처음 실패한 뒤 다시 연결을 시도할 때까지의 간격. 연속 실패마다 두 배가 된다.
    std::chrono::milliseconds initial_reconnect_delay = std::chrono::seconds(1);


    // 재연결 간격의 상한
    std::chrono::milliseconds max_reconnect_delay = std::chrono::seconds(10);
};


// RTSP 클라이언트의 연결 단계
enum class RtspCameraClientState {
    // Start() 전이거나 Stop() 뒤
    kStopped,

    // TCP 연결을 기다림
    kConnecting,

    // OPTIONS 응답을 기다림
    kOptions,

    // DESCRIBE 응답을 기다림
    kDescribe,

    // SETUP 응답을 기다림
    kSetup,

    // PLAY 응답을 기다림
    kPlay,

    // RTP를 받는 중
    kPlaying,

    // 실패한 뒤 재연결 타이머를 기다림
    kBackoff,
};


// 카메라 하나에 RTSP/1.0으로 상시 연결해 UDP로 받은 H.264 RTP를 프레임으로 재조립하고 LiveVideoSource로 넘기는 클라이언트.
//
// 제어 연결은 비차단 TCP이고 OPTIONS → DESCRIBE → SETUP → PLAY 순서로 요청을 하나씩 보내 응답을 기다린다. PLAY가
// 성공하면 소스에 kConnected를 알리고, Session timeout의 절반마다 OPTIONS로 세션을 유지한다. 제어 연결이 닫히거나,
// 응답이 response_timeout 안에 끝나지 않거나, 2xx가 아니거나, RTP가 rtp_timeout 동안 없으면 소켓을 모두 닫고(PLAY
// 뒤였으면 kDisconnected를 알림) 1초부터 두 배씩 최대 10초까지 늘어나는 간격으로 다시 연결한다. 재조립기의 손실은
// kLoss로 알린다. 루프 핸들러와 타이머가 이 객체의 주소를 캡처하므로 std::unique_ptr로만 다루며 이동하지 않는다.
// 연결된 EventLoop와 LiveVideoSource보다 먼저 소멸해야 한다.
class RtspCameraClient {
public:
    // 클라이언트를 만든다. 연결은 Start()에서 시작한다.
    //
    // loop: 소켓과 타이머를 등록할 이벤트 루프. 반환된 객체보다 오래 살아 있어야 한다.
    // config: 카메라 이름, URL, 주소, 시간 값
    // source: 프레임과 연결 상태를 넘길 실시간 소스. 반환된 객체보다 오래 살아 있어야 한다.
    // 반환값: kStopped 상태의 클라이언트
    static std::unique_ptr<RtspCameraClient> Create(core::EventLoop& loop, RtspCameraClientConfig config,
                                                    media::LiveVideoSource& source);


    // Stop()과 같이 정리한다.
    ~RtspCameraClient();


    // 핸들러가 캡처한 주소를 유지해야 하므로 복사를 금지한다.
    RtspCameraClient(const RtspCameraClient&) = delete;


    // 핸들러가 캡처한 주소를 유지해야 하므로 복사 대입을 금지한다.
    RtspCameraClient& operator=(const RtspCameraClient&) = delete;


    // 첫 연결을 시작한다. kStopped가 아니면 아무것도 하지 않는다.
    //
    // 연결 실패는 오류로 돌려주지 않고 로그를 남긴 뒤 재연결 간격 뒤에 다시 시도한다.
    void Start();


    // 연결을 끝내고 재연결을 멈춘다.
    //
    // kPlaying이면 TEARDOWN을 비차단으로 한 번 쓰고(응답은 기다리지 않음) 소켓을 닫은 뒤 소스에 kDisconnected를
    // 알린다. 이미 멈췄으면 아무것도 하지 않는다.
    void Stop();


    // 현재 연결 단계를 돌려준다.
    RtspCameraClientState State() const { return state_; }


private:
    // 이벤트 루프에서 받을 소켓 하나와 등록 여부.
    struct RegisteredSocket {
        // 소켓
        core::UniqueFd socket;


        // 루프에 등록했는지 여부
        bool is_registered = false;
    };


    // 설정과 소스로 객체를 만든다. Create()에서만 호출한다.
    RtspCameraClient(core::EventLoop& loop, RtspCameraClientConfig config, media::LiveVideoSource& source);


    // 비차단 TCP 연결을 시작하고 kConnecting이 된다. 실패하면 Fail()로 재연결을 예약한다.
    void Connect();


    // 제어 연결 소켓의 이벤트를 처리한다.
    //
    // events: epoll 이벤트 비트
    void HandleControlEvent(std::uint32_t events);


    // 쓰기 가능 이벤트에서 비차단 연결의 결과를 확인하고, 성공하면 OPTIONS를 보낸다.
    void FinishConnect();


    // 제어 연결에서 읽을 수 있는 바이트를 모두 읽고 완성된 응답을 처리한다.
    void ReceiveControlBytes();


    // 수신 버퍼에 완성된 응답이 있으면 하나씩 꺼내 처리한다.
    void ProcessResponses();


    // 응답 하나를 기다리던 요청에 맞춰 처리하고 다음 요청을 보낸다.
    //
    // response: 완성된 응답
    void HandleResponse(const RtspResponse& response);


    // DESCRIBE 응답의 SDP로 트랙을 고르고 UDP 포트 쌍을 연 뒤 SETUP을 보낸다.
    //
    // response: 2xx DESCRIBE 응답
    void HandleDescribeResponse(const RtspResponse& response);


    // SETUP 응답의 Session·Transport를 기록하고 PLAY를 보낸다.
    //
    // response: 2xx SETUP 응답
    void HandleSetupResponse(const RtspResponse& response);


    // PLAY 성공으로 kPlaying이 되고, 소스에 kConnected를 알리며 RTP 감시와 유지 타이머를 건다.
    void EnterPlaying();


    // 요청을 보내고 응답 제한 타이머를 건다.
    //
    // method: 메서드
    // url: 요청 URL
    // headers: CSeq·User-Agent 뒤에 넣을 헤더. 세션이 정해졌으면 Session을 덧붙인다.
    void SendRequest(std::string_view method, std::string_view url, std::vector<RtspHeader> headers = {});


    // 송신 버퍼를 쓸 수 있는 만큼 쓴다. 다 쓰지 못하면 쓰기 가능 이벤트를 기다린다.
    //
    // 반환값: 소켓 오류로 Fail()을 불렀으면 false
    bool FlushSendBuffer();


    // 짝수·홀수 UDP 포트 쌍을 바인드하고 루프에 등록한다.
    //
    // 반환값: 성공하면 true. 16번 시도해도 쌍을 잡지 못하거나 등록에 실패하면 Fail()을 부르고 false
    bool OpenMediaSockets();


    // RTP 소켓에 대기 중인 패킷을 정해진 수까지 읽어 재조립기에 넣는다.
    void ReceiveRtpPackets();


    // RTCP 소켓에 대기 중인 패킷을 읽어 버린다.
    void DrainRtcpPackets();


    // RTP 감시 타이머가 실행되었을 때, 마지막 RTP 뒤 rtp_timeout이 지났으면 끊고 아니면 남은 시간으로 다시 건다.
    void HandleRtpWatchTimer();


    // 세션 유지 OPTIONS를 보내고 다음 유지 타이머를 건다. 응답을 기다리는 요청이 있으면 다음 주기로 미룬다.
    void HandleKeepaliveTimer();


    // 연결을 실패·끊김으로 처리하고 재연결을 예약한다.
    //
    // reason: 로그에 남길 원인
    //
    // 소켓과 타이머를 모두 정리하고 kBackoff가 된다. kPlaying이었으면 소스에 kDisconnected를 알린다.
    void Fail(std::string_view reason);


    // 소켓과 연결별 타이머를 모두 닫고 연결별 상태를 비운다. 재연결 타이머와 간격은 그대로 둔다.
    void CloseConnection();


    // 소켓의 루프 등록을 해제하고 닫는다.
    //
    // registered: 정리할 소켓
    void CloseSocket(RegisteredSocket& registered);


    // 지금 기다리는 응답의 메서드 이름. 로그에 쓴다.
    std::string_view PendingMethodName() const;


    // 소켓과 타이머를 등록한 이벤트 루프
    core::EventLoop& loop_;


    // 카메라 이름, URL, 주소, 시간 값
    RtspCameraClientConfig config_;


    // 프레임과 연결 상태를 넘길 실시간 소스
    media::LiveVideoSource& source_;


    // 현재 연결 단계
    RtspCameraClientState state_ = RtspCameraClientState::kStopped;


    // RTSP 제어 연결
    RegisteredSocket control_socket_;


    // RTP를 받는 UDP 소켓(짝수 포트)
    RegisteredSocket rtp_socket_;


    // RTCP를 받는 UDP 소켓(RTP 포트 + 1)
    RegisteredSocket rtcp_socket_;


    // RTP 소켓의 포트. SETUP의 client_port에 쓴다.
    std::uint16_t rtp_port_ = 0;


    // 아직 다 쓰지 못한 요청 바이트
    std::string send_buffer_;


    // 받았지만 아직 응답으로 처리하지 않은 바이트
    std::string receive_buffer_;


    // 마지막으로 보낸 요청의 CSeq. 연결마다 0부터 시작해 요청마다 1씩 늘린다.
    std::uint32_t last_cseq_ = 0;


    // 응답을 기다리는 요청의 CSeq. 기다리는 요청이 없으면 비어 있다.
    std::optional<std::uint32_t> pending_cseq_;


    // DESCRIBE에서 고른 트랙
    RtspVideoTrack video_track_;


    // SETUP 대상 URL. 트랙의 control을 기준 URL로 해석한 값이다.
    std::string track_url_;


    // PLAY·TEARDOWN 대상 URL. 세션 수준 control을 기준 URL로 해석한 값이다.
    std::string aggregate_url_;


    // SETUP 응답의 세션. PLAY와 유지 요청, TEARDOWN에 넣는다.
    std::optional<RtspSession> session_;


    // 재조립 상태. DESCRIBE에서 payload type을 알게 되면 만들고, 연결을 닫을 때 지운다.
    std::unique_ptr<H264RtpDepacketizer> depacketizer_;


    // 마지막으로 RTP를 받은 시각. kPlaying에 들어갈 때 그 시각으로 초기화한다.
    std::chrono::steady_clock::time_point last_rtp_time_;


    // 걸려 있는 응답 제한 타이머. 없으면 0
    core::EventLoop::TimerId response_timer_id_ = 0;


    // 걸려 있는 RTP 감시 타이머. 없으면 0
    core::EventLoop::TimerId rtp_watch_timer_id_ = 0;


    // 걸려 있는 세션 유지 타이머. 없으면 0
    core::EventLoop::TimerId keepalive_timer_id_ = 0;


    // 걸려 있는 재연결 타이머. 없으면 0
    core::EventLoop::TimerId reconnect_timer_id_ = 0;


    // 다음 실패 뒤 재연결까지의 간격. PLAY에 성공하면 처음 값으로 되돌린다.
    std::chrono::milliseconds reconnect_delay_;


    // 다른 주소에서 온 RTP를 버렸다고 이 연결에서 이미 로그를 남겼는지 여부
    bool has_logged_foreign_packet_ = false;
};


}  // namespace vms::rtsp
