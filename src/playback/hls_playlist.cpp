#include "playback/hls_playlist.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <iterator>

#include "http/query_string.h"
#include "recording/recording_index.h"


namespace vms::playback {


namespace {


// 조각 길이를 #EXTINF 값으로 쓴다.
//
// duration: 조각 길이(1/90000초 단위)
// 반환값: 초 단위 값을 소수점 아래 세 자리로 반올림한 문자열. 예: 89910 → "0.999"
std::string FormatSegmentDuration(std::uint32_t duration) {
    const std::uint64_t milliseconds =
        (static_cast<std::uint64_t>(duration) * 1000 + recording::kRecordingTimescale / 2) / recording::kRecordingTimescale;
    return std::format("{}.{:03}", milliseconds / 1000, milliseconds % 1000);
}


// 파일 시작 시각을 #EXT-X-PROGRAM-DATE-TIME 값으로 쓴다.
//
// start_unix_ms: 유닉스 밀리초
// 반환값: "YYYY-MM-DDTHH:MM:SS.sssZ"(UTC)
std::string FormatProgramDateTime(std::int64_t start_unix_ms) {
    const std::chrono::sys_time<std::chrono::milliseconds> start{std::chrono::milliseconds(start_unix_ms)};
    return std::format("{:%FT%T}Z", start);
}


// 재생 목록의 목표 길이(#EXT-X-TARGETDURATION)를 구한다.
//
// files: 재생 목록에 넣는 파일
// 반환값: 가장 긴 조각 길이를 초 단위로 올림한 값. 1보다 작으면 1
//
// 반올림한 #EXTINF 값이 아니라 원래 길이를 올림하므로, 각 #EXTINF를 가장 가까운 정수로 반올림한 값은 항상 이 값
// 이하다.
std::uint64_t TargetDurationSeconds(std::span<const recording::RecordedFile> files) {
    std::uint64_t longest = 0;
    for (const recording::RecordedFile& file : files) {
        for (const recording::RecordedFragment& fragment : file.fragments) {
            longest = std::max<std::uint64_t>(longest, fragment.duration);
        }
    }
    const std::uint64_t seconds = (longest + recording::kRecordingTimescale - 1) / recording::kRecordingTimescale;
    return std::max<std::uint64_t>(seconds, 1);
}


}  // namespace


std::string BuildRecordingFileUrl(std::string_view camera_folder, std::string_view stem) {
    return std::format("{}?camera={}&file={}", kRecordingFilePath, http::EncodeQueryComponent(camera_folder),
                       http::EncodeQueryComponent(stem));
}


std::string BuildHlsPlaylist(std::string_view camera_folder, std::span<const recording::RecordedFile> files) {
    std::string playlist;
    auto output = std::back_inserter(playlist);
    std::format_to(output,
                   "#EXTM3U\n"
                   "#EXT-X-VERSION:7\n"
                   "#EXT-X-PLAYLIST-TYPE:VOD\n"
                   "#EXT-X-INDEPENDENT-SEGMENTS\n"
                   "#EXT-X-TARGETDURATION:{}\n",
                   TargetDurationSeconds(files));

    // 파일마다 재생 시각이 0부터 다시 시작하고 초기화 구간도 따로 있으므로, 파일 경계마다 불연속 표시를 둔다.
    // 카메라가 꺼져 녹화가 없던 구간은 어느 파일에도 없으므로 재생 목록에 나타나지 않는다.
    for (std::size_t file_index = 0; file_index < files.size(); ++file_index) {
        const recording::RecordedFile& file = files[file_index];
        const std::string url = BuildRecordingFileUrl(camera_folder, file.stem);
        const std::uint64_t init_segment_size = file.fragments.empty() ? 0 : file.fragments.front().offset;
        if (file_index > 0) {
            playlist += "#EXT-X-DISCONTINUITY\n";
        }
        std::format_to(output, "#EXT-X-MAP:URI=\"{}\",BYTERANGE=\"{}@0\"\n", url, init_segment_size);
        std::format_to(output, "#EXT-X-PROGRAM-DATE-TIME:{}\n", FormatProgramDateTime(file.start_unix_ms));

        for (const recording::RecordedFragment& fragment : file.fragments) {
            std::format_to(output, "#EXTINF:{},\n#EXT-X-BYTERANGE:{}@{}\n{}\n", FormatSegmentDuration(fragment.duration),
                           fragment.size, fragment.offset, url);
        }
    }

    playlist += "#EXT-X-ENDLIST\n";
    return playlist;
}


}  // namespace vms::playback
