#include "webrtc/stun_message.h"

#include <arpa/inet.h>
#include <openssl/crypto.h>

#include <algorithm>
#include <utility>

#include "crypto/hmac_sha1.h"


namespace vms::webrtc {


namespace {


// 서버가 해석하거나 만드는 STUN 속성 타입
enum class AttributeType : std::uint16_t {
    // "서버ufrag:브라우저ufrag". 서버는 앞부분으로 세션을 찾는다.
    kUsername = 0x0006,

    // HMAC-SHA1 서명(20바이트). 키는 요청을 받는 쪽의 ice-pwd다.
    kMessageIntegrity = 0x0008,

    // 요청을 보낸 쪽의 주소와 포트를 magic cookie와 XOR한 값. 성공 응답에 넣는다.
    kXorMappedAddress = 0x0020,

    // 브라우저가 이 candidate 쌍을 지명한다는 표시. 값이 없다.
    kUseCandidate = 0x0025,

    // CRC-32 XOR 0x5354554E(4바이트). STUN 메시지인지 확인하며 항상 마지막 속성이다.
    kFingerprint = 0x8028,
};


// 속성 헤더(타입 2바이트, 길이 2바이트) 크기
constexpr std::size_t kAttributeHeaderSize = 4;


// MESSAGE-INTEGRITY 값(HMAC-SHA1) 크기
constexpr std::size_t kMessageIntegrityValueSize = crypto::kHmacSha1Size;


// FINGERPRINT 값 크기
constexpr std::size_t kFingerprintValueSize = 4;


// FINGERPRINT 계산 시 CRC-32에 XOR하는 값. ASCII로 "STUN"이다.
constexpr std::uint32_t kFingerprintXor = 0x5354554E;


// XOR-MAPPED-ADDRESS의 IPv4 주소 계열 값
constexpr std::uint8_t kAddressFamilyIpv4 = 0x01;


// 다항식 0xEDB88320(비트 반전 표현)의 CRC-32 표를 만든다.
//
// 반환값: 바이트 값마다의 CRC-32 중간값 256개
constexpr std::array<std::uint32_t, 256> MakeCrc32Table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t value = 0; value < table.size(); ++value) {
        std::uint32_t crc = value;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
        table[value] = crc;
    }
    return table;
}


// CRC-32 계산에 쓰는 표. 컴파일 시점에 만든다.
constexpr std::array<std::uint32_t, 256> kCrc32Table = MakeCrc32Table();


// 빅엔디언 16비트 값을 읽는다.
//
// bytes: 읽을 위치에서 시작하는 2바이트 이상의 범위
// 반환값: 읽은 값
std::uint16_t ReadUint16(std::span<const std::uint8_t> bytes) {
    return static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]);
}


// 빅엔디언 32비트 값을 읽는다.
//
// bytes: 읽을 위치에서 시작하는 4바이트 이상의 범위
// 반환값: 읽은 값
std::uint32_t ReadUint32(std::span<const std::uint8_t> bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]);
}


// 빅엔디언 16비트 값을 버퍼 끝에 붙인다.
//
// buffer: 쓸 버퍼
// value: 쓸 값
void AppendUint16(std::vector<std::uint8_t>& buffer, std::uint16_t value) {
    buffer.push_back(static_cast<std::uint8_t>(value >> 8));
    buffer.push_back(static_cast<std::uint8_t>(value));
}


// 빅엔디언 32비트 값을 버퍼 끝에 붙인다.
//
// buffer: 쓸 버퍼
// value: 쓸 값
void AppendUint32(std::vector<std::uint8_t>& buffer, std::uint32_t value) {
    AppendUint16(buffer, static_cast<std::uint16_t>(value >> 16));
    AppendUint16(buffer, static_cast<std::uint16_t>(value));
}


// 빅엔디언 16비트 값을 버퍼의 지정 위치에 덮어쓴다.
//
// buffer: 쓸 버퍼. position + 2바이트 이상이어야 한다.
// position: 쓸 위치
// value: 쓸 값
void WriteUint16(std::span<std::uint8_t> buffer, std::size_t position, std::uint16_t value) {
    buffer[position] = static_cast<std::uint8_t>(value >> 8);
    buffer[position + 1] = static_cast<std::uint8_t>(value);
}


