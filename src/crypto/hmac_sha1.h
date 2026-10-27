#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>


namespace vms::crypto {


// HMAC-SHA1 결과의 크기(바이트)
inline constexpr std::size_t kHmacSha1Size = 20;


// HMAC-SHA1 결과값
using HmacSha1Digest = std::array<std::uint8_t, kHmacSha1Size>;


// 여러 조각을 순서대로 이어 붙인 데이터로 HMAC-SHA1을 계산한다.
//
// key: 비밀 키. STUN에서는 ICE 비밀번호 문자열 그대로다.
// segments: 서명할 데이터 조각. 이어 붙이지 않고 순서대로 넣으므로 복사가 필요 없다.
// 반환값: 20바이트 결과. OpenSSL이 실패하면 오류 메시지
//
// STUN 검증처럼 헤더 일부만 고친 메시지를 서명할 때, 고친 헤더와 원본 나머지를 따로 넘기는 데 쓴다.
std::expected<HmacSha1Digest, std::string> ComputeHmacSha1(
    std::string_view key, std::span<const std::span<const std::uint8_t>> segments);


// 키와 데이터 하나로 HMAC-SHA1을 계산한다.
//
// key: 비밀 키
// data: 서명할 데이터
// 반환값: 20바이트 결과. OpenSSL이 실패하면 오류 메시지
std::expected<HmacSha1Digest, std::string> ComputeHmacSha1(std::string_view key,
                                                           std::span<const std::uint8_t> data);


}  // namespace vms::crypto
