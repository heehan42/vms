#pragma once

#include <netinet/in.h>

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>


namespace vms::webrtc {


// UDP 소켓 하나의 bind address와 그 소켓의 candidate로 알릴 announced address 목록.
//
// 서버는 bind address마다 UDP 소켓을 하나 열고, announced address마다 host candidate를 하나 만든다.
// announced address는 이 머신의 인터페이스에 없어도 된다. 예를 들어 공유기의 public address를 쓸 수 있다.
struct IceListenAddress {
    // UDP 소켓을 바인드할 IPv4 주소(네트워크 바이트 순서). 이 머신의 인터페이스에 있는 주소여야 한다.
    in_addr bind_address{};


    // 이 소켓의 candidate로 알릴 IPv4 주소 목록(네트워크 바이트 순서). 앞의 주소일수록 priority가 높다.
    //
    // 비어 있으면 안 된다. bind address를 그대로 알릴 때도 bind address 하나를 담는다.
    std::vector<in_addr> announced_addresses;
};


// `--ice-address` 값 하나를 IceListenAddress로 바꾼다.
//
// text: `<bind address>[=<announced address>[,<announced address>]...]` 형식의 값
// 반환값: 해석한 값. `=`가 없으면 announced_addresses는 bind address 하나다.
//   주소가 IPv4 형식이 아니거나 `=` 뒤에 빈 주소가 있으면 원인을 담은 오류 메시지
std::expected<IceListenAddress, std::string> ParseIceListenAddress(std::string_view text);


// 여러 `--ice-address` 값에 걸쳐 bind address와 announced address의 중복을 검사한다.
//
// addresses: 명령행에 쓴 순서의 값 목록
// 반환값: 중복이 없으면 빈 성공 값. 같은 bind address가 두 번 있거나 같은 announced address가 두 번
//   나오면 처음 발견한 중복 주소를 담은 오류 메시지
std::expected<void, std::string> CheckIceListenAddresses(std::span<const IceListenAddress> addresses);


}  // namespace vms::webrtc