// 값 길이를 4바이트 경계로 올린다. 속성 값 뒤에는 이 크기까지 패딩이 붙는다.
//
// length: 속성 값 길이
// 반환값: 패딩을 포함한 길이
constexpr std::size_t PaddedLength(std::size_t length) { return (length + 3) & ~std::size_t{3}; }


// XOR-MAPPED-ADDRESS 값을 해독한다.
//
// value: 속성 값(패딩 제외)
// 반환값: IPv4 주소와 포트. 길이가 8이 아니거나 IPv4가 아니면 빈 optional
std::optional<sockaddr_in> DecodeXorMappedAddress(std::span<const std::uint8_t> value) {
    if (value.size() != 8 || value[1] != kAddressFamilyIpv4) {
        return std::nullopt;
    }

    // XOR 키와 다시 XOR하면 원래 값으로 돌아온다. 포트는 cookie 상위 16비트와, 주소는 cookie 전체와 섞었다.
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(ReadUint16(value.subspan(2)) ^
                                                        (kStunMagicCookie >> 16)));
    address.sin_addr.s_addr = htonl(ReadUint32(value.subspan(4)) ^ kStunMagicCookie);
    return address;
}


// STUN 메시지를 앞에서부터 쌓아 가며 헤더의 길이 필드를 맞춰 주는 작성기.
//
// MESSAGE-INTEGRITY와 FINGERPRINT는 그 시점까지의 내용으로 계산하므로 마지막에 차례로 붙여야 한다.
class StunMessageWriter {
public:
    // 헤더를 쓰고 속성이 없는 메시지로 시작한다.
    //
    // type: 메시지 타입
    // transaction_id: 트랜잭션 ID
    StunMessageWriter(StunMessageType type, const StunTransactionId& transaction_id) {
        AppendUint16(buffer_, std::to_underlying(type));
        AppendUint16(buffer_, 0);
        AppendUint32(buffer_, kStunMagicCookie);
        buffer_.insert(buffer_.end(), transaction_id.begin(), transaction_id.end());
    }


    // 속성 하나를 패딩과 함께 붙이고 길이 필드를 고친다.
    //
    // type: 속성 타입
    // value: 속성 값(패딩 제외)
    void AppendAttribute(AttributeType type, std::span<const std::uint8_t> value) {
        AppendUint16(buffer_, std::to_underlying(type));
        AppendUint16(buffer_, static_cast<std::uint16_t>(value.size()));
        buffer_.insert(buffer_.end(), value.begin(), value.end());
        buffer_.resize(buffer_.size() + PaddedLength(value.size()) - value.size(), 0);
        UpdateLengthField();
    }


    // 지금까지의 내용으로 MESSAGE-INTEGRITY를 계산해 붙인다.
    //
    // password: HMAC 키
    // 반환값: HMAC 계산이 실패하면 kMessageIntegrityComputationFailed
    std::expected<void, StunError> AppendMessageIntegrity(std::string_view password) {
        // 길이 필드가 MESSAGE-INTEGRITY 속성 끝까지를 가리키는 상태로 계산해야 한다.
        const std::size_t integrity_offset = buffer_.size();
        WriteUint16(buffer_, 2,
                    static_cast<std::uint16_t>(integrity_offset + kAttributeHeaderSize +
                                               kMessageIntegrityValueSize - kStunHeaderSize));
        const auto digest = crypto::ComputeHmacSha1(password, buffer_);
        if (!digest) {
            return std::unexpected(StunError::kMessageIntegrityComputationFailed);
        }
        AppendAttribute(AttributeType::kMessageIntegrity, *digest);
        return {};
    }


    // 지금까지의 내용으로 FINGERPRINT를 계산해 붙인다. 마지막 속성이어야 한다.
    void AppendFingerprint() {
        // 길이 필드가 FINGERPRINT 속성 끝까지를 가리키는 상태로 계산해야 한다.
        WriteUint16(buffer_, 2,
                    static_cast<std::uint16_t>(buffer_.size() + kAttributeHeaderSize +
                                               kFingerprintValueSize - kStunHeaderSize));
        const std::uint32_t fingerprint = ComputeCrc32(buffer_) ^ kFingerprintXor;
        std::vector<std::uint8_t> value;
        AppendUint32(value, fingerprint);
        AppendAttribute(AttributeType::kFingerprint, value);
    }


