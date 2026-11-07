#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>


namespace vms::recording {


// 녹화 파일(fMP4)의 확장자
inline constexpr std::string_view kRecordingVideoExtension = ".mp4";


// 녹화 파일마다 같은 이름으로 두는 색인 파일의 확장자
inline constexpr std::string_view kRecordingIndexExtension = ".idx";


// 녹화 파일 이름이 가리키는 파일 종류.
enum class RecordingFileKind {
    // fMP4 녹화 파일(.mp4)
    kVideo,

    // 색인 파일(.idx)
    kIndex,
};


// 규칙에 맞는 녹화 파일 이름을 해석한 결과.
struct RecordingFileName {
    // 확장자를 뺀 이름. 같은 녹화의 .mp4와 .idx를 짝짓는 데 쓴다.
    std::string stem;


    // 이름이 가리키는 시작 시각(유닉스 밀리초). 이름은 초 단위이므로 항상 1000의 배수다.
    std::int64_t start_unix_ms = 0;


    // 같은 이름을 피하려고 붙인 번호. 붙이지 않았으면 0이다.
    int collision_number = 0;


    // 파일 종류
    RecordingFileKind kind = RecordingFileKind::kVideo;
};


// 시작 시각과 충돌 번호로 녹화 파일 이름의 확장자 앞부분을 만든다.
//
// start_unix_ms: 파일 시작 시각(유닉스 밀리초, UTC). 초 아래는 내린다.
// collision_number: 같은 이름이 있을 때 붙이는 번호. 0이면 붙이지 않고, 1 이상이면 "-<번호>"를 붙인다.
// 반환값: "YYYYMMDDTHHMMSSZ" 또는 "YYYYMMDDTHHMMSSZ-<번호>"
std::string FormatRecordingFileStem(std::int64_t start_unix_ms, int collision_number);


// 파일 이름이 녹화 파일 이름 규칙에 맞는지 확인하고 해석한다.
//
// file_name: 디렉터리를 뺀 파일 이름
// 반환값: 해석 결과. 다음 경우 빈 optional
//   - 확장자가 .mp4·.idx가 아님
//   - 날짜·시각이 형식에 맞지 않거나 존재하지 않는 날짜·시각임
//   - 번호가 0이거나 0으로 시작하거나 숫자가 아님
//
// FormatRecordingFileStem이 만드는 이름만 받아들이므로, 해석한 값으로 다시 만들면 같은 이름이 된다.
std::optional<RecordingFileName> ParseRecordingFileName(std::string_view file_name);


// 시각이 속한 단위 구간 번호를 구한다.
//
// unix_ms: 유닉스 밀리초
// segment_seconds: 단위 시간(초). 1 이상이어야 한다.
// 반환값: floor(유닉스 초 / segment_seconds). 모든 카메라가 같은 경계를 쓴다.
std::int64_t SegmentNumberOf(std::int64_t unix_ms, int segment_seconds);


}  // namespace vms::recording
