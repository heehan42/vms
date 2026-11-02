#include "crypto/srtp.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "srtcp_test_sender.h"
#include "srtp_test_receiver.h"
#include "test_support.h"

using vms::crypto::DeriveSrtcpSessionKeys;
using vms::crypto::DeriveSrtpSessionKeys;
using vms::crypto::SrtcpReceiver;
using vms::crypto::SrtcpUnprotectError;
using vms::crypto::SrtcpUnprotectFailure;
using vms::crypto::SrtpSender;
using vms::crypto::XorAesCmKeystream;
using vms::test::Expect;
using vms::test::SrtcpTestSender;
using vms::test::SrtpTestReceiver;


namespace {


// 테스트 송수신에 쓰는 마스터 키
constexpr std::array<std::uint8_t, 16> kMasterKey = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                                     0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};


// 테스트 송수신에 쓰는 마스터 솔트
constexpr std::array<std::uint8_t, 14> kMasterSalt = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6,
                                                      0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD};


// 바이트를 대문자 16진수 문자열로 바꾼다.
//
// bytes: 바꿀 바이트
// 반환값: 바이트마다 두 글자인 문자열
std::string ToHex(std::span<const std::uint8_t> bytes) {
    constexpr std::string_view kDigits = "0123456789ABCDEF";
    std::string hex;
    for (const std::uint8_t value : bytes) {
        hex += kDigits[value >> 4];
        hex += kDigits[value & 0x0F];
    }
    return hex;
}


// 16진수 문자열을 고정 크기 바이트 배열로 바꾼다.
//
// hex: 대문자 16진수 문자열. 길이가 Size의 두 배여야 한다.
// 반환값: 바꾼 바이트
template <std::size_t Size>
std::array<std::uint8_t, Size> FromHex(std::string_view hex) {
    const auto digit = [](char character) {
        return static_cast<std::uint8_t>(character <= '9' ? character - '0' : character - 'A' + 10);
    };
    std::array<std::uint8_t, Size> bytes{};
    for (std::size_t index = 0; index < Size; ++index) {
        bytes[index] = static_cast<std::uint8_t>((digit(hex[2 * index]) << 4) | digit(hex[2 * index + 1]));
    }
    return bytes;
}


// 페이로드가 정해진 패턴인 RTP 패킷을 만든다.
//
// sequence_number: 순번
// csrc_count: 헤더에 넣을 CSRC 수
// payload_size: 페이로드 바이트 수
// 반환값: 버전 2, PT 102, 타임스탬프 0x01020304, SSRC 0xCAFEBABE인 패킷
std::vector<std::uint8_t> BuildTestRtpPacket(std::uint16_t sequence_number, std::uint8_t csrc_count = 0,
                                             std::size_t payload_size = 100) {
    std::vector<std::uint8_t> packet = {static_cast<std::uint8_t>(0x80 | csrc_count),
                                        102,
                                        static_cast<std::uint8_t>(sequence_number >> 8),
                                        static_cast<std::uint8_t>(sequence_number),
                                        0x01,
                                        0x02,
                                        0x03,
                                        0x04,
                                        0xCA,
                                        0xFE,
                                        0xBA,
                                        0xBE};
    for (std::uint8_t csrc = 0; csrc < csrc_count; ++csrc) {
        packet.insert(packet.end(), {0x11, 0x22, 0x33, csrc});
    }
    for (std::size_t index = 0; index < payload_size; ++index) {
        packet.push_back(static_cast<std::uint8_t>(index));
    }
    return packet;
}


// RFC 3711 부록 B.3: 키 유도 테스트 벡터
void TestRfc3711KeyDerivation() {
    const auto master_key = FromHex<16>("E1F97A0D3E018BE0D64FA32C06DE4139");
    const auto master_salt = FromHex<14>("0EC675AD498AFEEBB6960B3AABE6");
    const auto keys = DeriveSrtpSessionKeys(master_key, master_salt);
    if (!keys) {
        Expect(false, "B.3 세션 키 유도가 성공해야 한다");
        return;
    }
    Expect(ToHex(keys->encryption_key) == "C61E7A93744F39EE10734AFE3FF7A087", "B.3 암호화 키");
    Expect(ToHex(keys->authentication_key) == "CEBE321F6FF7716B6FD4AB49AF256A156D38BAA4",
           "B.3 인증 키(앞 20바이트)");
    Expect(ToHex(keys->salt) == "30CBBC08863D8C85D49DB34A9AE1", "B.3 세션 솔트");
}


