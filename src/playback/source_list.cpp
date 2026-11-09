#include "playback/source_list.h"

#include <format>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include "core/json.h"


namespace vms::playback {


http::HttpEndpointResponse BuildSourceListResponse(const media::VideoLibrary& video_library,
                                                   const media::CameraList& camera_list,
                                                   const RecordingLibrary& recording_library) {
    std::string body = R"({"sources":[)";
    bool is_first = true;
    const auto append_separator = [&] {
        if (!is_first) {
            body.push_back(',');
        }
        is_first = false;
    };
    for (const media::VideoEntry& entry : video_library.Entries()) {
        append_separator();
        body.append(R"({"type":"file","name":)");
        core::AppendJsonString(body, entry.name);
        std::format_to(std::back_inserter(body), R"(,"frames":{}}})", entry.file.Frames().size());
    }
    // 카메라는 연결 상태와 관계없이 등록한 순서대로 모두 넣는다.
    for (const std::unique_ptr<media::CameraEntry>& entry : camera_list.Entries()) {
        append_separator();
        body.append(R"({"type":"camera","name":)");
        core::AppendJsonString(body, entry->name);
        body.push_back('}');
    }
    // 녹화 목록은 닫힌 파일이 있는 폴더만 담고, 폴더 이름의 바이트 순서로 정렬되어 있다. 이름은 만들 때 UTF-8인지
    // 확인했으므로 JSON 문자열로 그대로 넣을 수 있다.
    for (const auto& [camera_folder, files] : recording_library.Folders()) {
        append_separator();
        body.append(R"({"type":"recording","name":)");
        core::AppendJsonString(body, camera_folder);
        body.push_back('}');
    }
    body.append("]}");
    return http::HttpEndpointResponse{
        .status = http::HttpStatus::kOk, .content_type = "application/json", .body = std::move(body)};
}


}  // namespace vms::playback
