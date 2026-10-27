#include "webrtc/sdp.h"

#include <algorithm>
#include <format>
#include <ranges>

#include "core/ascii.h"


namespace vms::webrtc {


namespace {


// 협상 대상 비디오 섹션에 요구하는 전송 프로토콜(DTLS-SRTP 위의 RTP)
constexpr std::string_view kSecureRtpProtocol = "UDP/TLS/RTP/SAVPF";


// 우선해서 고르는 H.264 프로파일(Constrained Baseline, 레벨 3.1)
constexpr std::string_view kPreferredH264Profile = "42e01f";


// 문자열을 공백 기준으로 나눈다. 연속된 공백은 하나로 본다.
//
// text: 나눌 문자열
// 반환값: 빈 항목을 뺀 조각 목록. text를 가리키므로 text가 살아 있는 동안에만 유효하다.
std::vector<std::string_view> SplitBySpace(std::string_view text) {
    std::vector<std::string_view> tokens;
    for (const auto token : std::views::split(text, ' ')) {
        if (!token.empty()) {
            tokens.emplace_back(token.begin(), token.end());
        }
    }
    return tokens;
}


// 속성 목록에서 이름이 같은 첫 속성의 값을 찾는다.
//
// attributes: 찾을 속성 목록
// name: 속성 이름
// 반환값: 속성 값. 없으면 빈 optional. attributes가 살아 있는 동안에만 유효하다.
std::optional<std::string_view> FindAttribute(std::span<const SdpAttribute> attributes,
                                              std::string_view name) {
    const auto found = std::ranges::find(attributes, name, &SdpAttribute::name);
    if (found == attributes.end()) {
        return std::nullopt;
    }
    return found->value;
}


// ICE 자격 증명 속성 값을 찾는다. 미디어 수준을 우선하고, 없으면 세션 수준을 본다.
//
// description: 나눈 SDP
// section: 협상 대상 섹션
// name: "ice-ufrag" 또는 "ice-pwd"
// 반환값: 비어 있지 않은 값. 어느 수준에도 없으면 빈 optional
std::optional<std::string_view> FindIceCredential(const SdpSessionDescription& description,
                                                  const SdpMediaSection& section,
                                                  std::string_view name) {
    auto value = FindAttribute(section.attributes, name);
    if (!value) {
        value = FindAttribute(description.session_attributes, name);
    }
    if (!value || value->empty()) {
        return std::nullopt;
    }
    return value;
}


// 세션 수준 a=group:BUNDLE에 속한 mid 목록을 찾는다.
//
// description: 나눈 SDP
// 반환값: BUNDLE 그룹의 mid 목록. 그룹이 없으면 빈 optional. description이 살아 있는 동안에만 유효하다.
std::optional<std::vector<std::string_view>> FindBundleMids(const SdpSessionDescription& description) {
    for (const SdpAttribute& attribute : description.session_attributes) {
        if (attribute.name != "group") {
            continue;
        }
        auto tokens = SplitBySpace(attribute.value);
        if (!tokens.empty() && tokens.front() == "BUNDLE") {
            tokens.erase(tokens.begin());
            return tokens;
        }
    }
    return std::nullopt;
}


// 협상 대상 비디오 섹션의 위치를 고른다.
//
// description: 나눈 SDP
// bundle_mids: FindBundleMids의 결과
// 반환값: 포트가 0이 아닌 첫 비디오 섹션의 위치. BUNDLE 그룹이 있으면 그 그룹에 mid가 있는 섹션만
//   고른다. 없으면 빈 optional
std::optional<std::size_t> SelectVideoSection(
    const SdpSessionDescription& description,
    const std::optional<std::vector<std::string_view>>& bundle_mids) {
    for (std::size_t index = 0; index < description.media_sections.size(); ++index) {
        const SdpMediaSection& section = description.media_sections[index];
        if (section.media != "video" || section.port == "0") {
            continue;
        }
        if (bundle_mids) {
            const auto mid = FindAttribute(section.attributes, "mid");
            if (!mid || std::ranges::find(*bundle_mids, *mid) == bundle_mids->end()) {
                continue;
            }
        }
        return index;
    }
    return std::nullopt;
}


// 포맷 매개변수 문자열에 "이름=값" 항목이 있는지 확인한다. 이름과 값은 대소문자를 무시한다.
//
// format_parameters: "a=b;c=d" 형식의 fmtp 매개변수
// name: 찾을 매개변수 이름
// value: 기대하는 값
// 반환값: 일치하는 항목이 있으면 true
bool HasFormatParameter(std::string_view format_parameters, std::string_view name,
                        std::string_view value) {
    for (const auto item : std::views::split(format_parameters, ';')) {
        const std::string_view entry(item.begin(), item.end());
        const std::size_t equals = entry.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        // "a=b; c=d"처럼 ';' 앞뒤에 공백이 올 수 있다.
        const std::string_view key = core::TrimAsciiWhitespace(entry.substr(0, equals));
        const std::string_view parameter_value = entry.substr(equals + 1);
        if (core::EqualsIgnoringAsciiCase(key, name) &&
            core::EqualsIgnoringAsciiCase(core::TrimAsciiWhitespace(parameter_value), value)) {
            return true;
        }
    }
    return false;
}


// 섹션에서 packetization-mode=1인 H.264 코덱을 고른다.
//
// section: 협상 대상 비디오 섹션
// 반환값: 고른 코덱. 42e01f 프로파일을 우선하며, 해당 코덱이 없으면 빈 optional
std::optional<H264Codec> SelectH264Codec(const SdpMediaSection& section) {
    std::optional<H264Codec> first_candidate;
    for (const SdpAttribute& rtpmap : section.attributes) {
        if (rtpmap.name != "rtpmap") {
            continue;
        }
        // "96 H264/90000" 형식이다.
        const std::size_t space = rtpmap.value.find(' ');
        if (space == std::string::npos ||
            !core::EqualsIgnoringAsciiCase(std::string_view(rtpmap.value).substr(space + 1), "H264/90000")) {
            continue;
        }
        const std::string payload_type = rtpmap.value.substr(0, space);
        if (std::ranges::find(section.formats, payload_type) == section.formats.end()) {
            continue;
        }

        // 같은 payload type의 a=fmtp에서 매개변수를 찾는다.
        const auto fmtp = std::ranges::find_if(section.attributes, [&](const SdpAttribute& attribute) {
            return attribute.name == "fmtp" && attribute.value.starts_with(payload_type + " ");
        });
        if (fmtp == section.attributes.end()) {
            continue;
        }
        const std::string format_parameters = fmtp->value.substr(payload_type.size() + 1);
        if (!HasFormatParameter(format_parameters, "packetization-mode", "1")) {
            continue;
        }

        H264Codec codec{.payload_type = payload_type, .format_parameters = format_parameters};
        if (HasFormatParameter(format_parameters, "profile-level-id", kPreferredH264Profile)) {
            return codec;
        }
        if (!first_candidate) {
            first_candidate = std::move(codec);
        }
    }
    return first_candidate;
}


// 포맷 목록을 공백으로 이어 붙인다.
//
// formats: 포맷 목록
// 반환값: "96 97 98" 형식의 문자열
std::string JoinFormats(std::span<const std::string> formats) {
    std::string joined;
    for (const std::string& format : formats) {
        if (!joined.empty()) {
            joined += ' ';
        }
        joined += format;
    }
    return joined;
}


}  // namespace


std::expected<SdpSessionDescription, std::string> ParseSdp(std::string_view text) {
    SdpSessionDescription description;
    bool is_first_line = true;

    for (const auto raw_line : std::views::split(text, '\n')) {
        std::string_view line(raw_line.begin(), raw_line.end());
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
        if (line.size() < 2 || line[1] != '=' || line[0] < 'a' || line[0] > 'z') {
            return std::unexpected(std::format("SDP 줄 형식이 아님: {}", line));
        }
        if (is_first_line && line != "v=0") {
            return std::unexpected("첫 줄이 v=0이 아님");
        }
        is_first_line = false;

        const char type = line[0];
        const std::string_view value = line.substr(2);
        if (type == 'm') {
            const auto tokens = SplitBySpace(value);
            if (tokens.size() < 4) {
                return std::unexpected(std::format("m= 줄 항목이 부족함: {}", line));
            }
            SdpMediaSection section;
            section.media = tokens[0];
            section.port = tokens[1];
            section.protocol = tokens[2];
            section.formats.assign(tokens.begin() + 3, tokens.end());
            description.media_sections.push_back(std::move(section));
        } else if (type == 'a') {
            const std::size_t colon = value.find(':');
            SdpAttribute attribute{.name = std::string(value.substr(0, colon)), .value = {}};
            if (colon != std::string_view::npos) {
                attribute.value = value.substr(colon + 1);
            }
            // 첫 m= 줄 앞의 속성은 세션 수준, 뒤의 속성은 가장 최근 미디어 섹션의 것이다.
            auto& attributes = description.media_sections.empty()
                                   ? description.session_attributes
                                   : description.media_sections.back().attributes;
            attributes.push_back(std::move(attribute));
        }
    }

    if (is_first_line) {
        return std::unexpected("빈 SDP");
    }
    return description;
}


std::expected<SdpOffer, std::string> InterpretOffer(const SdpSessionDescription& description) {
    const auto bundle_mids = FindBundleMids(description);
    const auto video_index = SelectVideoSection(description, bundle_mids);
    if (!video_index) {
        return std::unexpected("협상할 비디오 섹션이 없음");
    }
    const SdpMediaSection& video = description.media_sections[*video_index];
    if (video.protocol != kSecureRtpProtocol) {
        return std::unexpected(std::format("지원하지 않는 비디오 프로토콜: {}", video.protocol));
    }
    // answer의 미디어 섹션과 BUNDLE 그룹이 mid로 offer 섹션을 가리키므로 mid가 꼭 있어야 한다.
    const auto video_mid = FindAttribute(video.attributes, "mid");
    if (!video_mid || video_mid->empty()) {
        return std::unexpected("비디오 섹션에 mid가 없음");
    }

    const auto ice_ufrag = FindIceCredential(description, video, "ice-ufrag");
    const auto ice_pwd = FindIceCredential(description, video, "ice-pwd");
    if (!ice_ufrag || !ice_pwd) {
        return std::unexpected("ICE ufrag 또는 pwd가 없음");
    }
    auto h264 = SelectH264Codec(video);
    if (!h264) {
        return std::unexpected("packetization-mode=1인 H.264가 없음");
    }

    SdpOffer offer;
    offer.video_mid = *video_mid;
    offer.has_bundle_group = bundle_mids.has_value();
    offer.ice_ufrag = *ice_ufrag;
    offer.ice_pwd = *ice_pwd;
    offer.h264 = *std::move(h264);
    for (std::size_t index = 0; index < description.media_sections.size(); ++index) {
        const SdpMediaSection& section = description.media_sections[index];
        OfferedMediaSection offered;
        offered.media = section.media;
        offered.protocol = section.protocol;
        offered.formats = section.formats;
        if (const auto mid = FindAttribute(section.attributes, "mid")) {
            offered.mid = std::string(*mid);
        }
        offered.is_selected = index == *video_index;
        offer.media_sections.push_back(std::move(offered));
    }
    return offer;
}


std::string BuildSdpAnswer(const SdpOffer& offer, const SdpAnswerParameters& parameters) {
    std::string answer = std::format(
        "v=0\r\n"
        "o=- {} 2 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n",
        parameters.session_id);
    const auto append = [&answer]<typename... Arguments>(std::format_string<Arguments...> format,
                                                        Arguments&&... arguments) {
        std::format_to(std::back_inserter(answer), format, std::forward<Arguments>(arguments)...);
    };

    // 거절한 섹션은 BUNDLE에서 빠지므로 그룹에는 고른 섹션만 남는다.
    if (offer.has_bundle_group) {
        append("a=group:BUNDLE {}\r\n", offer.video_mid);
    }
    append("a=ice-lite\r\n");

    for (const OfferedMediaSection& section : offer.media_sections) {
        if (!section.is_selected) {
            // 거절한 섹션은 포트 0으로 두고, offer와 같은 미디어·프로토콜·포맷·mid를 유지해야 한다.
            append("m={} 0 {} {}\r\nc=IN IP4 0.0.0.0\r\n", section.media, section.protocol,
                   JoinFormats(section.formats));
            if (section.mid) {
                append("a=mid:{}\r\n", *section.mid);
            }
            continue;
        }

        const std::string& payload_type = offer.h264.payload_type;
        append("m=video 9 {} {}\r\n", kSecureRtpProtocol, payload_type);
        append("c=IN IP4 0.0.0.0\r\n");
        append("a=mid:{}\r\n", offer.video_mid);
        append("a=sendonly\r\n");
        append("a=rtcp-mux\r\n");
        append("a=rtpmap:{} H264/90000\r\n", payload_type);
        append("a=fmtp:{} {}\r\n", payload_type, offer.h264.format_parameters);
        append("a=ice-ufrag:{}\r\n", parameters.ice_ufrag);
        append("a=ice-pwd:{}\r\n", parameters.ice_pwd);
        append("a=fingerprint:sha-256 {}\r\n", parameters.sha256_fingerprint);
        append("a=setup:passive\r\n");
        for (const IceCandidate& candidate : parameters.candidates) {
            append("a=candidate:{} 1 udp {} {} {} typ host\r\n", candidate.foundation,
                   candidate.priority, candidate.address, candidate.port);
        }
        append("a=end-of-candidates\r\n");
    }
    return answer;
}


}  // namespace vms::webrtc