// 레이블로 직접 IV를 만들어 마스터 키의 AES-CM 키스트림을 구한다(키 유도 주기 0).
//
// label: 키 유도 레이블. 마스터 솔트의 바이트 7에 XOR한다.
// 반환값: 앞에서부터 Size바이트의 키스트림. 실패하면 0으로 채운 배열
template <std::size_t Size>
std::array<std::uint8_t, Size> ExpectedSessionKey(std::uint8_t label) {
    std::array<std::uint8_t, 16> iv{};
    std::ranges::copy(kMasterSalt, iv.begin());
    iv[7] ^= label;
    std::array<std::uint8_t, Size> keystream{};
    if (!XorAesCmKeystream(kMasterKey, iv, keystream)) {
        return {};
    }
    return keystream;
}


// SRTCP 세션 키는 레이블 0x03~0x05의 키스트림과 같고 SRTP 세션 키와 다르다.
void TestSrtcpKeyDerivation() {
    const auto srtp_keys = DeriveSrtpSessionKeys(kMasterKey, kMasterSalt);
    const auto srtcp_keys = DeriveSrtcpSessionKeys(kMasterKey, kMasterSalt);
    if (!srtp_keys || !srtcp_keys) {
        Expect(false, "SRTP·SRTCP 세션 키 유도가 성공해야 한다");
        return;
    }
    Expect(srtcp_keys->encryption_key == ExpectedSessionKey<16>(0x03), "SRTCP 암호화 키는 레이블 0x03");
    Expect(srtcp_keys->authentication_key == ExpectedSessionKey<20>(0x04), "SRTCP 인증 키는 레이블 0x04");
    Expect(srtcp_keys->salt == ExpectedSessionKey<14>(0x05), "SRTCP 세션 솔트는 레이블 0x05");
    Expect(srtcp_keys->encryption_key != srtp_keys->encryption_key, "SRTCP 암호화 키는 SRTP와 다름");
    Expect(srtcp_keys->authentication_key != srtp_keys->authentication_key, "SRTCP 인증 키는 SRTP와 다름");
    Expect(srtcp_keys->salt != srtp_keys->salt, "SRTCP 세션 솔트는 SRTP와 다름");
}


// RFC 3711 부록 B.2: AES-CM 키스트림 테스트 벡터
void TestRfc3711Keystream() {
    const auto session_key = FromHex<16>("2B7E151628AED2A6ABF7158809CF4F3C");
    const auto iv = FromHex<16>("F0F1F2F3F4F5F6F7F8F9FAFBFCFD0000");
    std::array<std::uint8_t, 48> keystream{};
    const auto result = XorAesCmKeystream(session_key, iv, keystream);
    Expect(result.has_value(), "B.2 키스트림 생성이 성공해야 한다");
    Expect(ToHex(keystream) ==
               "E03EAD0935C95E80E166B16DD92B4EB4"
               "D23513162B02D0F72A43A2FE4A5F97AB"
               "41E95B3BB0A2E8DD477901E4FCA894C0",
           "B.2 키스트림 앞 세 블록");
}


// 보호한 패킷은 헤더가 같고, 페이로드 길이가 같고 내용이 다르며, 끝에 10바이트가 붙는다.
void TestProtectedPacketShape() {
    auto sender = SrtpSender::Create(kMasterKey, kMasterSalt);
    if (!sender) {
        Expect(false, "SrtpSender 생성이 성공해야 한다");
        return;
    }
    const std::vector<std::uint8_t> rtp_packet = BuildTestRtpPacket(1000);
    const auto srtp_packet = (*sender)->Protect(rtp_packet);
    if (!srtp_packet) {
        Expect(false, "보호가 성공해야 한다");
        return;
    }
    Expect(srtp_packet->size() == rtp_packet.size() + 10, "끝에 10바이트 인증 태그가 붙어야 한다");
    Expect(std::equal(rtp_packet.begin(), rtp_packet.begin() + 12, srtp_packet->begin()),
           "RTP 헤더는 평문 그대로여야 한다");
    Expect(!std::equal(rtp_packet.begin() + 12, rtp_packet.end(), srtp_packet->begin() + 12),
           "페이로드는 암호화되어 원본과 달라야 한다");
}


