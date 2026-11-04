#include "rtsp/rtsp_camera_client.h"

#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/event_loop.h"
#include "core/unique_fd.h"
#include "media/live_video_source.h"
#include "test_support.h"
#include "webrtc/rtp_packet.h"

using vms::core::EventLoop;
using vms::core::UniqueFd;
using vms::media::LiveVideoEvent;
using vms::media::LiveVideoFrame;
using vms::media::LiveVideoSource;
using vms::rtsp::RtspCameraClient;
using vms::rtsp::RtspCameraClientConfig;
using vms::rtsp::RtspCameraClientState;
using vms::test::Expect;

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;


namespace {


// 가짜 서버가 보내는 RTP의 payload type
constexpr std::uint8_t kPayloadType = 96;


// vms-cam의 640x480 SPS·PPS
constexpr std::string_view kSpropParameterSets = "Z0LAHtoCgPbAWoMDA1KAAAADAIAAAB5HixdQ,aM48gA==";


// 가짜 서버가 받은 요청 하나.
struct FakeRequest {
    // 메서드
    std::string method;


    // 요청 URL
    std::string url;


    // 헤더를 포함한 요청 원문
    std::string text;
};


// 가짜 서버의 응답 방식
enum class FakeServerMode {
    // 모든 요청에 정상 응답하고 PLAY 뒤 RTP를 보낸다.
    kNormal,

    // DESCRIBE에 404로 응답한다.
    kDescribeNotFound,

    // 요청을 받기만 하고 응답하지 않는다.
    kSilent,

