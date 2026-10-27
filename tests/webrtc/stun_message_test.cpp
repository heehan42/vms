#include "webrtc/stun_message.h"

#include <arpa/inet.h>

#include <string>
#include <utility>
#include <vector>

#include "test_support.h"

using vms::test::Expect;
using vms::webrtc::BuildBindingRequest;
using vms::webrtc::BuildBindingSuccessResponse;
using vms::webrtc::ComputeCrc32;
using vms::webrtc::ParseStunMessage;
using vms::webrtc::StunError;
using vms::webrtc::StunMessageType;
using vms::webrtc::StunTransactionId;
using vms::webrtc::VerifyFingerprint;
using vms::webrtc::VerifyMessageIntegrity;


namespace {


// RFC 5769 견본 메시지의 MESSAGE-INTEGRITY 키
constexpr std::string_view kRfc5769Password = "VOkJxbRl1RmTxUk/WvJxBt";


// RFC 5769 2.1 견본 요청(SOFTWARE, PRIORITY, ICE-CONTROLLED, USERNAME, MESSAGE-INTEGRITY, FINGERPRINT)
const std::vector<std::uint8_t> kRfc5769Request = {
    0x00, 0x01, 0x00, 0x58, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6,
    0x86, 0xfa, 0x87, 0xdf, 0xae, 0x80, 0x22, 0x00, 0x10, 0x53, 0x54, 0x55, 0x4e, 0x20, 0x74,
    0x65, 0x73, 0x74, 0x20, 0x63, 0x6c, 0x69, 0x65, 0x6e, 0x74, 0x00, 0x24, 0x00, 0x04, 0x6e,
    0x00, 0x01, 0xff, 0x80, 0x29, 0x00, 0x08, 0x93, 0x2f, 0xf9, 0xb1, 0x51, 0x26, 0x3b, 0x36,
    0x00, 0x06, 0x00, 0x09, 0x65, 0x76, 0x74, 0x6a, 0x3a, 0x68, 0x36, 0x76, 0x59, 0x20, 0x20,
    0x20, 0x00, 0x08, 0x00, 0x14, 0x9a, 0xea, 0xa7, 0x0c, 0xbf, 0xd8, 0xcb, 0x56, 0x78, 0x1e,
    0xf2, 0xb5, 0xb2, 0xd3, 0xf2, 0x49, 0xc1, 0xb5, 0x71, 0xa2, 0x80, 0x28, 0x00, 0x04, 0xe5,
    0x7a, 0x3b, 0xcf,
};


// RFC 5769 2.2 견본 IPv4 응답(SOFTWARE, XOR-MAPPED-ADDRESS, MESSAGE-INTEGRITY, FINGERPRINT)
const std::vector<std::uint8_t> kRfc5769Response = {
    0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6,
    0x86, 0xfa, 0x87, 0xdf, 0xae, 0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76,
    0x65, 0x63, 0x74, 0x6f, 0x72, 0x20, 0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1,
    0x12, 0xa6, 0x43, 0x00, 0x08, 0x00, 0x14, 0x2b, 0x91, 0xf5, 0x99, 0xfd, 0x9e, 0x90, 0xc3,
    0x8c, 0x74, 0x89, 0xf9, 0x2a, 0xf9, 0xba, 0x53, 0xf0, 0x6b, 0xe7, 0xd7, 0x80, 0x28, 0x00,
    0x04, 0xc0, 0x7d, 0x4c, 0x96,
};


// 주소와 포트로 IPv4 소켓 주소를 만든다.
//
// address: 점 표기 IPv4 주소
// port: 호스트 바이트 순서의 포트
// 반환값: 네트워크 바이트 순서로 채운 소켓 주소
sockaddr_in MakeEndpoint(const char* address, std::uint16_t port) {
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    ::inet_pton(AF_INET, address, &endpoint.sin_addr);
    return endpoint;
}


// CRC-32 표준 점검값("123456789" -> 0xCBF43926)을 검사한다.
void TestComputesCrc32CheckValue() {
    const std::string text = "123456789";
    const auto crc = ComputeCrc32(
        std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    Expect(crc == 0xCBF43926u, "CRC-32 점검값");
}


// RFC 5769 견본 요청을 해석하고 FINGERPRINT와 MESSAGE-INTEGRITY를 검증하는지 검사한다.
void TestParsesAndVerifiesRfc5769Request() {
    const auto message = ParseStunMessage(kRfc5769Request);
    if (!message) {
        Expect(false, "견본 요청 해석");
        return;
    }
    Expect(message->type == std::to_underlying(StunMessageType::kBindingRequest), "Binding 요청 타입");
    Expect(message->username == "evtj:h6vY", "패딩을 뺀 USERNAME");
    Expect(!message->has_use_candidate, "USE-CANDIDATE 없음");
    Expect(VerifyFingerprint(*message).has_value(), "FINGERPRINT 검증");
    Expect(VerifyMessageIntegrity(*message, kRfc5769Password).has_value(),
           "올바른 비밀번호로 MESSAGE-INTEGRITY 검증");

    const auto wrong = VerifyMessageIntegrity(*message, "wrong-password");
    Expect(!wrong && wrong.error() == StunError::kMessageIntegrityMismatch, "틀린 비밀번호 거부");
}


// RFC 5769 견본 응답의 XOR-MAPPED-ADDRESS를 192.0.2.1:32853으로 해독하는지 검사한다.
void TestDecodesRfc5769ResponseAddress() {
    const auto message = ParseStunMessage(kRfc5769Response);
    if (!message || !message->xor_mapped_address) {
        Expect(false, "견본 응답 해석과 XOR-MAPPED-ADDRESS");
        return;
    }
    const sockaddr_in expected = MakeEndpoint("192.0.2.1", 32853);
    Expect(message->xor_mapped_address->sin_addr.s_addr == expected.sin_addr.s_addr, "해독한 주소");
    Expect(message->xor_mapped_address->sin_port == expected.sin_port, "해독한 포트");
    Expect(VerifyFingerprint(*message).has_value(), "응답 FINGERPRINT 검증");
    Expect(VerifyMessageIntegrity(*message, kRfc5769Password).has_value(),
           "응답 MESSAGE-INTEGRITY 검증");
}


// 잘린 패킷, 잘못된 magic cookie, 속성이 끝을 넘는 패킷, 바뀐 FINGERPRINT를 거부하는지 검사한다.
void TestRejectsMalformedPackets() {
    const auto truncated = ParseStunMessage(std::span(kRfc5769Request).first(19));
    Expect(!truncated && truncated.error() == StunError::kNotStunMessage, "헤더보다 짧은 패킷");

    std::vector<std::uint8_t> wrong_cookie = kRfc5769Request;
    wrong_cookie[4] = 0x00;
    const auto cookie = ParseStunMessage(wrong_cookie);
    Expect(!cookie && cookie.error() == StunError::kNotStunMessage, "잘못된 magic cookie");

    std::vector<std::uint8_t> length_mismatch = kRfc5769Request;
    length_mismatch.resize(length_mismatch.size() - 4);
    const auto mismatch = ParseStunMessage(length_mismatch);
    Expect(!mismatch && mismatch.error() == StunError::kNotStunMessage,
           "길이 필드와 실제 크기가 다른 패킷");

    // 마지막 속성(FINGERPRINT)의 길이를 늘려 메시지 끝을 넘게 한다.
    std::vector<std::uint8_t> overflow = kRfc5769Request;
    overflow[overflow.size() - 5] = 0x08;
    const auto attribute = ParseStunMessage(overflow);
    Expect(!attribute && attribute.error() == StunError::kMalformedAttribute, "끝을 넘는 속성");

    std::vector<std::uint8_t> altered = kRfc5769Request;
    altered.back() ^= 0x01;
    const auto altered_message = ParseStunMessage(altered);
    const auto fingerprint = altered_message ? VerifyFingerprint(*altered_message)
                                             : std::unexpected(StunError::kNotStunMessage);
    Expect(!fingerprint && fingerprint.error() == StunError::kFingerprintMismatch,
           "바뀐 FINGERPRINT 거부");
}


// 직접 만든 성공 응답이 해석과 두 검증을 통과하고 주소를 그대로 담는지 검사한다.
void TestBuildsVerifiableSuccessResponse() {
    const StunTransactionId transaction_id = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const sockaddr_in mapped = MakeEndpoint("172.27.208.1", 54321);
    const auto response = BuildBindingSuccessResponse(transaction_id, mapped, "server-password");
    if (!response) {
        Expect(false, "성공 응답 생성");
        return;
    }

    const auto message = ParseStunMessage(*response);
    if (!message) {
        Expect(false, "생성한 응답 해석");
        return;
    }
    Expect(message->type == std::to_underlying(StunMessageType::kBindingSuccessResponse),
           "성공 응답 타입");
    Expect(message->transaction_id == transaction_id, "요청과 같은 트랜잭션 ID");
    Expect(message->xor_mapped_address &&
               message->xor_mapped_address->sin_addr.s_addr == mapped.sin_addr.s_addr &&
               message->xor_mapped_address->sin_port == mapped.sin_port,
           "XOR-MAPPED-ADDRESS 왕복");
    Expect(VerifyFingerprint(*message).has_value(), "생성한 응답 FINGERPRINT");
    Expect(VerifyMessageIntegrity(*message, "server-password").has_value(),
           "생성한 응답 MESSAGE-INTEGRITY");
}


// 직접 만든 요청이 USERNAME, USE-CANDIDATE를 담고 검증을 통과하는지 검사한다.
void TestBuildsVerifiableRequest() {
    const StunTransactionId transaction_id = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    const auto request = BuildBindingRequest(transaction_id, "server:brow", "server-password", true);
    const auto message = request ? ParseStunMessage(*request)
                                 : std::unexpected(StunError::kMessageIntegrityComputationFailed);
    if (!message) {
        Expect(false, "요청 생성과 해석");
        return;
    }
    Expect(message->username == "server:brow", "USERNAME");
    Expect(message->has_use_candidate, "USE-CANDIDATE");
    Expect(VerifyFingerprint(*message).has_value(), "요청 FINGERPRINT");
    Expect(VerifyMessageIntegrity(*message, "server-password").has_value(),
           "요청 MESSAGE-INTEGRITY");
}


}  // namespace


int main() {
    TestComputesCrc32CheckValue();
    TestParsesAndVerifiesRfc5769Request();
    TestDecodesRfc5769ResponseAddress();
    TestRejectsMalformedPackets();
    TestBuildsVerifiableSuccessResponse();
    TestBuildsVerifiableRequest();
    return vms::test::ExitCode();
}