// 테스트 수신 측이 태그를 검증하고 복호화한 결과가 원본과 같은지, CSRC가 있는 헤더도 평문으로 두는지 검사한다.
void TestReceiverRoundTrip() {
    auto sender = SrtpSender::Create(kMasterKey, kMasterSalt);
    auto receiver = SrtpTestReceiver::Create(kMasterKey, kMasterSalt);
    if (!sender || !receiver) {
        Expect(false, "송신·수신 측 생성이 성공해야 한다");
        return;
    }

    const std::vector<std::uint8_t> plain_packet = BuildTestRtpPacket(1);
    const auto protected_plain = (*sender)->Protect(plain_packet);
    const auto recovered_plain =
        protected_plain ? receiver->Unprotect(*protected_plain) : std::expected<std::vector<std::uint8_t>, std::string>();
    Expect(recovered_plain && *recovered_plain == plain_packet, "복호화 결과가 원본 RTP 패킷과 같아야 한다");

    // CSRC 두 개(8바이트)가 있으면 헤더는 20바이트다. 헤더 길이를 틀리면 CSRC가 암호화되거나 복원이 틀린다.
    const std::vector<std::uint8_t> csrc_packet = BuildTestRtpPacket(2, 2);
    const auto protected_csrc = (*sender)->Protect(csrc_packet);
    Expect(protected_csrc && std::equal(csrc_packet.begin(), csrc_packet.begin() + 20, protected_csrc->begin()),
           "CSRC를 포함한 20바이트 헤더는 평문이어야 한다");
    Expect(protected_csrc && !std::equal(csrc_packet.begin() + 20, csrc_packet.end(), protected_csrc->begin() + 20),
           "CSRC 뒤의 페이로드는 암호화되어야 한다");
    const auto recovered_csrc = protected_csrc ? receiver->Unprotect(*protected_csrc)
                                               : std::expected<std::vector<std::uint8_t>, std::string>();
    Expect(recovered_csrc && *recovered_csrc == csrc_packet, "CSRC가 있는 패킷도 원본으로 복원되어야 한다");
}


// 순번 65535 다음 0인 패킷은 ROC 1로 보호해야 한다.
void TestRolloverCounter() {
    auto sender = SrtpSender::Create(kMasterKey, kMasterSalt);
    auto receiver = SrtpTestReceiver::Create(kMasterKey, kMasterSalt);
    if (!sender || !receiver) {
        Expect(false, "송신·수신 측 생성이 성공해야 한다");
        return;
    }

    const auto before_wrap = (*sender)->Protect(BuildTestRtpPacket(65535));
    const std::vector<std::uint8_t> wrapped_packet = BuildTestRtpPacket(0);
    const auto after_wrap = (*sender)->Protect(wrapped_packet);
    if (!before_wrap || !after_wrap) {
        Expect(false, "순환 전후 패킷 보호가 성공해야 한다");
        return;
    }
    Expect(receiver->UnprotectWithRolloverCounter(*before_wrap, 0).has_value(), "순번 65535는 ROC 0이어야 한다");
    const auto recovered = receiver->UnprotectWithRolloverCounter(*after_wrap, 1);
    Expect(recovered && *recovered == wrapped_packet, "순번 0으로 넘어간 패킷은 ROC 1로 복호화되어야 한다");
    Expect(!receiver->UnprotectWithRolloverCounter(*after_wrap, 0).has_value(),
           "ROC 0을 가정하면 태그 검증이 실패해야 한다");

    // 순번 순환을 따라가는 수신 측도 같은 결과를 내야 한다.
    Expect(receiver->Unprotect(*before_wrap).has_value() && receiver->Unprotect(*after_wrap).has_value(),
           "순번 순환을 세는 수신 측이 두 패킷을 모두 받아들여야 한다");
}


