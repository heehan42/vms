#include "recording/recording_file_name.h"

#include <charconv>
#include <chrono>
#include <format>


namespace vms::recording {


namespace {


// 이름에서 시각 부분("YYYYMMDDTHHMMSSZ")의 길이
constexpr std::size_t kTimestampLength = 16;


// 고정 길이의 10진수를 읽는다.
//
// text: 숫자만으로 이루어져야 하는 문자열
// 반환값: 읽은 값. 숫자가 아닌 글자가 있거나 비어 있으면 빈 optional
std::optional<int> ParseFixedDigits(std::string_view text) {
    int value = 0;
    if (text.empty()) {
        return std::nullopt;
    }
    for (const char digit : text) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        value = value * 10 + (digit - '0');
    }
    return value;
}


// "YYYYMMDDTHHMMSSZ"를 유닉스 초로 바꾼다.
//
// text: 시각 부분. 길이가 kTimestampLength여야 한다.
// 반환값: 유닉스 초. 형식이 틀렸거나 존재하지 않는 날짜·시각이면 빈 optional
std::optional<std::int64_t> ParseTimestamp(std::string_view text) {
    if (text.size() != kTimestampLength || text[8] != 'T' || text[15] != 'Z') {
        return std::nullopt;
    }
    const auto year = ParseFixedDigits(text.substr(0, 4));
    const auto month = ParseFixedDigits(text.substr(4, 2));
    const auto day = ParseFixedDigits(text.substr(6, 2));
    const auto hour = ParseFixedDigits(text.substr(9, 2));
    const auto minute = ParseFixedDigits(text.substr(11, 2));
    const auto second = ParseFixedDigits(text.substr(13, 2));
    if (!year || !month || !day || !hour || !minute || !second) {
        return std::nullopt;
    }

    const std::chrono::year_month_day date{std::chrono::year(*year), std::chrono::month(*month),
                                           std::chrono::day(*day)};
    if (!date.ok() || *hour > 23 || *minute > 59 || *second > 59) {
        return std::nullopt;
    }
    const std::chrono::sys_seconds time = std::chrono::sys_days(date) + std::chrono::hours(*hour) +
                                          std::chrono::minutes(*minute) + std::chrono::seconds(*second);
    return time.time_since_epoch().count();
}


// "-<번호>" 접미사를 해석한다.
//
// text: 시각 부분 뒤, 확장자 앞의 문자열
// 반환값: 번호. 비어 있으면 0. '-'로 시작하지 않거나, 번호가 0이거나 0으로 시작하거나 숫자가 아니면 빈 optional
std::optional<int> ParseCollisionSuffix(std::string_view text) {
    if (text.empty()) {
        return 0;
    }
    if (text.size() < 2 || text[0] != '-' || text[1] == '0') {
        return std::nullopt;
    }

    int number = 0;
    const char* const begin = text.data() + 1;
    const char* const end = text.data() + text.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, number);
    if (error != std::errc() || parsed_end != end || number < 1) {
        return std::nullopt;
    }
    return number;
}


}  // namespace


std::string FormatRecordingFileStem(std::int64_t start_unix_ms, int collision_number) {
    const std::chrono::sys_seconds start =
        std::chrono::floor<std::chrono::seconds>(std::chrono::sys_time<std::chrono::milliseconds>(
            std::chrono::milliseconds(start_unix_ms)));
    std::string stem = std::format("{:%Y%m%dT%H%M%SZ}", start);
    if (collision_number > 0) {
        stem += std::format("-{}", collision_number);
    }
    return stem;
}


std::optional<RecordingFileName> ParseRecordingFileName(std::string_view file_name) {
    RecordingFileName parsed;
    std::string_view stem;
    if (file_name.ends_with(kRecordingVideoExtension)) {
        parsed.kind = RecordingFileKind::kVideo;
        stem = file_name.substr(0, file_name.size() - kRecordingVideoExtension.size());
    } else if (file_name.ends_with(kRecordingIndexExtension)) {
        parsed.kind = RecordingFileKind::kIndex;
        stem = file_name.substr(0, file_name.size() - kRecordingIndexExtension.size());
    } else {
        return std::nullopt;
    }
    if (stem.size() < kTimestampLength) {
        return std::nullopt;
    }

    const auto start_seconds = ParseTimestamp(stem.substr(0, kTimestampLength));
    const auto collision_number = ParseCollisionSuffix(stem.substr(kTimestampLength));
    if (!start_seconds || !collision_number) {
        return std::nullopt;
    }
    parsed.stem = std::string(stem);
    parsed.start_unix_ms = *start_seconds * 1000;
    parsed.collision_number = *collision_number;
    return parsed;
}


std::int64_t SegmentNumberOf(std::int64_t unix_ms, int segment_seconds) {
    const std::int64_t segment_ms = static_cast<std::int64_t>(segment_seconds) * 1000;
    const std::int64_t quotient = unix_ms / segment_ms;

    // 정수 나눗셈은 0 쪽으로 자르므로, 1970년 이전 시각에서도 내림이 되도록 보정한다.
    return (unix_ms % segment_ms < 0) ? quotient - 1 : quotient;
}


}  // namespace vms::recording
