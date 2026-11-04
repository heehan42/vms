#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "crypto/srtp.h"
#include "media/live_video_source.h"
#include "webrtc/srtp_video_packet_writer.h"


namespace vms::webrtc {


// 세션 하나의 카메라 송신 설정.
struct LiveVideoStreamSenderConfig {
    // RTP 헤더에 넣을 SSRC. answer의 a=ssrc 값과 같아야 한다.
    std::uint32_t ssrc = 0;


    // RTP 헤더에 넣을 H.264 payload type. answer에서 정한 값이다.
    std::uint8_t payload_type = 0;


    // 첫 패킷의 순번. 비어 있으면 무작위로 정한다. 테스트용
    std::optional<std::uint16_t> initial_sequence_number;


    // 처음 보내는 키프레임의 세션 RTP 타임스탬프. 비어 있으면 무작위로 정한다. 테스트용
    std::optional<std::uint32_t> initial_timestamp;
};


// 카메라 송신 상태
enum class LiveVideoStreamState {
    // 카메라는 연결되어 있고 다음 키프레임을 기다림. 그 전의 프레임은 보내지 않는다.
    kWaitingForKeyframe,

    // 키프레임부터 받은 프레임을 보내는 중
    kSending,

    // 카메라 연결이 끊겨 다시 연결되기를 기다림
    kWaitingForCamera,
};


// 세션 하나에 카메라 프레임을 도착하는 대로 RTP로 패킷화하고 SRTP로 보호하는 송신 상태.
//
// 소켓과 루프를 모르며, 호출자가 LiveVideoSource 구독 콜백에서 받은 프레임·이벤트와 현재 시각을 넘긴다. 세션마다 따로
// 키프레임을 기다리고, 손실 뒤에도 다음 키프레임까지 보내지 않는다. 세션 타임스탬프는 처음 보낸 키프레임을 기준으로
// 카메라 타임스탬프 차이를 그대로 더해 만든다(32비트 순환). 카메라가 다시 연결되면 카메라 타임스탬프 원점이 바뀔 수
// 있으므로, 마지막으로 보낸 타임스탬프에 끊긴 동안의 경과 시간(90kHz)을 더한 값을 새 기준으로 삼는다. 순번은 끊기지
// 않는다. 패킷화·RTP 헤더·SRTP 보호는 소유한 SrtpVideoPacketWriter가 맡는다.
class LiveVideoStreamSender {
public:
    // 송신 상태를 만든다.
    //
    // config: SSRC, payload type, 순번·타임스탬프 초기값
    // master_key: 서버가 보내는 패킷용 SRTP 마스터 키
    // master_salt: 서버가 보내는 패킷용 SRTP 마스터 솔트
    // is_camera_connected: 만드는 시점의 카메라 연결 상태. true이면 kWaitingForKeyframe, false이면
    //   kWaitingForCamera로 시작한다.
    // 반환값: 송신 상태. 난수 생성이나 SRTP 키 유도가 실패하면 오류 메시지
    static std::expected<std::unique_ptr<LiveVideoStreamSender>, std::string> Create(
        const LiveVideoStreamSenderConfig& config, std::span<const std::uint8_t, crypto::kSrtpMasterKeySize> master_key,
        std::span<const std::uint8_t, crypto::kSrtpMasterSaltSize> master_salt, bool is_camera_connected);


    // 카메라 프레임 하나를 처리한다.
    //
    // frame: 구독 콜백이 받은 프레임. 호출 동안만 참조한다.
    // now: 현재 시각. 재연결 뒤 타임스탬프 기준을 정할 때 쓴다.
    // 반환값: 순서대로 보낼 SRTP 패킷. 보내지 않는 상태이거나 키프레임을 기다리는 중이면 비어 있다.
    //   SRTP 보호가 실패하면 오류 메시지이며, 그 뒤로는 이 객체를 쓰지 않는다.
    //
    // kWaitingForKeyframe에서 키프레임이 오면 kSending이 되고 그 프레임부터 보낸다.
    std::expected<std::vector<std::vector<std::uint8_t>>, std::string> HandleFrame(
        const media::LiveVideoFrame& frame, std::chrono::steady_clock::time_point now);


    // 카메라 상태 변화를 반영한다.
    //
    // event: 구독 콜백이 받은 상태 변화
    //
    // kLoss는 kSending을 kWaitingForKeyframe으로, kDisconnected는 어느 상태든 kWaitingForCamera로,
    // kConnected는 kWaitingForCamera를 kWaitingForKeyframe으로 바꾼다. kDisconnected 뒤 처음 보내는 키프레임에서
    // 타임스탬프 기준을 다시 정한다.
    void HandleEvent(media::LiveVideoEvent event);


    // 현재 송신 상태를 돌려준다.
    LiveVideoStreamState State() const { return state_; }


private:
    // 마지막으로 보낸 프레임의 기록. 재연결 뒤 타임스탬프 기준 계산에 쓴다.
    struct SentFrameRecord {
        // 보낸 세션 타임스탬프
        std::uint32_t session_timestamp = 0;


        // 보낸 시각
        std::chrono::steady_clock::time_point sent_time;
    };


    // 구성한 상태로 객체를 만든다. Create()에서만 호출한다.
    LiveVideoStreamSender(SrtpVideoPacketWriter packet_writer, std::uint32_t initial_timestamp,
                          bool is_camera_connected);


    // 키프레임으로 송신을 시작할 때 타임스탬프 기준을 정한다.
    //
    // camera_timestamp: 시작하는 키프레임의 카메라 타임스탬프
    // now: 현재 시각
    //
    // 처음이면 초기값을, 카메라가 다시 연결된 뒤면 마지막 송신 타임스탬프에 경과 시간을 더한 값을 기준으로 삼는다.
    // 같은 카메라 연결 안의 손실 뒤에는 카메라 타임스탬프가 이어지므로 기준을 그대로 둔다.
    void RebaseTimestampIfNeeded(std::uint32_t camera_timestamp, std::chrono::steady_clock::time_point now);


    // 패킷화·RTP 헤더·SRTP 보호와 순번 상태
    SrtpVideoPacketWriter packet_writer_;


    // 처음 보내는 키프레임의 세션 타임스탬프
    std::uint32_t initial_timestamp_;


    // 현재 송신 상태
    LiveVideoStreamState state_;


    // session_ts = session_base_timestamp_ + (camera_ts - camera_base_timestamp_)의 카메라 쪽 기준
    std::uint32_t camera_base_timestamp_ = 0;


    // 위 식의 세션 쪽 기준
    std::uint32_t session_base_timestamp_ = 0;


    // 두 기준 값이 지금 카메라 연결의 타임스탬프에 맞는지 여부. 처음과 kDisconnected 뒤에는 false다.
    bool is_timestamp_base_valid_ = false;


    // 마지막으로 보낸 프레임. 아직 보내지 않았으면 비어 있다.
    std::optional<SentFrameRecord> last_sent_frame_;
};


}  // namespace vms::webrtc