// 태그 한 바이트를 바꾸면 검증이 실패해야 한다.
void TestTamperedTagRejected() {
    auto sender = SrtpSender::Create(kMasterKey, kMasterSalt);
    auto receiver = SrtpTestReceiver::Create(kMasterKey, kMasterSalt);
    if (!sender || !receiver) {
        Expect(false, "송신·수신 측 생성이 성공해야 한다");
        return;
    }
    auto srtp_packet = (*sender)->Protect(BuildTestRtpPacket(7));
    if (!srtp_packet) {
        Expect(false, "보호가 성공해야 한다");
        return;
    }
    srtp_packet->back() ^= 0x01;
    Expect(!receiver->Unprotect(*srtp_packet).has_value(), "태그를 바꾼 패킷은 검증에 실패해야 한다");
}


// RTP 형식이 아닌 입력은 보호하지 않는다.
void TestRejectsMalformedRtp() {
    auto sender = SrtpSender::Create(kMasterKey, kMasterSalt);
    if (!sender) {
        Expect(false, "SrtpSender 생성이 성공해야 한다");
        return;
    }
    const std::array<std::uint8_t, 8> short_packet = {0x80, 102, 0, 1, 0, 0, 0, 0};
    Expect(!(*sender)->Protect(short_packet).has_value(), "고정 헤더보다 짧은 패킷은 거부해야 한다");

    std::vector<std::uint8_t> csrc_overflow = BuildTestRtpPacket(1, 0, 4);
    csrc_overflow[0] = 0x8F;
    Expect(!(*sender)->Protect(csrc_overflow).has_value(), "CSRC 목록이 패킷보다 길면 거부해야 한다");
}


