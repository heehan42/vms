#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "media/h264_file.h"


namespace vms::rtsp {


// 재조립한 프레임 하나로 받아들이는 최대 크기(바이트). 넘으면 버리고 손실로 알린다.
inline constexpr std::size_t kMaxDepacketizedFrameSize = 4 * 1024 * 1024;


// 카메라의 RTP 패킷(RFC 6184 packetization-mode 1)을 H.264 프레임으로 되돌리는 재조립 상태.
//
// 소켓을 모르며, 호출자가 받은 패킷을 하나씩 넣으면 완성한 프레임과 손실을 생성 시 받은 핸들러로 바로 알린다.
// 단일 NAL(1~23), STAP-A(24), FU-A(28)를 지원하고 다른 유형은 버린다. 같은 타임스탬프의 NAL이 한 프레임이며,
// marker가 켜진 패킷이나 타임스탬프가 바뀐 패킷에서 앞 프레임을 끝낸다. 재정렬은 하지 않으므로 순번이 1씩 늘지
// 않으면 손실로 본다. 핸들러는 이 객체가 소유한다.
class H264RtpDepacketizer {
public:
    // 완성한 프레임을 받는 핸들러.
    //
    // frame: 재조립한 프레임. NAL 뷰는 이 객체의 버퍼를 가리키므로 핸들러 호출 동안만 유효하다.
    // rtp_timestamp: 프레임의 카메라 RTP 타임스탬프(90kHz)
    // is_keyframe: SPS, PPS, IDR 슬라이스를 모두 포함하면 true
    using FrameHandler =
        std::move_only_function<void(const media::H264Frame& frame, std::uint32_t rtp_timestamp, bool is_keyframe)>;


    // 손실(순번 빠짐, SSRC 변경, 크기 초과, 잘못된 조각 순서)을 받는 핸들러. 만들던 프레임은 이미 버렸다.
    using LossHandler = std::move_only_function<void()>;


    // 재조립 상태를 만든다.
    //
    // payload_type: SETUP한 트랙의 payload type. 다른 값의 패킷은 손실로 치지 않고 버린다.
    // frame_handler: 완성한 프레임을 받을 핸들러
    // loss_handler: 손실을 받을 핸들러
    H264RtpDepacketizer(std::uint8_t payload_type, FrameHandler frame_handler, LossHandler loss_handler);


    // RTP 패킷 하나를 처리한다.
    //
    // packet: 받은 RTP 패킷(헤더 포함). 호출 동안만 참조한다.
    //
    // 핸들러를 0번 이상 동기로 호출한다. 버전이 2가 아니거나 헤더가 잘렸거나 payload type이 다르면 아무 일도 하지
    // 않는다. 순번이 기대 값과 다르면 만들던 프레임을 버리고 손실을 알린 뒤, 버린 프레임과 타임스탬프가 같은
    // 패킷과 시작 조각이 아닌 FU-A는 다음 프레임까지 버린다.
    void HandlePacket(std::span<const std::uint8_t> packet);


    // 연결이 끊겼을 때 모든 상태를 처음으로 되돌린다. 핸들러는 호출하지 않는다.
    //
    // 다음 패킷의 순번과 SSRC를 새 기준으로 삼는다.
    void Reset();


private:
    // 만들던 프레임의 NAL 하나가 버퍼에서 차지하는 범위.
    struct NalRange {
        // frame_buffer_ 안의 시작 위치
        std::size_t offset = 0;


        // NAL 크기(헤더 포함)
        std::size_t size = 0;
    };


    // 만들던 프레임과 FU-A 진행 상태를 비운다. 순번·SSRC 기준은 그대로 둔다.
    void ClearFrame();


    // 만들던 프레임을 버리고 손실을 알린다. 이후 패킷은 다음 프레임까지 버린다.
    void DropFrameAsLoss();


    // 만들던 프레임에 슬라이스가 있으면 핸들러로 내보내고 비운다.
    //
    // FU-A가 끝나지 않은 채 프레임이 끝나면 프레임을 버리고 손실로 알린다.
    void EmitFrame();


    // 단일 NAL 패킷, STAP-A, FU-A의 페이로드를 만들던 프레임에 넣는다.
    //
    // payload: RTP 페이로드. 비어 있지 않아야 한다.
    // 반환값: 형식 오류나 크기 초과로 프레임을 버려야 하면 false
    bool AppendPayload(std::span<const std::uint8_t> payload);


    // NAL 하나를 프레임 버퍼에 새로 넣는다.
    //
    // nal_unit: 넣을 NAL(헤더 포함)
    // 반환값: kMaxDepacketizedFrameSize를 넘게 되면 넣지 않고 false
    bool AppendNalUnit(std::span<const std::uint8_t> nal_unit);


    // 받아들일 payload type
    std::uint8_t payload_type_;


    // 완성한 프레임을 받는 핸들러
    FrameHandler frame_handler_;


    // 손실을 받는 핸들러
    LossHandler loss_handler_;


    // 따라가는 스트림의 SSRC. 첫 패킷 전이나 Reset() 뒤에는 비어 있다.
    std::optional<std::uint32_t> ssrc_;


    // 다음에 올 순번. 첫 패킷 전이나 Reset() 뒤에는 비어 있다.
    std::optional<std::uint16_t> expected_sequence_number_;


    // 만들던 프레임의 타임스탬프. frame_nal_ranges_가 비어 있지 않거나 FU-A 진행 중일 때만 의미가 있다.
    std::uint32_t frame_timestamp_ = 0;


    // 만들던 프레임에 패킷을 하나 이상 넣었는지 여부
    bool has_frame_data_ = false;


    // 만들던 프레임의 NAL을 이어 쓴 버퍼. frame_nal_ranges_가 범위를 가리킨다.
    //
    // 재할당하면 뷰가 무효화되므로, H264Frame의 뷰는 프레임을 다 모은 EmitFrame에서만 만든다.
    std::vector<std::uint8_t> frame_buffer_;


    // frame_buffer_ 안의 NAL 범위. 디코딩 순서대로다.
    std::vector<NalRange> frame_nal_ranges_;


    // FU-A 조각을 이어 붙이는 중인지 여부. true이면 frame_nal_ranges_의 마지막 항목이 그 NAL이다.
    bool is_fragment_in_progress_ = false;


    // 손실 뒤 다음 프레임까지 버리는 중이면 버리는 프레임의 타임스탬프. 버리지 않으면 비어 있다.
    std::optional<std::uint32_t> discarding_timestamp_;
};


}  // namespace vms::rtsp
