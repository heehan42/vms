#pragma once

#include <netinet/in.h>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>


namespace vms::rtsp {


// RTSP를 생략한 포트로 쓸 기본 포트(RFC 2326)
inline constexpr std::uint16_t kDefaultRtspPort = 554;


// rtsp://<호스트>[:<포트>][/<경로>] 형식을 나눈 카메라 URL.
struct RtspUrl {
    // 요청 줄에 쓸 URL. 스킴을 소문자 "rtsp"로 바꾼 것 말고는 원문과 같다.
    std::string request_url;


    // 호스트 이름 또는 IPv4 점 표기 주소
    std::string host;


    // TCP 포트. 생략했으면 554
    std::uint16_t port = kDefaultRtspPort;


    // '/'로 시작하는 경로. 생략했으면 빈 문자열
    std::string path;
};


// 카메라 URL 문자열을 해석한다.
//
// text: --camera 값의 URL 부분
// 반환값: 해석 결과. 다음 경우 원인 메시지
//   - 스킴이 대소문자 구분 없이 rtsp가 아님
//   - 사용자 정보('@')가 있음. 인증을 지원하지 않는다.
//   - 호스트가 비었음
//   - 포트가 1~65535 정수가 아님
//
// 호스트 이름은 해석하지 않으며, IPv4 주소로 바꾸는 것은 ResolveIpv4Address가 한다.
std::expected<RtspUrl, std::string> ParseRtspUrl(std::string_view text);


// 호스트 이름이나 점 표기 주소를 IPv4 주소 하나로 해석한다.
//
// host: 해석할 호스트
// 반환값: getaddrinfo(AF_INET)의 첫 결과(네트워크 바이트 순서). 해석할 수 없으면 원인 메시지
//
// getaddrinfo는 블로킹이므로 이벤트 루프가 돌기 전, 시작할 때 한 번만 호출한다.
std::expected<in_addr, std::string> ResolveIpv4Address(const std::string& host);


}  // namespace vms::rtsp
