#pragma once

#include <cstdint>
#include <string>


namespace vms::webrtc {


// SDP answer에 a=candidate 줄로 알릴 서버의 host candidate 하나.
struct IceCandidate {
    // 같은 종류·같은 기반 주소의 후보를 묶는 식별자. 서버 후보는 주소마다 다른 값이다.
    std::uint32_t foundation = 1;


    // RFC 8445 공식으로 계산한 우선순위. 클수록 브라우저가 먼저 확인한다.
    std::uint32_t priority = 0;


    // 점 표기 IPv4 주소. 예: "172.27.209.81"
    std::string address;


    // UDP 포트(호스트 바이트 순서)
    std::uint16_t port = 0;
};


}  // namespace vms::webrtc
