#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "crypto/srtp.h"
#include "media/h264_file.h"


namespace vms::webrtc {


// 세션 하나의 RTP 패킷 규칙 설정.
struct SrtpVideoPacketWriterConfig {
    // RTP 헤더에 넣을 SSRC. answer의 a=ssrc 값과 같아야 한다.
    std::uint32_t ssrc = 0;


    // RTP 헤더에 넣을 H.264 payload type. answer에서 정한 값이다.
    std::uint8_t payload_type = 0;


    // 첫 패킷의 순번. 비어 있으면 무작위로 정한다. 테스트가 순번 순환을 재현하는 데 쓴다.
    std::optional<std::uint16_t> initial_sequence_number;
};


// 세션 하나의 프레임을 RTP로 패킷화하고 SRTP로 보호하는 송신 공통부.
//
// 파일과 카메라 송신자가 함께 써서, 패킷화·RTP 헤더·marker·SRTP 보호 규칙이 한 곳에서 정해지게 한다. 언제 어떤
// 프레임을 어떤 타임스탬프로 보낼지는 호출자가 정한다. 순번은 프레임을 넘어 이어지며 65535 다음은 0이다.
// SrtpSender를 소유하며, 이동하면 순번과 ROC 상태가 함께 옮겨 간다.
class SrtpVideoPacketWriter {
public:
    // 송신 공통부를 만든다.
    //
    // config: SSRC, payload type, 순번 초기값
    // master_key: 서버가 보내는 패킷용 SRTP 마스터 키
    // master_salt: 서버가 보내는 패킷용 SRTP 마스터 솔트
    // 반환값: 만든 객체. 난수 생성이나 SRTP 키 유도가 실패하면 오류 메시지
    //
    // 순번 초기값을 무작위로 정해 RTP 패킷만 보고 송신 시작 시점을 추측하기 어렵게 한다(RFC 3550).
    static std::expected<SrtpVideoPacketWriter, std::string> Create(
        const SrtpVideoPacketWriterConfig& config, std::span<const std::uint8_t, crypto::kSrtpMasterKeySize> master_key,
        std::span<const std::uint8_t, crypto::kSrtpMasterSaltSize> master_salt);


    // 프레임 하나의 SRTP 패킷을 만든다.
    //
    // frame: 보낼 프레임. 호출 동안만 참조한다.
    // timestamp: 이 프레임의 모든 패킷에 넣을 RTP 타임스탬프
    // 반환값: 순서대로 보낼 SRTP 패킷. SRTP 보호가 실패하면 오류 메시지이며, 그 뒤로는 이 객체를 쓰지 않는다.
    //
    // 프레임의 마지막 패킷에만 marker 비트를 켜고, 만든 패킷 수만큼 다음 순번이 늘어난다.
    std::expected<std::vector<std::vector<std::uint8_t>>, std::string> WriteFrame(const media::H264Frame& frame,
                                                                                std::uint32_t timestamp);


private:
    // 구성한 상태로 객체를 만든다. Create()에서만 호출한다.
    SrtpVideoPacketWriter(const SrtpVideoPacketWriterConfig& config, std::unique_ptr<crypto::SrtpSender> srtp_sender,
                          std::uint16_t initial_sequence_number);


    // RTP 헤더의 SSRC
    std::uint32_t ssrc_;


    // RTP 헤더의 payload type
    std::uint8_t payload_type_;


    // 패킷 보호 상태. 순번 순환에 따른 ROC를 관리한다.
    std::unique_ptr<crypto::SrtpSender> srtp_sender_;


    // 다음 패킷의 순번. 패킷마다 1씩 늘고 65535 다음은 0이다.
    std::uint16_t next_sequence_number_;
};


// 송신 상태의 첫 RTP 타임스탬프로 쓸 난수를 만든다.
//
// 반환값: 32비트 난수. 난수 생성이 실패하면 오류 메시지
//
// 순번과 같은 이유로 타임스탬프 초기값도 무작위로 정한다(RFC 3550).
std::expected<std::uint32_t, std::string> GenerateInitialRtpTimestamp();


}  // namespace vms::webrtc