    // 완성한 메시지 바이트를 넘겨준다.
    //
    // 반환값: 메시지 바이트. 호출 후 이 작성기는 쓰지 않는다.
    std::vector<std::uint8_t> TakeBytes() { return std::move(buffer_); }


private:
    // 헤더의 길이 필드를 현재 속성 영역 크기로 고친다.
    void UpdateLengthField() {
        WriteUint16(buffer_, 2, static_cast<std::uint16_t>(buffer_.size() - kStunHeaderSize));
    }


    // 작성 중인 메시지 바이트
    std::vector<std::uint8_t> buffer_;
};


}  // namespace


std::string_view StunErrorDescription(StunError error) {
    switch (error) {
        case StunError::kNotStunMessage:
            return "STUN 헤더 형식이 아님";
        case StunError::kMalformedAttribute:
            return "속성 구조가 잘못됨";
        case StunError::kMissingFingerprint:
            return "FINGERPRINT 없음";
        case StunError::kFingerprintMismatch:
            return "FINGERPRINT 불일치";
        case StunError::kMissingMessageIntegrity:
            return "MESSAGE-INTEGRITY 없음";
        case StunError::kMessageIntegrityMismatch:
            return "MESSAGE-INTEGRITY 불일치";
        case StunError::kMessageIntegrityComputationFailed:
            return "MESSAGE-INTEGRITY 계산 실패";
    }
    return "알 수 없는 STUN 오류";
}


