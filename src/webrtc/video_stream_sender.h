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
#include "media/h264_file.h"
#include "webrtc/srtp_video_packet_writer.h"


namespace vms::webrtc {


// 세션 하나의 동영상 송신 설정.
struct VideoStreamSenderConfig {
    // 재생 속도(초당 프레임 수). 1 이상이어야 한다.
    int frames_per_second = 30;


    // RTP 헤더에 넣을 SSRC. answer의 a=ssrc 값과 같아야 한다.
    std::uint32_t ssrc = 0;


    // RTP 헤더에 넣을 H.264 payload type. answer에서 정한 값이다.
    std::uint8_t payload_type = 0;


    // 첫 패킷의 순번. 비어 있으면 무작위로 정한다. 테스트가 순번 순환을 재현하는 데 쓴다.
    std::optional<std::uint16_t> initial_sequence_number;


    // 첫 프레임의 RTP 타임스탬프. 비어 있으면 무작위로 정한다.
    std::optional<std::uint32_t> initial_timestamp;
};


// 프레임 하나를 보낸 결과.
struct VideoFrameTransmission {
    // 보낼 SRTP 패킷. 순서대로 보내야 한다. 아직 예정 시각이 아니면 비어 있다.
    std::vector<std::vector<std::uint8_t>> packets;


    // 다음 프레임의 예정 시각. 이 시각에 SendDueFrame을 다시 호출한다.
    std::chrono::steady_clock::time_point next_due_time;
};


// 세션 하나에 동영상 프레임을 재생 속도에 맞춰 RTP로 패킷화하고 SRTP로 보호하는 송신 상태.
//
// 소켓과 루프를 모르며, 호출자가 시각을 넘기고 돌려받은 패킷을 보낸 뒤 다음 예정 시각에 타이머를 건다.
// 보낸 프레임 수 n을 계속 늘리며 소스 위치는 n mod (프레임 수)이므로, 파일 끝에서 별도 분기 없이 처음으로
// 돌아가고 순번과 타임스탬프가 이어진다. 이 객체는 일정과 타임스탬프만 정하고, 패킷화·RTP 헤더·SRTP 보호는
// 소유한 SrtpVideoPacketWriter가 맡는다. H264File은 참조만 하므로 이 객체보다 오래 살아 있어야 한다.
class VideoStreamSender {
public:
    // 송신 상태를 만든다.
    //
    // video_file: 보낼 동영상. 참조만 하므로 반환된 객체보다 오래 살아 있어야 한다.
    // config: 재생 속도, SSRC, payload type, 순번·타임스탬프 초기값
    // master_key: 서버가 보내는 패킷용 SRTP 마스터 키
    // master_salt: 서버가 보내는 패킷용 SRTP 마스터 솔트
    // start_time: 첫 프레임의 예정 시각
    // 반환값: 송신 상태. 난수 생성이나 SRTP 키 유도가 실패하면 오류 메시지
    static std::expected<std::unique_ptr<VideoStreamSender>, std::string> Create(
        const media::H264File& video_file, const VideoStreamSenderConfig& config,
        std::span<const std::uint8_t, crypto::kSrtpMasterKeySize> master_key,
        std::span<const std::uint8_t, crypto::kSrtpMasterSaltSize> master_salt,
        std::chrono::steady_clock::time_point start_time);


    // 예정 시각이 된 프레임 하나의 SRTP 패킷을 만든다.
    //
    // now: 현재 시각
    // 반환값: 보낼 패킷과 다음 예정 시각. SRTP 보호가 실패하면 오류 메시지이며, 그 뒤로는 이 객체를 쓰지 않는다.
    //
    // now가 예정 시각보다 이르면 패킷 없이 같은 예정 시각을 돌려준다. 예정 시각이 이미 한 프레임 간격 넘게
    // 지났으면 밀린 프레임을 몰아 보내지 않도록 일정의 기준을 now로 다시 잡는다. 타임스탬프는 n으로
    // 계산하므로 재설정해도 프레임 간격이 유지된다. 프레임의 마지막 패킷에만 marker 비트를 켠다.
    std::expected<VideoFrameTransmission, std::string> SendDueFrame(std::chrono::steady_clock::time_point now);


private:
    // 구성한 상태로 객체를 만든다. Create()에서만 호출한다.
    VideoStreamSender(const media::H264File& video_file, int frames_per_second, SrtpVideoPacketWriter packet_writer,
                      std::uint32_t initial_timestamp, std::chrono::steady_clock::time_point start_time);


    // 프레임 번호의 예정 시각을 계산한다.
    //
    // frame_number: 송신 시작부터의 프레임 번호
    // 반환값: schedule_origin_ + (frame_number - schedule_origin_frame_) / fps
    std::chrono::steady_clock::time_point DueTimeOf(std::uint64_t frame_number) const;


    // 보낼 동영상. 이 객체보다 오래 산다.
    const media::H264File& video_file_;


    // 재생 속도(초당 프레임 수)
    int frames_per_second_;


    // 패킷화·RTP 헤더·SRTP 보호와 순번 상태
    SrtpVideoPacketWriter packet_writer_;


    // 첫 프레임의 RTP 타임스탬프. 프레임 n의 타임스탬프는 이 값에 ⌊n × 90000 / fps⌋를 더한 값이다.
    std::uint32_t initial_timestamp_;


    // 지금까지 보낸 프레임 수 n. 반복 재생해도 계속 늘어난다.
    std::uint64_t sent_frame_count_ = 0;


    // 일정 계산의 기준 시각. 처음에는 송신 시작 시각이고, 늦어지면 그때의 현재 시각으로 바뀐다.
    std::chrono::steady_clock::time_point schedule_origin_;


    // schedule_origin_에 보내기로 한 프레임 번호
    std::uint64_t schedule_origin_frame_ = 0;
};


}  // namespace vms::webrtc
