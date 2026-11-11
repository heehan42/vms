#pragma once

#include <cstdint>
#include <string>


namespace vms::webrtc {


// SDP answer에 a=candidate 줄로 알릴 서버의 host candidate 하나.
struct IceCandidate {
    // 같은 종류·같은 기반 주소의 후보를 묶는 식별자. 서버 후보는 announced address마다 다른 값이다.
    //
    // 같은 소켓의 후보도 서로 다른 값을 쓴다. 값이 같으면 브라우저가 앞의 후보 확인이 끝날 때까지 뒤의 후보 확인을
    // 미룰 수 있다.
    std::uint32_t foundation = 1;


    // RFC 8445 공식으로 계산한 우선순위. 클수록 브라우저가 먼저 확인한다.
    std::uint32_t priority = 0;


    // 브라우저에 알릴 점 표기 IPv4 주소(announced address). 예: "172.27.209.81"
    //
    // 소켓의 bind address와 다를 수 있다. 예를 들어 공유기의 public address를 쓸 수 있다.
    std::string address;


    // UDP 포트(호스트 바이트 순서)
    std::uint16_t port = 0;
};


}  // namespace vms::webrtc