std::uint32_t ComputeCrc32(std::span<const std::uint8_t> data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::uint8_t value : data) {
        crc = kCrc32Table[(crc ^ value) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}


std::expected<StunMessage, StunError> ParseStunMessage(std::span<const std::uint8_t> packet) {
    // STUN 메시지는 앞 2비트가 0이고, 길이 필드가 헤더 뒤 크기와 같으며 4의 배수다.
    if (packet.size() < kStunHeaderSize || (packet[0] & 0xC0) != 0 ||
        ReadUint16(packet.subspan(2)) != packet.size() - kStunHeaderSize ||
        packet.size() % 4 != 0 || ReadUint32(packet.subspan(4)) != kStunMagicCookie) {
        return std::unexpected(StunError::kNotStunMessage);
    }

    StunMessage message;
    message.type = ReadUint16(packet);
    std::ranges::copy(packet.subspan(8, message.transaction_id.size()),
                      message.transaction_id.begin());
    message.packet = packet;

    std::size_t offset = kStunHeaderSize;
    while (offset < packet.size()) {
        // FINGERPRINT는 항상 마지막 속성이어야 한다.
        if (message.fingerprint_offset) {
            return std::unexpected(StunError::kMalformedAttribute);
        }
        if (packet.size() - offset < kAttributeHeaderSize) {
            return std::unexpected(StunError::kMalformedAttribute);
        }
        const std::uint16_t type = ReadUint16(packet.subspan(offset));
        const std::size_t length = ReadUint16(packet.subspan(offset + 2));
        const std::size_t value_offset = offset + kAttributeHeaderSize;
        if (packet.size() - value_offset < PaddedLength(length)) {
            return std::unexpected(StunError::kMalformedAttribute);
        }
        const std::span<const std::uint8_t> value = packet.subspan(value_offset, length);

        // MESSAGE-INTEGRITY 뒤에 오는 속성은 무결성 보호를 받지 않으므로 FINGERPRINT 외에는 쓰지 않는다.
        const bool is_after_integrity = message.message_integrity_offset.has_value();
        switch (static_cast<AttributeType>(type)) {
            case AttributeType::kUsername:
                if (!is_after_integrity) {
                    message.username =
                        std::string_view(reinterpret_cast<const char*>(value.data()), value.size());
                }
                break;
            case AttributeType::kUseCandidate:
                if (!is_after_integrity) {
                    message.has_use_candidate = true;
                }
                break;
            case AttributeType::kXorMappedAddress:
                if (!is_after_integrity) {
                    message.xor_mapped_address = DecodeXorMappedAddress(value);
                }
                break;
            case AttributeType::kMessageIntegrity:
                if (length != kMessageIntegrityValueSize) {
                    return std::unexpected(StunError::kMalformedAttribute);
                }
                if (!is_after_integrity) {
                    message.message_integrity_offset = offset;
                }
                break;
            case AttributeType::kFingerprint:
                if (length != kFingerprintValueSize) {
                    return std::unexpected(StunError::kMalformedAttribute);
                }
                message.fingerprint_offset = offset;
                break;
        }
        offset = value_offset + PaddedLength(length);
    }
    return message;
}


std::expected<void, StunError> VerifyFingerprint(const StunMessage& message) {
    if (!message.fingerprint_offset) {
        return std::unexpected(StunError::kMissingFingerprint);
    }

    // FINGERPRINT는 마지막 속성이므로 길이 필드를 고치지 않은 원본 그대로 계산한다.
    const std::size_t offset = *message.fingerprint_offset;
    const std::uint32_t expected = ComputeCrc32(message.packet.first(offset)) ^ kFingerprintXor;
    const std::uint32_t received = ReadUint32(message.packet.subspan(offset + kAttributeHeaderSize));
    if (expected != received) {
        return std::unexpected(StunError::kFingerprintMismatch);
    }
    return {};
}


std::expected<void, StunError> VerifyMessageIntegrity(const StunMessage& message,
                                                      std::string_view password) {
    if (!message.message_integrity_offset) {
        return std::unexpected(StunError::kMissingMessageIntegrity);
    }

    // 길이 필드를 이 속성 끝까지로 고친 헤더와, 원본의 MESSAGE-INTEGRITY 앞까지 속성으로 계산한다.
    // 고칠 곳은 헤더뿐이므로 헤더만 복사하고 속성은 원본을 그대로 넘긴다.
    const std::size_t offset = *message.message_integrity_offset;
    std::array<std::uint8_t, kStunHeaderSize> header{};
    std::ranges::copy(message.packet.first(kStunHeaderSize), header.begin());
    WriteUint16(header, 2,
                static_cast<std::uint16_t>(offset + kAttributeHeaderSize +
                                           kMessageIntegrityValueSize - kStunHeaderSize));
    const std::array<std::span<const std::uint8_t>, 2> signed_segments = {
        std::span<const std::uint8_t>(header),
        message.packet.subspan(kStunHeaderSize, offset - kStunHeaderSize),
    };
    const auto digest = crypto::ComputeHmacSha1(password, signed_segments);
    if (!digest) {
        return std::unexpected(StunError::kMessageIntegrityComputationFailed);
    }

    const std::span<const std::uint8_t> received =
        message.packet.subspan(offset + kAttributeHeaderSize, kMessageIntegrityValueSize);
    if (::CRYPTO_memcmp(digest->data(), received.data(), kMessageIntegrityValueSize) != 0) {
        return std::unexpected(StunError::kMessageIntegrityMismatch);
    }
    return {};
}


std::expected<std::vector<std::uint8_t>, StunError> BuildBindingSuccessResponse(
    const StunTransactionId& transaction_id, const sockaddr_in& mapped_address,
    std::string_view password) {
    std::vector<std::uint8_t> address_value = {0, kAddressFamilyIpv4};
    AppendUint16(address_value,
                 static_cast<std::uint16_t>(ntohs(mapped_address.sin_port) ^ (kStunMagicCookie >> 16)));
    AppendUint32(address_value, ntohl(mapped_address.sin_addr.s_addr) ^ kStunMagicCookie);

    StunMessageWriter writer(StunMessageType::kBindingSuccessResponse, transaction_id);
    writer.AppendAttribute(AttributeType::kXorMappedAddress, address_value);
    if (auto appended = writer.AppendMessageIntegrity(password); !appended) {
        return std::unexpected(appended.error());
    }
    writer.AppendFingerprint();
    return writer.TakeBytes();
}


std::expected<std::vector<std::uint8_t>, StunError> BuildBindingRequest(
    const StunTransactionId& transaction_id, std::string_view username, std::string_view password,
    bool use_candidate) {
    StunMessageWriter writer(StunMessageType::kBindingRequest, transaction_id);
    writer.AppendAttribute(
        AttributeType::kUsername,
        std::span(reinterpret_cast<const std::uint8_t*>(username.data()), username.size()));
    if (use_candidate) {
        writer.AppendAttribute(AttributeType::kUseCandidate, {});
    }
    if (auto appended = writer.AppendMessageIntegrity(password); !appended) {
        return std::unexpected(appended.error());
    }
    writer.AppendFingerprint();
    return writer.TakeBytes();
}


}  // namespace vms::webrtc
