#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>


namespace vms::crypto {


// 암호학적으로 안전한 난수 바이트로 버퍼를 채운다.
//
// buffer: 채울 버퍼
// 반환값: 성공하면 빈 값, OpenSSL 난수 생성기가 실패하면 오류 메시지
std::expected<void, std::string> FillRandomBytes(std::span<std::uint8_t> buffer);


// ICE 자격 증명(ufrag, pwd)에 쓸 난수 문자열을 만든다.
//
// length: 만들 문자 수
// 반환값: ice-char(영문 대소문자, 숫자, '+', '/') 64개 중에서 고른 문자열.
//   난수 생성기가 실패하면 오류 메시지
std::expected<std::string, std::string> GenerateIceCharString(std::size_t length);


}  // namespace vms::crypto
