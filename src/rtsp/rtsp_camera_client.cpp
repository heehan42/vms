#include "rtsp/rtsp_camera_client.h"

#include <sys/epoll.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <format>
#include <print>
#include <utility>

#include "core/network_interfaces.h"
#include "core/system_error.h"
#include "media/h264_sps.h"
#include "webrtc/sdp.h"


namespace vms::rtsp {


namespace {


// 한 번에 받을 최대 데이터그램 크기. 카메라 RTP는 MTU보다 작지만 넉넉히 잡는다.
constexpr std::size_t kDatagramBufferSize = 65536;


// 읽기 이벤트 한 번에 처리할 최대 데이터그램 수. 다른 소켓도 처리되도록 이만큼만 읽고 루프로 돌아간다.
constexpr int kMaxDatagramsPerEvent = 64;


// 짝수·홀수 UDP 포트 쌍을 잡는 최대 시도 횟수
constexpr int kMaxPortPairAttempts = 16;


// 제어 연결에서 한 번에 읽는 크기
constexpr std::size_t kControlReadSize = 4096;


// 바인드한 논블로킹 UDP 소켓을 만든다.
//
// port: 바인드할 포트. 0이면 커널이 고른다.
// 반환값: 소켓과 실제 포트. 실패하면 시스템 오류 코드
std::expected<std::pair<core::UniqueFd, std::uint16_t>, std::error_code> OpenBoundUdpSocket(std::uint16_t port) {
    core::UniqueFd socket(::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!socket.IsValid()) {
        return std::unexpected(core::LastSystemError());
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (::bind(socket.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        return std::unexpected(core::LastSystemError());
    }
    socklen_t address_size = sizeof(address);
    if (::getsockname(socket.Get(), reinterpret_cast<sockaddr*>(&address), &address_size) != 0) {
        return std::unexpected(core::LastSystemError());
    }
    return std::pair(std::move(socket), ntohs(address.sin_port));
}


// sprop-parameter-sets의 SPS에서 로그용 해상도 문구를 만든다.
//
// sprop_parameter_sets: SDP의 sprop-parameter-sets 값
// 반환값: "640x480" 형식. SPS가 없거나 해석할 수 없으면 "알 수 없음"
std::string DescribeResolution(std::string_view sprop_parameter_sets) {
    for (const std::vector<std::uint8_t>& nal_unit : DecodeSpropParameterSets(sprop_parameter_sets)) {
        if (const auto resolution = media::ParseH264SpsResolution(nal_unit)) {
            return std::format("{}x{}", resolution->width, resolution->height);
        }
    }
    return "알 수 없음";
}


}  // namespace


std::unique_ptr<RtspCameraClient> RtspCameraClient::Create(core::EventLoop& loop, RtspCameraClientConfig config,
                                                           media::LiveVideoSource& source) {
    return std::unique_ptr<RtspCameraClient>(new RtspCameraClient(loop, std::move(config), source));
}


RtspCameraClient::RtspCameraClient(core::EventLoop& loop, RtspCameraClientConfig config,
                                   media::LiveVideoSource& source)
    : loop_(loop),
      config_(std::move(config)),
      source_(source),
      reconnect_delay_(config_.initial_reconnect_delay) {}


RtspCameraClient::~RtspCameraClient() { Stop(); }


void RtspCameraClient::Start() {
    if (state_ != RtspCameraClientState::kStopped) {
        return;
    }
    Connect();
}


void RtspCameraClient::Stop() {
    if (state_ == RtspCameraClientState::kStopped) {
        return;
    }

    const bool was_playing = state_ == RtspCameraClientState::kPlaying;
    if (was_playing && session_) {
        // 종료를 늦추지 않도록 한 번만 쓰고 결과는 보지 않는다. 카메라는 세션 timeout으로도 정리한다.
        const std::string teardown =
            BuildRtspRequest("TEARDOWN", aggregate_url_, ++last_cseq_, std::vector{RtspHeader{"Session", session_->id}});
        [[maybe_unused]] const ssize_t written =
            ::send(control_socket_.socket.Get(), teardown.data(), teardown.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        std::println(stderr, "카메라 연결 종료: {}, TEARDOWN 보냄", config_.camera_name);
    }

    CloseConnection();
    loop_.CancelTimer(reconnect_timer_id_);
    reconnect_timer_id_ = 0;
    state_ = RtspCameraClientState::kStopped;
    if (was_playing) {
        source_.PublishEvent(media::LiveVideoEvent::kDisconnected);
    }
}


void RtspCameraClient::Connect() {
    reconnect_timer_id_ = 0;
    sockaddr_in camera_endpoint{};
    camera_endpoint.sin_family = AF_INET;
    camera_endpoint.sin_addr = config_.address;
    camera_endpoint.sin_port = htons(config_.url.port);
    std::println(stderr, "카메라 연결 시도: {}, {} ({})", config_.camera_name, config_.url.request_url,
                 core::FormatIpv4Endpoint(camera_endpoint));

    state_ = RtspCameraClientState::kConnecting;
    control_socket_.socket.Reset(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!control_socket_.socket.IsValid()) {
        Fail(std::format("TCP 소켓 생성 실패: {}", core::LastSystemError().message()));
        return;
    }
    if (::connect(control_socket_.socket.Get(), reinterpret_cast<const sockaddr*>(&camera_endpoint),
                  sizeof(camera_endpoint)) != 0 &&
        errno != EINPROGRESS) {
        Fail(std::format("TCP 연결 실패: {}", core::LastSystemError().message()));
        return;
    }

    // 연결 결과는 쓰기 가능 이벤트로 알 수 있다. 연결 시도도 응답 제한 시간 안에 끝나야 한다.
    const std::error_code add_error = loop_.Add(control_socket_.socket.Get(), EPOLLOUT,
                                                [this](std::uint32_t events) { HandleControlEvent(events); });
    if (add_error) {
        Fail(std::format("TCP 소켓 등록 실패: {}", add_error.message()));
        return;
    }
    control_socket_.is_registered = true;
    response_timer_id_ = loop_.AddTimer(config_.response_timeout, [this] {
        response_timer_id_ = 0;
        Fail(std::format("{} 응답 없음({}ms)", PendingMethodName(), config_.response_timeout.count()));
    });
}


void RtspCameraClient::HandleControlEvent(std::uint32_t events) {
    if (state_ == RtspCameraClientState::kConnecting) {
        FinishConnect();
        return;
    }

    if ((events & EPOLLOUT) != 0 && !FlushSendBuffer()) {
        return;
    }
    if ((events & (EPOLLIN | EPOLLERR | EPOLLHUP)) != 0) {
        ReceiveControlBytes();
    }
}


void RtspCameraClient::FinishConnect() {
    int socket_error = 0;
    socklen_t option_size = sizeof(socket_error);
    if (::getsockopt(control_socket_.socket.Get(), SOL_SOCKET, SO_ERROR, &socket_error, &option_size) != 0) {
        socket_error = errno;
    }
    if (socket_error != 0) {
        Fail(std::format("TCP 연결 실패: {}", std::error_code(socket_error, std::system_category()).message()));
        return;
    }

    state_ = RtspCameraClientState::kOptions;
    if (const std::error_code modify_error = loop_.Modify(control_socket_.socket.Get(), EPOLLIN)) {
        Fail(std::format("TCP 소켓 등록 변경 실패: {}", modify_error.message()));
        return;
    }
    SendRequest("OPTIONS", config_.url.request_url);
}


void RtspCameraClient::ReceiveControlBytes() {
    std::array<char, kControlReadSize> buffer;
    while (true) {
        const ssize_t received_size = ::recv(control_socket_.socket.Get(), buffer.data(), buffer.size(), 0);
        if (received_size > 0) {
            receive_buffer_.append(buffer.data(), static_cast<std::size_t>(received_size));
            continue;
        }
        if (received_size == 0) {
            // 닫히기 전에 도착한 응답은 처리하고, 그래도 연결이 남아 있으면 닫힘으로 끊는다.
            ProcessResponses();
            if (control_socket_.socket.IsValid()) {
                Fail("카메라가 제어 연결을 닫음");
            }
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        Fail(std::format("제어 연결 수신 오류: {}", core::LastSystemError().message()));
        return;
    }
    ProcessResponses();
}


void RtspCameraClient::ProcessResponses() {
    // 응답을 처리하다 Fail()이 연결을 닫으면 버퍼도 비워지므로, 연결이 살아 있는 동안만 이어 간다.
    while (control_socket_.socket.IsValid() && !receive_buffer_.empty()) {
        const auto parsed = ParseRtspResponse(receive_buffer_);
        if (!parsed) {
            Fail(std::format("응답 형식 오류: {}", parsed.error()));
            return;
        }
        if (!*parsed) {
            return;
        }
        const RtspResponse& response = **parsed;
        receive_buffer_.erase(0, response.consumed_size);
        HandleResponse(response);
    }
}


void RtspCameraClient::HandleResponse(const RtspResponse& response) {
    // 요청은 한 번에 하나만 보내므로, 기다리는 요청이 없을 때 온 메시지(서버가 먼저 보낸 요청 등)는 처리하지 않는다.
    if (!pending_cseq_) {
        Fail("기다리는 요청 없이 메시지를 받음");
        return;
    }
    const auto cseq = response.FindHeader("CSeq");
    if (cseq != std::to_string(*pending_cseq_)) {
        Fail(std::format("{} 응답의 CSeq가 다름 (보냄 {}, 받음 {})", PendingMethodName(), *pending_cseq_,
                         cseq.value_or("없음")));
        return;
    }
    if (response.status_code < 200 || response.status_code > 299) {
        Fail(std::format("{} 응답 {} {}", PendingMethodName(), response.status_code, response.reason_phrase));
        return;
    }

    pending_cseq_.reset();
    loop_.CancelTimer(response_timer_id_);
    response_timer_id_ = 0;
    switch (state_) {
        case RtspCameraClientState::kOptions:
            state_ = RtspCameraClientState::kDescribe;
            SendRequest("DESCRIBE", config_.url.request_url, {RtspHeader{"Accept", "application/sdp"}});
            break;
        case RtspCameraClientState::kDescribe:
            HandleDescribeResponse(response);
            break;
        case RtspCameraClientState::kSetup:
            HandleSetupResponse(response);
            break;
        case RtspCameraClientState::kPlay:
            EnterPlaying();
            break;
        case RtspCameraClientState::kPlaying:
            // 세션 유지 OPTIONS의 응답은 2xx인지만 본다.
            break;
        case RtspCameraClientState::kStopped:
        case RtspCameraClientState::kConnecting:
        case RtspCameraClientState::kBackoff:
            break;
    }
}


void RtspCameraClient::HandleDescribeResponse(const RtspResponse& response) {
    const auto description = webrtc::ParseSdp(response.body);
    if (!description) {
        Fail(std::format("DESCRIBE 응답의 SDP 해석 실패: {}", description.error()));
        return;
    }
    auto track = SelectH264VideoTrack(*description);
    if (!track) {
        Fail(track.error());
        return;
    }
    video_track_ = *std::move(track);
    const std::string_view base_url = ContentBaseOf(response, config_.url.request_url);
    track_url_ = ResolveControlUrl(base_url, video_track_.control);
    aggregate_url_ = ResolveControlUrl(base_url, SessionControlOf(*description));

    if (!OpenMediaSockets()) {
        return;
    }
    depacketizer_ = std::make_unique<H264RtpDepacketizer>(
        video_track_.payload_type,
        [this](const media::H264Frame& frame, std::uint32_t rtp_timestamp, bool is_keyframe) {
            source_.PublishFrame(
                media::LiveVideoFrame{.frame = frame, .rtp_timestamp = rtp_timestamp, .is_keyframe = is_keyframe});
        },
        [this] { source_.PublishEvent(media::LiveVideoEvent::kLoss); });

    state_ = RtspCameraClientState::kSetup;
    SendRequest("SETUP", track_url_,
                {RtspHeader{"Transport", std::format("RTP/AVP;unicast;client_port={}-{}", rtp_port_, rtp_port_ + 1)}});
}


void RtspCameraClient::HandleSetupResponse(const RtspResponse& response) {
    const auto session_header = response.FindHeader("Session");
    if (!session_header) {
        Fail("SETUP 응답에 Session이 없음");
        return;
    }
    auto session = ParseSessionHeader(*session_header);
    if (!session) {
        Fail(session.error());
        return;
    }
    const auto transport_header = response.FindHeader("Transport");
    const auto transport = transport_header ? ParseTransportHeader(*transport_header)
                                            : std::expected<RtspTransport, std::string>(
                                                  std::unexpected("SETUP 응답에 Transport가 없음"));
    if (!transport) {
        Fail(transport.error());
        return;
    }
    session_ = *std::move(session);

    state_ = RtspCameraClientState::kPlay;
    SendRequest("PLAY", aggregate_url_, {RtspHeader{"Range", "npt=0.000-"}});
}


void RtspCameraClient::EnterPlaying() {
    state_ = RtspCameraClientState::kPlaying;
    reconnect_delay_ = config_.initial_reconnect_delay;
    last_rtp_time_ = std::chrono::steady_clock::now();
    std::println(stderr, "카메라 연결 성공: {}, 해상도 {}, payload type {}, 세션 timeout {}초", config_.camera_name,
                 DescribeResolution(video_track_.sprop_parameter_sets), video_track_.payload_type,
                 session_->timeout.count());

    rtp_watch_timer_id_ = loop_.AddTimer(config_.rtp_timeout, [this] { HandleRtpWatchTimer(); });
    // RTCP RR을 보내지 않으므로, timeout의 절반마다 요청을 보내 카메라가 세션을 만료시키지 않게 한다.
    keepalive_timer_id_ = loop_.AddTimer(std::chrono::duration_cast<std::chrono::milliseconds>(session_->timeout) / 2,
                                         [this] { HandleKeepaliveTimer(); });

    // 구독자가 이 알림 안에서 상태를 볼 수 있도록 내부 상태를 모두 바꾼 뒤 알린다.
    source_.PublishEvent(media::LiveVideoEvent::kConnected);
}


void RtspCameraClient::SendRequest(std::string_view method, std::string_view url, std::vector<RtspHeader> headers) {
    // SETUP 응답으로 세션이 정해진 뒤의 요청(PLAY, 유지 OPTIONS)은 모두 그 세션에 대한 것이다.
    if (session_) {
        headers.push_back(RtspHeader{"Session", session_->id});
    }
    pending_cseq_ = ++last_cseq_;
    send_buffer_ += BuildRtspRequest(method, url, *pending_cseq_, headers);

    loop_.CancelTimer(response_timer_id_);
    response_timer_id_ = loop_.AddTimer(config_.response_timeout, [this] {
        response_timer_id_ = 0;
        Fail(std::format("{} 응답 없음({}ms)", PendingMethodName(), config_.response_timeout.count()));
    });
    FlushSendBuffer();
}


bool RtspCameraClient::FlushSendBuffer() {
    while (!send_buffer_.empty()) {
        const ssize_t written =
            ::send(control_socket_.socket.Get(), send_buffer_.data(), send_buffer_.size(), MSG_NOSIGNAL);
        if (written > 0) {
            send_buffer_.erase(0, static_cast<std::size_t>(written));
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // 요청은 작아 거의 생기지 않지만, 생기면 나머지를 쓰기 가능 이벤트에서 마저 쓴다.
            loop_.Modify(control_socket_.socket.Get(), EPOLLIN | EPOLLOUT);
            return true;
        }
        Fail(std::format("제어 연결 송신 오류: {}", core::LastSystemError().message()));
        return false;
    }
    loop_.Modify(control_socket_.socket.Get(), EPOLLIN);
    return true;
}


bool RtspCameraClient::OpenMediaSockets() {
    for (int attempt = 0; attempt < kMaxPortPairAttempts; ++attempt) {
        // 커널이 고른 포트가 짝수이고 그다음 포트도 비어 있을 때만 쌍으로 쓴다(RFC 3550 11절).
        auto rtp = OpenBoundUdpSocket(0);
        if (!rtp) {
            Fail(std::format("RTP 소켓 생성 실패: {}", rtp.error().message()));
            return false;
        }
        if (rtp->second % 2 != 0 || rtp->second == 65535) {
            continue;
        }
        auto rtcp = OpenBoundUdpSocket(static_cast<std::uint16_t>(rtp->second + 1));
        if (!rtcp) {
            continue;
        }

        rtp_port_ = rtp->second;
        rtp_socket_.socket = std::move(rtp->first);
        rtcp_socket_.socket = std::move(rtcp->first);
        std::error_code add_error =
            loop_.Add(rtp_socket_.socket.Get(), EPOLLIN, [this](std::uint32_t) { ReceiveRtpPackets(); });
        rtp_socket_.is_registered = !add_error;
        if (!add_error) {
            add_error = loop_.Add(rtcp_socket_.socket.Get(), EPOLLIN, [this](std::uint32_t) { DrainRtcpPackets(); });
            rtcp_socket_.is_registered = !add_error;
        }
        if (add_error) {
            Fail(std::format("UDP 소켓 등록 실패: {}", add_error.message()));
            return false;
        }
        return true;
    }
    Fail(std::format("연속된 짝수·홀수 UDP 포트 쌍을 {}번 시도해도 잡지 못함", kMaxPortPairAttempts));
    return false;
}


void RtspCameraClient::ReceiveRtpPackets() {
    std::array<std::uint8_t, kDatagramBufferSize> buffer;
    for (int received_count = 0; received_count < kMaxDatagramsPerEvent; ++received_count) {
        sockaddr_in sender{};
        socklen_t address_size = sizeof(sender);
        const ssize_t received_size = ::recvfrom(rtp_socket_.socket.Get(), buffer.data(), buffer.size(), 0,
                                                 reinterpret_cast<sockaddr*>(&sender), &address_size);
        if (received_size < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }

        // 다른 호스트가 RTP 포트로 보낸 패킷이 영상에 섞이지 않게 한다.
        if (sender.sin_addr.s_addr != config_.address.s_addr) {
            if (!has_logged_foreign_packet_) {
                has_logged_foreign_packet_ = true;
                std::println(stderr, "카메라가 아닌 주소의 RTP 버림: {}, {}. 이 연결의 이후 패킷은 기록하지 않음",
                             config_.camera_name, core::FormatIpv4Endpoint(sender));
            }
            continue;
        }
        // PLAY 응답 전에 도착한 패킷은 구독자가 아직 연결을 모르므로 버린다.
        if (state_ != RtspCameraClientState::kPlaying) {
            continue;
        }
        last_rtp_time_ = std::chrono::steady_clock::now();
        depacketizer_->HandlePacket(std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(received_size)));
    }
}


void RtspCameraClient::DrainRtcpPackets() {
    std::array<std::uint8_t, kDatagramBufferSize> buffer;
    for (int received_count = 0; received_count < kMaxDatagramsPerEvent; ++received_count) {
        const ssize_t received_size = ::recv(rtcp_socket_.socket.Get(), buffer.data(), buffer.size(), 0);
        if (received_size < 0 && errno != EINTR) {
            return;
        }
    }
}


void RtspCameraClient::HandleRtpWatchTimer() {
    rtp_watch_timer_id_ = 0;
    const auto now = std::chrono::steady_clock::now();
    const auto deadline = last_rtp_time_ + config_.rtp_timeout;
    if (now >= deadline) {
        Fail(std::format("RTP가 {}ms 동안 없음", config_.rtp_timeout.count()));
        return;
    }
    rtp_watch_timer_id_ = loop_.AddTimer(std::chrono::ceil<std::chrono::milliseconds>(deadline - now),
                                         [this] { HandleRtpWatchTimer(); });
}


void RtspCameraClient::HandleKeepaliveTimer() {
    keepalive_timer_id_ = 0;
    const auto keepalive_interval = std::chrono::duration_cast<std::chrono::milliseconds>(session_->timeout) / 2;
    if (!pending_cseq_) {
        SendRequest("OPTIONS", config_.url.request_url);
    }
    // SendRequest가 송신 오류로 연결을 닫았으면 더 걸지 않는다.
    if (state_ == RtspCameraClientState::kPlaying) {
        keepalive_timer_id_ = loop_.AddTimer(keepalive_interval, [this] { HandleKeepaliveTimer(); });
    }
}


void RtspCameraClient::Fail(std::string_view reason) {
    const bool was_playing = state_ == RtspCameraClientState::kPlaying;
    CloseConnection();
    state_ = RtspCameraClientState::kBackoff;

    std::println(stderr, "카메라 {}: {}: {}. {}ms 뒤 다시 연결", was_playing ? "끊김" : "연결 실패", config_.camera_name,
                 reason, reconnect_delay_.count());
    reconnect_timer_id_ = loop_.AddTimer(reconnect_delay_, [this] { Connect(); });
    reconnect_delay_ = std::min(reconnect_delay_ * 2, config_.max_reconnect_delay);

    if (was_playing) {
        source_.PublishEvent(media::LiveVideoEvent::kDisconnected);
    }
}


void RtspCameraClient::CloseConnection() {
    CloseSocket(control_socket_);
    CloseSocket(rtp_socket_);
    CloseSocket(rtcp_socket_);
    for (core::EventLoop::TimerId* timer_id : {&response_timer_id_, &rtp_watch_timer_id_, &keepalive_timer_id_}) {
        loop_.CancelTimer(*timer_id);
        *timer_id = 0;
    }

    send_buffer_.clear();
    receive_buffer_.clear();
    last_cseq_ = 0;
    pending_cseq_.reset();
    session_.reset();
    track_url_.clear();
    aggregate_url_.clear();
    depacketizer_.reset();
    has_logged_foreign_packet_ = false;
}


void RtspCameraClient::CloseSocket(RegisteredSocket& registered) {
    if (registered.is_registered) {
        loop_.Remove(registered.socket.Get());
        registered.is_registered = false;
    }
    registered.socket.Reset();
}


std::string_view RtspCameraClient::PendingMethodName() const {
    switch (state_) {
        case RtspCameraClientState::kConnecting:
            return "TCP 연결";
        case RtspCameraClientState::kOptions:
            return "OPTIONS";
        case RtspCameraClientState::kDescribe:
            return "DESCRIBE";
        case RtspCameraClientState::kSetup:
            return "SETUP";
        case RtspCameraClientState::kPlay:
            return "PLAY";
        case RtspCameraClientState::kPlaying:
            return "유지 OPTIONS";
        case RtspCameraClientState::kStopped:
        case RtspCameraClientState::kBackoff:
            break;
    }
    return "요청";
}


}  // namespace vms::rtsp
