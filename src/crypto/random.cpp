#include "crypto/random.h"

#include <openssl/rand.h>

#include <string_view>
#include <vector>

#include "crypto/openssl_error.h"


namespace vms::crypto {


namespace {


// ICE 자격 증명에 쓸 수 있는 문자(RFC 8839의 ice-char). 정확히 64개다.
constexpr std::string_view kIceCharacters =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";


static_assert(kIceCharacters.size() == 64);


}  // namespace


std::expected<void, std::string> FillRandomBytes(std::span<std::uint8_t> buffer) {
    if (::RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1) {
        return std::unexpected(TakeOpenSslError("난수 생성"));
    }
    return {};
}


std::expected<std::string, std::string> GenerateIceCharString(std::size_t length) {
    std::vector<std::uint8_t> random_bytes(length);
    if (auto filled = FillRandomBytes(random_bytes); !filled) {
        return std::unexpected(filled.error());
    }

    // 문자 집합이 64개이므로 바이트의 하위 6비트로 고르면 모든 문자가 같은 확률로 나온다.
    std::string text;
    text.reserve(length);
    for (const std::uint8_t random_byte : random_bytes) {
        text += kIceCharacters[random_byte & 0x3F];
    }
    return text;
}


}  // namespace vms::crypto
