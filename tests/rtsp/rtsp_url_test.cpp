#include "rtsp/rtsp_url.h"

#include <string_view>

#include "core/network_interfaces.h"
#include "test_support.h"

using vms::rtsp::ParseRtspUrl;
using vms::rtsp::ResolveIpv4Address;
using vms::test::Expect;


namespace {


// 포트·경로가 있는 URL, 포트를 생략한 URL, 대문자 스킴, 경로 없는 URL을 해석하는지 검사한다.
void TestParsesValidUrls() {
    const auto full = ParseRtspUrl("rtsp://127.0.0.1:8554/cam");
    Expect(full && full->host == "127.0.0.1" && full->port == 8554 && full->path == "/cam" &&
               full->request_url == "rtsp://127.0.0.1:8554/cam",
           "호스트, 포트, 경로");

    const auto default_port = ParseRtspUrl("rtsp://camera.local/live/main");
    Expect(default_port && default_port->host == "camera.local" && default_port->port == 554 &&
               default_port->path == "/live/main",
           "포트를 생략하면 554");

    const auto upper_scheme = ParseRtspUrl("RTSP://10.0.0.1:9554/cam");
    Expect(upper_scheme && upper_scheme->port == 9554 && upper_scheme->request_url == "rtsp://10.0.0.1:9554/cam",
           "대문자 스킴을 받아들이고 요청 URL은 소문자 스킴");

    const auto no_path = ParseRtspUrl("rtsp://127.0.0.1:8554");
    Expect(no_path && no_path->path.empty() && no_path->port == 8554, "경로 없음");
}


// 사용자 정보, 잘못된 포트, 다른 스킴, 빈 호스트를 거부하는지 검사한다.
void TestRejectsInvalidUrls() {
    const auto user_info = ParseRtspUrl("rtsp://user:pass@127.0.0.1:8554/cam");
    Expect(!user_info && user_info.error().contains("인증"), "사용자 정보는 인증 미지원 원인으로 거부");

    for (const std::string_view text :
         {"rtsp://127.0.0.1:0/cam", "rtsp://127.0.0.1:65536/cam", "rtsp://127.0.0.1:abc/cam", "rtsp://127.0.0.1:/cam",
          "http://127.0.0.1:8554/cam", "rtsp:/127.0.0.1/cam", "rtsp://:8554/cam", "rtsp://"}) {
        Expect(!ParseRtspUrl(text), std::string("거부해야 함: ") + std::string(text));
    }
}


// 점 표기 주소와 localhost를 IPv4로 해석하고, 해석할 수 없는 이름은 실패하는지 검사한다.
void TestResolvesAddresses() {
    const auto literal = ResolveIpv4Address("192.168.0.10");
    Expect(literal && vms::core::FormatIpv4Address(*literal) == "192.168.0.10", "점 표기 주소");

    const auto localhost = ResolveIpv4Address("localhost");
    Expect(localhost && vms::core::FormatIpv4Address(*localhost) == "127.0.0.1", "localhost는 127.0.0.1");

    Expect(!ResolveIpv4Address("no-such-host.invalid"), ".invalid 이름은 해석 실패");
}


}  // namespace


int main() {
    TestParsesValidUrls();
    TestRejectsInvalidUrls();
    TestResolvesAddresses();
    return vms::test::ExitCode();
}