    // 연결을 받자마자 닫는다.
    kCloseOnAccept,
};


// 같은 이벤트 루프에서 도는 가짜 RTSP 서버. TCP로 요청에 응답하고 PLAY 뒤 UDP로 H.264 RTP를 보낸다.
//
// 루프 핸들러와 타이머가 이 객체의 주소를 캡처하므로 이동하지 않으며, 루프보다 먼저 소멸해야 한다.
class FakeRtspServer {
public:
    // 127.0.0.1의 빈 포트에서 연결을 기다린다.
    //
    // loop: 소켓과 타이머를 등록할 루프
    // rtp_source_address: RTP를 보낼 출발 주소. 카메라 주소와 다르게 하면 출발 주소 검사를 시험할 수 있다.
    explicit FakeRtspServer(EventLoop& loop, std::string_view rtp_source_address = "127.0.0.1") : loop_(loop) {
        listen_socket_.Reset(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        sockaddr_in address = MakeAddress("127.0.0.1", 0);
        ::bind(listen_socket_.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        ::listen(listen_socket_.Get(), 8);
        socklen_t address_size = sizeof(address);
        ::getsockname(listen_socket_.Get(), reinterpret_cast<sockaddr*>(&address), &address_size);
        port_ = ntohs(address.sin_port);
        loop_.Add(listen_socket_.Get(), EPOLLIN, [this](std::uint32_t) { Accept(); });

        rtp_socket_.Reset(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
        sockaddr_in rtp_address = MakeAddress(rtp_source_address, 0);
        ::bind(rtp_socket_.Get(), reinterpret_cast<const sockaddr*>(&rtp_address), sizeof(rtp_address));
    }


    // 등록과 타이머를 정리한다.
    ~FakeRtspServer() {
        StopRtp();
        CloseConnection();
        loop_.Remove(listen_socket_.Get());
    }


    // 대기 포트
    std::uint16_t Port() const { return port_; }


    // 지금 연결을 닫고 RTP 송신을 멈춘다.
    void CloseConnection() {
        StopRtp();
        if (connection_.IsValid()) {
            loop_.Remove(connection_.Get());
            connection_.Reset();
        }
        receive_buffer_.clear();
    }


    // RTP 송신을 멈춘다. 연결은 그대로 둔다.
    void StopRtp() {
        loop_.CancelTimer(rtp_timer_id_);
        rtp_timer_id_ = 0;
    }


    // 응답 방식. 다음 요청이나 연결부터 적용한다.
    FakeServerMode mode = FakeServerMode::kNormal;


    // SETUP 응답 Session의 timeout(초)
    int session_timeout_seconds = 60;


    // 받은 요청. 연결이 바뀌어도 이어서 쌓는다.
    std::vector<FakeRequest> requests;


    // 연결을 받은 시각
    std::vector<Clock::time_point> accept_times;


    // SETUP의 client_port 쌍. 아직 받지 않았으면 비어 있다.
    std::optional<std::pair<int, int>> client_ports;


private:
    // 점 표기 주소와 포트로 주소 구조체를 만든다.
    static sockaddr_in MakeAddress(std::string_view text, std::uint16_t port) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        ::inet_pton(AF_INET, std::string(text).c_str(), &address.sin_addr);
        return address;
    }


    // 대기 중인 연결을 받는다. 이전 연결은 닫는다.
    void Accept() {
        UniqueFd accepted(::accept4(listen_socket_.Get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
        if (!accepted.IsValid()) {
            return;
        }
        accept_times.push_back(Clock::now());
        CloseConnection();
        if (mode == FakeServerMode::kCloseOnAccept) {
            return;
        }
        connection_ = std::move(accepted);
        loop_.Add(connection_.Get(), EPOLLIN, [this](std::uint32_t) { ReceiveRequests(); });
    }


    // 받은 바이트에서 요청을 꺼내 응답한다.
    void ReceiveRequests() {
        std::array<char, 4096> buffer;
        const ssize_t size = ::recv(connection_.Get(), buffer.data(), buffer.size(), 0);
        if (size <= 0) {
            CloseConnection();
            return;
        }
        receive_buffer_.append(buffer.data(), static_cast<std::size_t>(size));

        std::size_t end = 0;
        while ((end = receive_buffer_.find("\r\n\r\n")) != std::string::npos) {
            const std::string text = receive_buffer_.substr(0, end + 4);
            receive_buffer_.erase(0, end + 4);
            const std::size_t method_end = text.find(' ');
            const std::size_t url_end = text.find(' ', method_end + 1);
            requests.push_back(FakeRequest{.method = text.substr(0, method_end),
                                           .url = text.substr(method_end + 1, url_end - method_end - 1),
                                           .text = text});
            Respond(requests.back());
            if (!connection_.IsValid()) {
                return;
            }
        }
    }


    // 요청 하나에 응답 방식대로 응답한다.
    //
    // request: 받은 요청
    void Respond(const FakeRequest& request) {
        if (mode == FakeServerMode::kSilent || request.method == "TEARDOWN") {
            return;
        }
        const std::size_t cseq_begin = request.text.find("CSeq: ") + 6;
        const std::string cseq = request.text.substr(cseq_begin, request.text.find("\r\n", cseq_begin) - cseq_begin);

        std::string response;
        if (request.method == "DESCRIBE" && mode == FakeServerMode::kDescribeNotFound) {
            response = std::format("RTSP/1.0 404 Not Found\r\nCSeq: {}\r\n\r\n", cseq);
        } else if (request.method == "DESCRIBE") {
            const std::string sdp = std::format(
                "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=fake\r\nt=0 0\r\na=control:*\r\nm=video 0 RTP/AVP {0}\r\n"
                "a=rtpmap:{0} H264/90000\r\na=fmtp:{0} packetization-mode=1;sprop-parameter-sets={1}\r\n"
                "a=control:stream=0\r\n",
                kPayloadType, kSpropParameterSets);
            response = std::format(
                "RTSP/1.0 200 OK\r\nCSeq: {}\r\nContent-Type: application/sdp\r\nContent-Base: "
                "rtsp://127.0.0.1:{}/cam/\r\nContent-Length: {}\r\n\r\n{}",
                cseq, port_, sdp.size(), sdp);
        } else if (request.method == "SETUP") {
            int rtp_port = 0;
            int rtcp_port = 0;
            const std::size_t ports = request.text.find("client_port=");
            std::sscanf(request.text.c_str() + ports, "client_port=%d-%d", &rtp_port, &rtcp_port);
            client_ports = std::pair(rtp_port, rtcp_port);
            response = std::format(
                "RTSP/1.0 200 OK\r\nCSeq: {}\r\nTransport: RTP/AVP;unicast;client_port={}-{};server_port=6970-6971\r\n"
                "Session: fake1234;timeout={}\r\n\r\n",
                cseq, rtp_port, rtcp_port, session_timeout_seconds);
        } else {
            response = std::format("RTSP/1.0 200 OK\r\nCSeq: {}\r\n\r\n", cseq);
        }
        ::send(connection_.Get(), response.data(), response.size(), MSG_NOSIGNAL);

        if (request.method == "PLAY" && client_ports) {
            StopRtp();
            frame_number_ = 0;
            rtp_timer_id_ = loop_.AddTimer(0ms, [this] { SendFrame(); });
        }
    }


    // 프레임 하나를 RTP로 보내고 다음 프레임 타이머를 건다. 다섯 프레임마다 키프레임이다.
    void SendFrame() {
        rtp_timer_id_ = 0;
        const sockaddr_in destination = MakeAddress("127.0.0.1", static_cast<std::uint16_t>(client_ports->first));
        const auto timestamp = static_cast<std::uint32_t>(frame_number_ * 3000);
        const auto send_payload = [&](std::vector<std::uint8_t> payload, bool marker) {
            const std::vector<std::uint8_t> packet =
                vms::webrtc::BuildRtpPacket(vms::webrtc::RtpHeader{.marker = marker,
                                                                   .payload_type = kPayloadType,
                                                                   .sequence_number = sequence_number_++,
                                                                   .timestamp = timestamp,
                                                                   .ssrc = 0x1234},
                                            payload);
            ::sendto(rtp_socket_.Get(), packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr*>(&destination),
                     sizeof(destination));
        };
        if (frame_number_ % 5 == 0) {
            send_payload({0x78, 0x00, 0x03, 0x67, 0x42, 0xC0, 0x00, 0x02, 0x68, 0xCE}, false);
            send_payload({0x65, 0x88, 0x84, 0x21}, true);
        } else {
            send_payload({0x41, 0x9A, 0x02, 0x03}, true);
        }

        // RTCP 포트에도 패킷을 보내 클라이언트가 읽어 버리는지 본다.
        const sockaddr_in rtcp_destination =
            MakeAddress("127.0.0.1", static_cast<std::uint16_t>(client_ports->second));
        const std::array<std::uint8_t, 8> rtcp = {0x80, 200, 0x00, 0x01, 0x00, 0x00, 0x12, 0x34};
        ::sendto(rtp_socket_.Get(), rtcp.data(), rtcp.size(), 0, reinterpret_cast<const sockaddr*>(&rtcp_destination),
                 sizeof(rtcp_destination));

        ++frame_number_;
        rtp_timer_id_ = loop_.AddTimer(20ms, [this] { SendFrame(); });
    }


    // 소켓과 타이머를 등록한 루프
    EventLoop& loop_;


    // 연결을 기다리는 TCP 소켓
    UniqueFd listen_socket_;


    // 지금 연결
    UniqueFd connection_;


    // RTP와 RTCP를 보내는 UDP 소켓
    UniqueFd rtp_socket_;


    // 대기 포트
    std::uint16_t port_ = 0;


    // 아직 요청으로 꺼내지 않은 바이트
    std::string receive_buffer_;


    // 걸려 있는 RTP 송신 타이머. 없으면 0
    EventLoop::TimerId rtp_timer_id_ = 0;


    // PLAY 뒤 보낸 프레임 수
    std::uint64_t frame_number_ = 0;


    // 다음 RTP 순번
    std::uint16_t sequence_number_ = 100;
};


// 소스가 구독자에게 넘긴 것을 기록한다.
struct SourceRecorder {
    // 받은 프레임 수
    int frame_count = 0;


    // 받은 키프레임 수
    int keyframe_count = 0;


    // 받은 이벤트
    std::vector<LiveVideoEvent> events;


    // source를 구독한다. 이 객체와 source가 함께 살아 있는 동안만 쓴다.
    void SubscribeTo(LiveVideoSource& source) {
        source.Subscribe(
            [this](const LiveVideoFrame& frame) {
                ++frame_count;
                keyframe_count += frame.is_keyframe ? 1 : 0;
            },
            [this](LiveVideoEvent event) { events.push_back(event); });
    }


    // event를 받은 수
    int CountOf(LiveVideoEvent event) const { return static_cast<int>(std::ranges::count(events, event)); }
};


// 조건이 참이 되거나 제한 시간이 지날 때까지 루프를 돌린다.
//
// loop: 돌릴 루프
// condition: 5ms마다 확인할 조건
// limit: 최대 대기 시간
// 반환값: 조건이 참이 되어 멈췄으면 true
bool RunUntil(EventLoop& loop, const std::function<bool()>& condition, Clock::duration limit) {
    const Clock::time_point deadline = Clock::now() + limit;
    bool is_satisfied = false;
    std::function<void()> check = [&] {
        if (condition()) {
            is_satisfied = true;
            loop.Stop();
        } else if (Clock::now() >= deadline) {
            loop.Stop();
        } else {
            loop.AddTimer(5ms, [&] { check(); });
        }
    };
    loop.AddTimer(0ms, [&] { check(); });
    loop.Run();
    return is_satisfied;
}


// 테스트용 짧은 시간 값으로 클라이언트 설정을 만든다.
//
// port: 가짜 서버 포트
// 반환값: 응답 제한 300ms, RTP 감시 300ms, 재연결 50ms부터 최대 200ms인 설정
RtspCameraClientConfig MakeConfig(std::uint16_t port) {
    RtspCameraClientConfig config;
    config.camera_name = "fake";
    config.url = *vms::rtsp::ParseRtspUrl(std::format("rtsp://127.0.0.1:{}/cam", port));
    ::inet_pton(AF_INET, "127.0.0.1", &config.address);
    config.response_timeout = 300ms;
    config.rtp_timeout = 300ms;
    config.initial_reconnect_delay = 50ms;
    config.max_reconnect_delay = 200ms;
    return config;
}


// 정상 절차의 요청 순서·CSeq·헤더, client_port 쌍, 프레임 전달, 유지 OPTIONS의 Session, Stop()의 TEARDOWN을 검사한다.
void TestNormalSession(EventLoop& loop) {
    FakeRtspServer server(loop);
    server.session_timeout_seconds = 1;
    LiveVideoSource source;
    SourceRecorder recorder;
    recorder.SubscribeTo(source);
    auto client = RtspCameraClient::Create(loop, MakeConfig(server.Port()), source);
    client->Start();

    Expect(RunUntil(loop, [&] { return recorder.keyframe_count >= 2; }, 2s), "PLAY 뒤 키프레임이 소스로 전달됨");
    Expect(client->State() == RtspCameraClientState::kPlaying && source.IsConnected(), "kPlaying, 소스 연결됨");
    Expect(recorder.frame_count > recorder.keyframe_count, "키프레임이 아닌 프레임도 전달됨");

    const std::vector<std::string> expected_methods = {"OPTIONS", "DESCRIBE", "SETUP", "PLAY"};
    if (server.requests.size() < expected_methods.size()) {
        Expect(false, "요청 네 개 이상");
        return;
    }
    for (std::size_t index = 0; index < expected_methods.size(); ++index) {
        const FakeRequest& request = server.requests[index];
        Expect(request.method == expected_methods[index], "요청 순서: " + request.method);
        Expect(request.text.contains(std::format("\r\nCSeq: {}\r\n", index + 1)), "CSeq는 1부터 1씩 늘어남");
    }
    const std::string base_url = std::format("rtsp://127.0.0.1:{}/cam", server.Port());
    Expect(server.requests[0].url == base_url && server.requests[1].url == base_url, "OPTIONS·DESCRIBE는 카메라 URL");
    Expect(server.requests[1].text.contains("Accept: application/sdp"), "DESCRIBE의 Accept");
    Expect(server.requests[2].url == base_url + "/stream=0", "SETUP은 Content-Base 기준 control URL");
    Expect(server.requests[2].text.contains("Transport: RTP/AVP;unicast;client_port="), "SETUP의 Transport");
    Expect(server.client_ports && server.client_ports->first % 2 == 0 &&
               server.client_ports->second == server.client_ports->first + 1,
           "client_port는 짝수·홀수 쌍");
    Expect(server.requests[3].url == base_url + "/" && server.requests[3].text.contains("Session: fake1234"),
           "PLAY는 세션 수준 control(*)인 Content-Base로 보내고 Session을 넣음");

    // timeout 1초이므로 0.5초마다 유지 OPTIONS를 보낸다.
    Expect(RunUntil(loop, [&] { return server.requests.size() >= 6; }, 2s), "유지 OPTIONS를 주기적으로 보냄");
    Expect(server.requests.size() >= 5 && server.requests[4].method == "OPTIONS" &&
               server.requests[4].text.contains("Session: fake1234"),
           "유지 OPTIONS에 Session");
    Expect(client->State() == RtspCameraClientState::kPlaying, "유지 중에도 kPlaying");

    const std::size_t request_count = server.requests.size();
    client->Stop();
    Expect(RunUntil(loop, [&] { return server.requests.size() > request_count; }, 1s) &&
               server.requests.back().method == "TEARDOWN" && server.requests.back().text.contains("Session: fake1234"),
           "Stop()은 Session을 넣은 TEARDOWN을 보냄");
    Expect(client->State() == RtspCameraClientState::kStopped && !source.IsConnected() &&
               recorder.CountOf(LiveVideoEvent::kDisconnected) == 1,
           "Stop() 뒤 kStopped, 소스에 끊김 알림");
}


// DESCRIBE 404면 연결 실패로 보고 재연결 간격 뒤 다시 시도하는지 검사한다.
void TestDescribeNotFoundRetries(EventLoop& loop) {
    FakeRtspServer server(loop);
    server.mode = FakeServerMode::kDescribeNotFound;
    LiveVideoSource source;
    SourceRecorder recorder;
    recorder.SubscribeTo(source);
    auto client = RtspCameraClient::Create(loop, MakeConfig(server.Port()), source);
    client->Start();

    Expect(RunUntil(loop, [&] { return server.accept_times.size() >= 2; }, 1s), "404 뒤 다시 연결");
    Expect(recorder.events.empty(), "PLAY 전 실패는 소스에 알리지 않음");
    const std::size_t describe_count = static_cast<std::size_t>(
        std::ranges::count_if(server.requests, [](const FakeRequest& request) { return request.method == "DESCRIBE"; }));
    Expect(describe_count >= 1 && std::ranges::none_of(server.requests, [](const FakeRequest& request) {
               return request.method == "SETUP";
           }),
           "404 뒤에는 SETUP을 보내지 않음");
}


// 응답이 없으면 제한 시간 뒤 끊고 다시 연결하는지 검사한다.
void TestResponseTimeout(EventLoop& loop) {
    FakeRtspServer server(loop);
    server.mode = FakeServerMode::kSilent;
    LiveVideoSource source;
    auto client = RtspCameraClient::Create(loop, MakeConfig(server.Port()), source);
    const Clock::time_point start = Clock::now();
    client->Start();

    Expect(RunUntil(loop, [&] { return server.accept_times.size() >= 2; }, 2s), "응답이 없으면 다시 연결");
    if (server.accept_times.size() >= 2) {
        Expect(server.accept_times[1] - start >= 300ms + 50ms, "응답 제한 시간과 재연결 간격 뒤 다시 연결");
    }
}


// RTP가 끊기면 감시 시간 뒤 kDisconnected를 알리고, 다시 연결되면 간격을 처음으로 되돌리는지 검사한다.
void TestRtpStopAndReconnect(EventLoop& loop) {
    FakeRtspServer server(loop);
    LiveVideoSource source;
    SourceRecorder recorder;
    recorder.SubscribeTo(source);
    auto client = RtspCameraClient::Create(loop, MakeConfig(server.Port()), source);
    client->Start();
    if (!RunUntil(loop, [&] { return recorder.frame_count > 0; }, 2s)) {
        Expect(false, "처음 연결");
        return;
    }

    server.StopRtp();
    const Clock::time_point stopped_at = Clock::now();
    Expect(RunUntil(loop, [&] { return recorder.CountOf(LiveVideoEvent::kDisconnected) == 1; }, 2s),
           "RTP가 멈추면 끊김 이벤트");
    Expect(Clock::now() - stopped_at >= 250ms, "끊김은 RTP 감시 시간 뒤");

    // 재연결은 성공하므로(서버는 정상 응답) 다시 kConnected가 온다.
    Expect(RunUntil(loop, [&] { return recorder.CountOf(LiveVideoEvent::kConnected) == 2; }, 2s), "다시 연결됨");
    Expect(server.accept_times.size() == 2, "재연결은 한 번");
}


// 서버가 연결을 닫으면 끊김 이벤트를 알리고, 연속 실패마다 재연결 간격이 두 배로 늘어나 상한에서 멈추는지 검사한다.
void TestServerCloseAndBackoff(EventLoop& loop) {
    FakeRtspServer server(loop);
    LiveVideoSource source;
    SourceRecorder recorder;
    recorder.SubscribeTo(source);
    auto client = RtspCameraClient::Create(loop, MakeConfig(server.Port()), source);
    client->Start();
    if (!RunUntil(loop, [&] { return recorder.frame_count > 0; }, 2s)) {
        Expect(false, "처음 연결");
        return;
    }

    server.mode = FakeServerMode::kCloseOnAccept;
    server.CloseConnection();
    Expect(RunUntil(loop, [&] { return recorder.CountOf(LiveVideoEvent::kDisconnected) == 1; }, 1s),
           "서버가 연결을 닫으면 끊김 이벤트");
    Expect(RunUntil(loop, [&] { return server.accept_times.size() >= 6; }, 3s), "계속 다시 시도");

    // 끊긴 직후 간격은 50ms이고, 이후 연속 실패마다 100, 200, 200, 200ms로 늘어난다(상한 200ms).
    // 연결 시도에 걸리는 시간만큼 조금 늘어날 수 있다.
    const std::vector<Clock::duration> expected = {100ms, 200ms, 200ms, 200ms};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const Clock::duration interval = server.accept_times[index + 2] - server.accept_times[index + 1];
        Expect(interval >= expected[index] && interval < expected[index] + 80ms,
               std::format("재연결 간격 {}번째: {}ms", index + 1,
                           std::chrono::duration_cast<std::chrono::milliseconds>(interval).count()));
    }
}


// 연결 성공 뒤 끊기면 다음 재연결 간격이 처음 값으로 돌아가는지 끊김 시각부터 재어 검사한다.
void TestReconnectDelayResetsAfterSuccess(EventLoop& loop) {
    FakeRtspServer server(loop);
    server.mode = FakeServerMode::kCloseOnAccept;
    LiveVideoSource source;
    SourceRecorder recorder;
    recorder.SubscribeTo(source);
    auto client = RtspCameraClient::Create(loop, MakeConfig(server.Port()), source);
    client->Start();

    // 세 번 실패시켜 간격을 200ms까지 늘린 뒤 성공시킨다.
    RunUntil(loop, [&] { return server.accept_times.size() >= 3; }, 2s);
    server.mode = FakeServerMode::kNormal;
    if (!RunUntil(loop, [&] { return recorder.frame_count > 0; }, 2s)) {
        Expect(false, "실패 뒤 연결 성공");
        return;
    }

    server.mode = FakeServerMode::kCloseOnAccept;
    const std::size_t accepts_before = server.accept_times.size();
    server.CloseConnection();
    const Clock::time_point closed_at = Clock::now();
    Expect(RunUntil(loop, [&] { return server.accept_times.size() > accepts_before; }, 1s), "끊긴 뒤 다시 시도");
    const Clock::duration interval = server.accept_times.back() - closed_at;
    Expect(interval < 50ms + 80ms,
           std::format("성공 뒤 첫 재연결 간격은 처음 값: {}ms",
                       std::chrono::duration_cast<std::chrono::milliseconds>(interval).count()));
}


// 카메라 주소가 아닌 곳에서 온 RTP는 재조립에 쓰지 않는지 검사한다.
void TestDropsForeignRtp(EventLoop& loop) {
    FakeRtspServer server(loop, "127.0.0.2");
    LiveVideoSource source;
    SourceRecorder recorder;
    recorder.SubscribeTo(source);
    auto client = RtspCameraClient::Create(loop, MakeConfig(server.Port()), source);
    client->Start();

    RunUntil(loop, [&] { return recorder.CountOf(LiveVideoEvent::kConnected) == 1; }, 2s);
    RunUntil(loop, [] { return false; }, 200ms);
    Expect(recorder.CountOf(LiveVideoEvent::kConnected) == 1 && recorder.frame_count == 0,
           "다른 주소의 RTP로는 프레임을 만들지 않음");
}


}  // namespace


int main() {
    auto loop = EventLoop::Create();
    if (!loop) {
        Expect(false, "이벤트 루프 생성");
        return vms::test::ExitCode();
    }

    TestNormalSession(*loop);
    TestDescribeNotFoundRetries(*loop);
    TestResponseTimeout(*loop);
    TestRtpStopAndReconnect(*loop);
    TestServerCloseAndBackoff(*loop);
    TestReconnectDelayResetsAfterSuccess(*loop);
    TestDropsForeignRtp(*loop);
    return vms::test::ExitCode();
}
