#include "srtp_test_receiver.h"

#include <algorithm>
#include <array>
#include <string_view>

#include "crypto/hmac_sha1.h"


namespace vms::test {


std::expected<SrtpTestReceiver, std::string> SrtpTestReceiver::Create(
    std::span<const std::uint8_t, crypto::kSrtpMasterKeySize> master_key,
    std::span<const std::uint8_t, crypto::kSrtpMasterSaltSize> master_salt) {
    const auto session_keys = crypto::DeriveSrtpSessionKeys(master_key, master_salt);
    if (!session_keys) {
        return std::unexpected(session_keys.error());
    }
    return SrtpTestReceiver(*session_keys);
}


std::expected<std::vector<std::uint8_t>, std::string> SrtpTestReceiver::Unprotect(
    std::span<const std::uint8_t> srtp_packet) {
    if (srtp_packet.size() < 4) {
        return std::unexpected("패킷이 너무 짧음");
    }
    const auto sequence_number = static_cast<std::uint16_t>((srtp_packet[2] << 8) | srtp_packet[3]);
    const std::uint32_t rollover_counter =
        last_sequence_number_ && sequence_number < *last_sequence_number_ ? rollover_counter_ + 1
                                                                           : rollover_counter_;
    auto rtp_packet = UnprotectWithRolloverCounter(srtp_packet, rollover_counter);
    if (rtp_packet) {
        rollover_counter_ = rollover_counter;
        last_sequence_number_ = sequence_number;
    }
    return rtp_packet;
}


std::expected<std::vector<std::uint8_t>, std::string> SrtpTestReceiver::UnprotectWithRolloverCounter(
    std::span<const std::uint8_t> srtp_packet, std::uint32_t rollover_counter) const {
    if (srtp_packet.size() < 12 + crypto::kSrtpAuthenticationTagSize) {
        return std::unexpected("패킷이 RTP 헤더와 태그보다 짧음");
    }
    const std::span<const std::uint8_t> authenticated_portion =
        srtp_packet.first(srtp_packet.size() - crypto::kSrtpAuthenticationTagSize);
    const std::span<const std::uint8_t> received_tag =
        srtp_packet.last(crypto::kSrtpAuthenticationTagSize);

    // 태그를 먼저 확인한다. 입력은 암호문 패킷 다음에 ROC 4바이트다.
    const std::array<std::uint8_t, 4> rollover_counter_bytes = {
        static_cast<std::uint8_t>(rollover_counter >> 24),
        static_cast<std::uint8_t>(rollover_counter >> 16),
        static_cast<std::uint8_t>(rollover_counter >> 8), static_cast<std::uint8_t>(rollover_counter)};
    const std::array<std::span<const std::uint8_t>, 2> segments = {authenticated_portion,
                                                                   rollover_counter_bytes};
    const auto digest = crypto::ComputeHmacSha1(
        std::string_view(reinterpret_cast<const char*>(session_keys_.authentication_key.data()),
                         session_keys_.authentication_key.size()),
        segments);
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!std::ranges::equal(received_tag, std::span(*digest).first(crypto::kSrtpAuthenticationTagSize))) {
        return std::unexpected("인증 태그 불일치");
    }

    std::vector<std::uint8_t> rtp_packet(authenticated_portion.begin(), authenticated_portion.end());
    std::size_t header_size = 12 + 4 * static_cast<std::size_t>(rtp_packet[0] & 0x0F);
    if ((rtp_packet[0] & 0x10) != 0) {
        if (rtp_packet.size() < header_size + 4) {
            return std::unexpected("확장 헤더가 잘림");
        }
        header_size += 4 + 4 * static_cast<std::size_t>((rtp_packet[header_size + 2] << 8) |
                                                        rtp_packet[header_size + 3]);
    }
    if (rtp_packet.size() < header_size) {
        return std::unexpected("헤더가 패킷보다 김");
    }

    // 48비트 인덱스와 SSRC를 16바이트 블록의 제자리에 놓은 뒤 솔트(왼쪽으로 16비트 민 값)와 XOR한다.
    const std::uint64_t packet_index =
        (static_cast<std::uint64_t>(rollover_counter) << 16) | ((rtp_packet[2] << 8) | rtp_packet[3]);
    std::array<std::uint8_t, crypto::kAesCmIvSize> iv{};
    std::copy(rtp_packet.begin() + 8, rtp_packet.begin() + 12, iv.begin() + 4);
    for (int shift = 40, position = 8; shift >= 0; shift -= 8, ++position) {
        iv[position] = static_cast<std::uint8_t>(packet_index >> shift);
    }
    for (std::size_t position = 0; position < session_keys_.salt.size(); ++position) {
        iv[position] ^= session_keys_.salt[position];
    }

    if (auto decrypted = crypto::XorAesCmKeystream(session_keys_.encryption_key, iv,
                                                   std::span(rtp_packet).subspan(header_size));
        !decrypted) {
        return std::unexpected(decrypted.error());
    }
    return rtp_packet;
}


}  // namespace vms::test