// 보고 블록 하나인 RR(32바이트) 뒤에 CNAME SDES(28바이트)가 이어진 RTCP 복합 패킷을 만든다.
//
// ssrc: RR과 SDES의 보낸 쪽 SSRC
// 반환값: 60바이트 복합 패킷
std::vector<std::uint8_t> BuildTestRtcpCompound(std::uint32_t ssrc = 1) {
    const std::array<std::uint8_t, 4> ssrc_bytes = {static_cast<std::uint8_t>(ssrc >> 24),
                                                    static_cast<std::uint8_t>(ssrc >> 16),
                                                    static_cast<std::uint8_t>(ssrc >> 8), static_cast<std::uint8_t>(ssrc)};
    std::vector<std::uint8_t> compound = {0x81, 0xC9, 0x00, 0x07};
    compound.insert(compound.end(), ssrc_bytes.begin(), ssrc_bytes.end());
    compound.insert(compound.end(), {0x12, 0x34, 0x56, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x11, 0xEB,
                                     0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    compound.insert(compound.end(), {0x81, 0xCA, 0x00, 0x06});
    compound.insert(compound.end(), ssrc_bytes.begin(), ssrc_bytes.end());
    compound.insert(compound.end(), {0x01, 0x10});
    for (char character = 'a'; character <= 'p'; ++character) {
        compound.push_back(static_cast<std::uint8_t>(character));
    }
    compound.insert(compound.end(), {0x00, 0x00});
    return compound;
}


// SRTCP 테스트 송신 측과 서버 수신 측을 같은 마스터 키로 만든다.
struct SrtcpPair {
    // 브라우저 역할 송신 측
    SrtcpTestSender sender;


    // 서버 수신 측
    std::unique_ptr<SrtcpReceiver> receiver;
};


// 같은 마스터 키·솔트로 송신·수신 측을 만든다.
//
// 반환값: 송신·수신 측. 생성에 실패하면 빈 optional
std::optional<SrtcpPair> CreateSrtcpPair() {
    auto sender = SrtcpTestSender::Create(kMasterKey, kMasterSalt);
    auto receiver = SrtcpReceiver::Create(kMasterKey, kMasterSalt);
    if (!sender || !receiver) {
        return std::nullopt;
    }
    return SrtcpPair{.sender = *std::move(sender), .receiver = *std::move(receiver)};
}


// 받아들이지 않은 결과의 원인이 기대한 값인지 확인한다.
//
// result: Unprotect 결과
// failure: 기대한 원인
// 반환값: 실패했고 원인이 같으면 true
bool FailedWith(const std::expected<std::vector<std::uint8_t>, SrtcpUnprotectError>& result,
                SrtcpUnprotectFailure failure) {
    return !result && result.error().failure == failure;
}


// E=1로 보호한 패킷은 원본으로 복원되고, E=0 패킷은 인증만으로 받아들인다.
void TestSrtcpRoundTrip() {
    auto pair = CreateSrtcpPair();
    if (!pair) {
        Expect(false, "SRTCP 송신·수신 측 생성이 성공해야 한다");
        return;
    }
    const std::vector<std::uint8_t> compound = BuildTestRtcpCompound();

    const auto encrypted = pair->sender.Protect(compound, true);
    if (!encrypted) {
        Expect(false, "SRTCP 보호가 성공해야 한다");
        return;
    }
    Expect(encrypted->size() == compound.size() + 14, "SRTCP 꼬리는 인덱스 4바이트와 태그 10바이트");
    Expect(std::equal(compound.begin(), compound.begin() + 8, encrypted->begin()), "첫 8바이트는 평문");
    Expect(!std::equal(compound.begin() + 8, compound.end(), encrypted->begin() + 8), "나머지는 암호화됨");
    const auto recovered = pair->receiver->Unprotect(*encrypted);
    Expect(recovered && *recovered == compound, "E=1 패킷은 보호 전 복합 패킷으로 복원");

    const auto authenticated_only = pair->sender.Protect(compound, false);
    Expect(authenticated_only && std::equal(compound.begin(), compound.end(), authenticated_only->begin()),
           "E=0 패킷은 RTCP 부분이 평문");
    const auto accepted = authenticated_only ? pair->receiver->Unprotect(*authenticated_only)
                                             : std::unexpected(SrtcpUnprotectError{});
    Expect(accepted && *accepted == compound, "E=0 패킷은 인증만으로 받아들임");
}


// 암호문이나 인덱스의 한 비트를 바꾸면 인증 실패, 21바이트 패킷은 길이 부족이다.
void TestSrtcpRejectsTamperedAndShort() {
    auto pair = CreateSrtcpPair();
    if (!pair) {
        Expect(false, "SRTCP 송신·수신 측 생성이 성공해야 한다");
        return;
    }
    const auto protected_packet = pair->sender.Protect(BuildTestRtcpCompound(), true, 42);
    if (!protected_packet) {
        Expect(false, "SRTCP 보호가 성공해야 한다");
        return;
    }

    auto tampered_ciphertext = *protected_packet;
    tampered_ciphertext[20] ^= 0x01;
    Expect(FailedWith(pair->receiver->Unprotect(tampered_ciphertext), SrtcpUnprotectFailure::kAuthenticationFailed),
           "암호문 1비트 변조는 인증 실패");

    // 인덱스 단어의 마지막 바이트는 태그 바로 앞이다.
    auto tampered_index = *protected_packet;
    tampered_index[tampered_index.size() - 11] ^= 0x01;
    const auto index_result = pair->receiver->Unprotect(tampered_index);
    Expect(FailedWith(index_result, SrtcpUnprotectFailure::kAuthenticationFailed), "인덱스 1비트 변조는 인증 실패");
    Expect(!index_result && index_result.error().ssrc == 1u && index_result.error().srtcp_index == 43u,
           "오류에 패킷의 SSRC와 인덱스가 담김");

    const std::vector<std::uint8_t> short_packet(protected_packet->begin(), protected_packet->begin() + 21);
    const auto short_result = pair->receiver->Unprotect(short_packet);
    Expect(FailedWith(short_result, SrtcpUnprotectFailure::kTooShort), "21바이트 패킷은 길이 부족");
    Expect(!short_result && short_result.error().ssrc == 1u && !short_result.error().srtcp_index,
           "길이 부족 오류에는 SSRC만 담김");

    Expect(pair->receiver->Unprotect(*protected_packet).has_value(), "변조 패킷 뒤에도 원본은 받아들임");
}


// 재생 방지 창: 같은 패킷, 순서 바뀐 패킷, 창보다 오래된 패킷, 인증 실패 패킷의 인덱스를 검사한다.
void TestSrtcpReplayWindow() {
    auto pair = CreateSrtcpPair();
    if (!pair) {
        Expect(false, "SRTCP 송신·수신 측 생성이 성공해야 한다");
        return;
    }
    const std::vector<std::uint8_t> compound = BuildTestRtcpCompound();
    const auto protect = [&](std::uint32_t index) { return *pair->sender.Protect(compound, true, index); };

    const auto first = protect(0);
    Expect(pair->receiver->Unprotect(first).has_value(), "처음 받은 패킷은 받아들임");
    const auto replayed = pair->receiver->Unprotect(first);
    Expect(FailedWith(replayed, SrtcpUnprotectFailure::kReplayed), "같은 패킷 두 번째는 재생");
    Expect(!replayed && replayed.error().ssrc == 1u && replayed.error().srtcp_index == 0u,
           "재생 오류에 SSRC와 인덱스가 담김");

    Expect(pair->receiver->Unprotect(protect(10)).has_value(), "인덱스 10은 받아들임");
    Expect(pair->receiver->Unprotect(protect(9)).has_value(), "인덱스 10 뒤 9도 받아들임");
    Expect(pair->receiver->Unprotect(protect(100)).has_value(), "인덱스 100은 받아들임");
    Expect(FailedWith(pair->receiver->Unprotect(protect(30)), SrtcpUnprotectFailure::kReplayed),
           "인덱스 100 뒤 30은 창보다 오래되어 재생");
    Expect(pair->receiver->Unprotect(protect(37)).has_value(), "인덱스 100 뒤 37은 창 안이라 받아들임");

    auto forged = protect(5);
    forged.back() ^= 0x01;
    auto other_pair = CreateSrtcpPair();
    if (!other_pair) {
        Expect(false, "두 번째 SRTCP 송신·수신 측 생성이 성공해야 한다");
        return;
    }
    Expect(FailedWith(other_pair->receiver->Unprotect(forged), SrtcpUnprotectFailure::kAuthenticationFailed),
           "태그가 틀린 인덱스 5는 인증 실패");
    Expect(other_pair->receiver->Unprotect(protect(5)).has_value(), "인증 실패는 창에 남지 않아 올바른 인덱스 5는 받아들임");
}


// SSRC 8개까지는 창을 만들고, 9번째 새 SSRC는 거부한다.
void TestSrtcpSsrcLimit() {
    auto pair = CreateSrtcpPair();
    if (!pair) {
        Expect(false, "SRTCP 송신·수신 측 생성이 성공해야 한다");
        return;
    }
    for (std::uint32_t ssrc = 1; ssrc <= 8; ++ssrc) {
        const auto packet = pair->sender.Protect(BuildTestRtcpCompound(ssrc), true);
        Expect(packet && pair->receiver->Unprotect(*packet).has_value(), "8번째 SSRC까지는 받아들임");
    }
    const auto ninth = pair->sender.Protect(BuildTestRtcpCompound(9), true);
    const auto result = ninth ? pair->receiver->Unprotect(*ninth) : std::unexpected(SrtcpUnprotectError{});
    Expect(FailedWith(result, SrtcpUnprotectFailure::kTooManySsrcs), "9번째 SSRC는 SSRC 수 초과");
    Expect(!result && result.error().ssrc == 9u, "SSRC 수 초과 오류에 SSRC가 담김");

    const auto known = pair->sender.Protect(BuildTestRtcpCompound(3), true);
    Expect(known && pair->receiver->Unprotect(*known).has_value(), "이미 창이 있는 SSRC는 계속 받아들임");
}


}  // namespace


int main() {
    TestRfc3711KeyDerivation();
    TestSrtcpKeyDerivation();
    TestRfc3711Keystream();
    TestProtectedPacketShape();
    TestReceiverRoundTrip();
    TestRolloverCounter();
    TestTamperedTagRejected();
    TestRejectsMalformedRtp();
    TestSrtcpRoundTrip();
    TestSrtcpRejectsTamperedAndShort();
    TestSrtcpReplayWindow();
    TestSrtcpSsrcLimit();
    return vms::test::ExitCode();
}
