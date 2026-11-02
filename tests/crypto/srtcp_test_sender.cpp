#include "srtcp_test_sender.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

#include "crypto/hmac_sha1.h"


namespace vms::test {


std::expected<SrtcpTestSender, std::string> SrtcpTestSender::Create(
    std::span<const std::uint8_t, crypto::kSrtpMasterKeySize> master_key,
    std::span<const std::uint8_t, crypto::kSrtpMasterSaltSize> master_salt) {
    const auto session_keys = crypto::DeriveSrtcpSessionKeys(master_key, master_salt);
    if (!session_keys) {
        return std::unexpected(session_keys.error());
    }
    return SrtcpTestSender(*session_keys);
}


std::expected<std::vector<std::uint8_t>, std::string> SrtcpTestSender::Protect(
    std::span<const std::uint8_t> rtcp_compound, bool encrypt) {
    auto srtcp_packet = std::as_const(*this).Protect(rtcp_compound, encrypt, next_index_);
    if (srtcp_packet) {
        ++next_index_;
    }
    return srtcp_packet;
}


std::expected<std::vector<std::uint8_t>, std::string> SrtcpTestSender::Protect(
    std::span<const std::uint8_t> rtcp_compound, bool encrypt, std::uint32_t srtcp_index) const {
    if (rtcp_compound.size() < 8) {
        return std::unexpected("RTCP 패킷이 8바이트보다 짧음");
    }
    std::vector<std::uint8_t> srtcp_packet(rtcp_compound.begin(), rtcp_compound.end());

    if (encrypt) {
        // 31비트 인덱스를 48비트 자리(바이트 8~13)에, 헤더의 SSRC를 바이트 4~7에 놓은 블록을 솔트와 XOR한다.
        std::array<std::uint8_t, crypto::kAesCmIvSize> iv{};
        std::copy(srtcp_packet.begin() + 4, srtcp_packet.begin() + 8, iv.begin() + 4);
        iv[10] = static_cast<std::uint8_t>(srtcp_index >> 24);
        iv[11] = static_cast<std::uint8_t>(srtcp_index >> 16);
        iv[12] = static_cast<std::uint8_t>(srtcp_index >> 8);
        iv[13] = static_cast<std::uint8_t>(srtcp_index);
        for (std::size_t position = 0; position < session_keys_.salt.size(); ++position) {
            iv[position] ^= session_keys_.salt[position];
        }
        if (auto encrypted = crypto::XorAesCmKeystream(session_keys_.encryption_key, iv,
                                                       std::span(srtcp_packet).subspan(8));
            !encrypted) {
            return std::unexpected(encrypted.error());
        }
    }

    // 꼬리의 첫 비트가 E, 나머지 31비트가 인덱스다. 태그는 인덱스 단어까지 포함한 전체의 HMAC이다.
    const std::uint32_t index_word = (encrypt ? 0x80000000u : 0u) | (srtcp_index & 0x7FFFFFFFu);
    srtcp_packet.push_back(static_cast<std::uint8_t>(index_word >> 24));
    srtcp_packet.push_back(static_cast<std::uint8_t>(index_word >> 16));
    srtcp_packet.push_back(static_cast<std::uint8_t>(index_word >> 8));
    srtcp_packet.push_back(static_cast<std::uint8_t>(index_word));
    const auto digest = crypto::ComputeHmacSha1(
        std::string_view(reinterpret_cast<const char*>(session_keys_.authentication_key.data()),
                         session_keys_.authentication_key.size()),
        std::span<const std::uint8_t>(srtcp_packet));
    if (!digest) {
        return std::unexpected(digest.error());
    }
    srtcp_packet.insert(srtcp_packet.end(), digest->begin(), digest->begin() + crypto::kSrtpAuthenticationTagSize);
    return srtcp_packet;
}


}  // namespace vms::test
