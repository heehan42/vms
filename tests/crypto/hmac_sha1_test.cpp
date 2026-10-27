#include "crypto/hmac_sha1.h"

#include <array>
#include <string>
#include <string_view>

#include "test_support.h"

using vms::crypto::ComputeHmacSha1;
using vms::crypto::HmacSha1Digest;
using vms::test::Expect;


namespace {


// 문자열을 HMAC 입력 바이트로 본다.
//
// text: 입력 문자열. 반환값이 쓰이는 동안 살아 있어야 한다.
// 반환값: text를 가리키는 바이트 범위
std::span<const std::uint8_t> AsBytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}


// 결과값을 소문자 16진수 문자열로 바꾼다.
//
// digest: HMAC-SHA1 결과
// 반환값: 40자 16진수 문자열
std::string ToHex(const HmacSha1Digest& digest) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string hex;
    for (const std::uint8_t value : digest) {
        hex += kDigits[value >> 4];
        hex += kDigits[value & 0x0F];
    }
    return hex;
}


// RFC 2202 테스트 케이스 1: 0x0b 20바이트 키와 "Hi There"
void TestRfc2202Case1() {
    const std::string key(20, '\x0b');
    const auto digest = ComputeHmacSha1(key, AsBytes("Hi There"));
    Expect(digest && ToHex(*digest) == "b617318655057264e28bc0b6fb378c8ef146be00",
           "RFC 2202 테스트 케이스 1");
}


// RFC 2202 테스트 케이스 2: 키 "Jefe"와 "what do ya want for nothing?"
void TestRfc2202Case2() {
    const auto digest = ComputeHmacSha1("Jefe", AsBytes("what do ya want for nothing?"));
    Expect(digest && ToHex(*digest) == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79",
           "RFC 2202 테스트 케이스 2");
}


// RFC 2202 테스트 케이스 6: 블록 크기(64바이트)보다 긴 80바이트 키
void TestRfc2202Case6() {
    const std::string key(80, '\xaa');
    const auto digest =
        ComputeHmacSha1(key, AsBytes("Test Using Larger Than Block-Size Key - Hash Key First"));
    Expect(digest && ToHex(*digest) == "aa4ae5e15272d00e95705637ce8a3b55ed402112",
           "RFC 2202 테스트 케이스 6");
}


// 여러 조각으로 나눠 계산한 결과가 이어 붙인 데이터의 결과와 같은지 검사한다.
void TestSegmentsMatchConcatenation() {
    const std::string_view first = "what do ya ";
    const std::string_view second = "want for nothing?";
    const std::array<std::span<const std::uint8_t>, 2> segments = {AsBytes(first), AsBytes(second)};
    const auto digest = ComputeHmacSha1("Jefe", segments);
    Expect(digest && ToHex(*digest) == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79",
           "조각으로 나눈 계산이 RFC 2202 테스트 케이스 2와 같아야 한다");
}


}  // namespace


int main() {
    TestRfc2202Case1();
    TestRfc2202Case2();
    TestRfc2202Case6();
    TestSegmentsMatchConcatenation();
    return vms::test::ExitCode();
}
