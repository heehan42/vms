#pragma once

#include <netinet/in.h>

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>


namespace vms::core {


// 이 머신의 인터페이스 중 켜져 있고 루프백이 아닌 것의 IPv4 주소를 조회한다.
//
// 반환값: 조회한 주소 목록. 인터페이스 순서를 따르며 같은 주소는 한 번만 담는다.
//   getifaddrs가 실패하면 시스템 오류 코드
//
// 시작 시점의 주소만 돌려주므로, 실행 중 주소가 바뀌면 다시 조회해야 한다.
std::expected<std::vector<in_addr>, std::error_code> ListNonLoopbackIpv4Addresses();


// IPv4 주소를 점 표기 문자열로 바꾼다.
//
// address: 네트워크 바이트 순서의 IPv4 주소
// 반환값: "192.168.0.10" 형식의 문자열
std::string FormatIpv4Address(in_addr address);


// 점 표기 문자열을 IPv4 주소로 바꾼다.
//
// text: "192.168.0.10" 형식의 문자열
// 반환값: 네트워크 바이트 순서의 주소. 형식이 잘못되었으면 빈 optional
std::optional<in_addr> ParseIpv4Address(std::string_view text);


// IPv4 주소와 포트를 "주소:포트" 문자열로 바꾼다.
//
// endpoint: 주소와 포트(모두 네트워크 바이트 순서)
// 반환값: "192.168.0.10:50000" 형식의 문자열. 로그에 쓴다.
std::string FormatIpv4Endpoint(const sockaddr_in& endpoint);


}  // namespace vms::core
