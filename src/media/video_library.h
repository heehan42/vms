#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "media/h264_file.h"


namespace vms::media {


// 동영상 목록에 읽어 들이는 파일 크기 합계의 기본 상한(바이트). 시작할 때 모든 파일을 메모리에 읽으므로,
// 큰 디렉터리를 잘못 지정했을 때 메모리를 지나치게 쓰는 것을 막는다.
inline constexpr std::uintmax_t kMaxVideoLibraryBytes = 256 * 1024 * 1024;


// 동영상 목록을 만들 때 적용하는 한도.
struct VideoLibraryLimits {
    // 목록에 넣은 파일 크기의 합계 상한. 넣으면 이 값을 넘게 되는 파일은 건너뛴다.
    // 테스트가 작은 값으로 합계 상한을 재현하기 위해 따로 둔다.
    std::uintmax_t max_total_bytes = kMaxVideoLibraryBytes;
};


// 동영상 목록의 항목 하나.
struct VideoEntry {
    // 동영상 ID. 디렉터리 안의 파일 이름이며 유효한 UTF-8이다.
    std::string id;


    // 읽어 들인 파일. 프레임 뷰는 이 항목이 살아 있는 동안 유효하다.
    H264File file;
};


struct VideoLibraryLoadResult;


// 서버가 재생할 수 있는 동영상 목록.
//
// 시작할 때 동영상 디렉터리의 파일을 모두 읽어 항목으로 소유하며, 이후 바꾸지 않는다. 그래서 Find()와
// Entries()가 돌려준 항목 주소와 그 파일의 프레임 뷰는 이 객체가 살아 있는 동안 유효하다. std::vector는
// 이동해도 버퍼 주소가 바뀌지 않으므로 이동한 뒤에도 유효하다. 세션과 송신 상태가 항목을 참조하므로,
// 이 객체는 그들보다 오래 살아 있어야 한다.
class VideoLibrary {
public:
    // 동영상 디렉터리를 읽어 목록을 만든다.
    //
    // directory: 동영상 디렉터리
    // limits: 파일 크기 합계 상한
    // 반환값: 목록과 건너뛴 파일의 경고. 다음 경우 원인 메시지
    //   - directory가 디렉터리가 아니거나 열거나 순회할 수 없음
    //   - 건너뛴 뒤 목록이 비어 있음
    //
    // directory 바로 아래 항목 중 확장자가 대소문자 구분 없이 .h264 또는 .264인 것을 이름의 바이트 순서로
    // 검사한다. 디렉터리(심볼릭 링크는 대상 기준)는 조용히 무시하고, 이름이 UTF-8이 아니거나 크기를
    // 알 수 없거나 합계 상한을 넘거나 H264File::Load가 실패한 파일은 경고를 남기고 건너뛴다.
    // 로그는 남기지 않으며, 경고 출력은 호출자가 한다.
    static std::expected<VideoLibraryLoadResult, std::string> LoadDirectory(const std::filesystem::path& directory,
                                                                            const VideoLibraryLimits& limits = {});


    // 동영상이 없는 빈 목록을 만든다. 동영상 디렉터리 없이 실행할 때 쓴다.
    VideoLibrary() = default;


    // ID가 같은 항목을 찾는다.
    //
    // id: 찾을 동영상 ID. 바이트 단위로 비교한다.
    // 반환값: 찾은 항목. 없으면 nullptr. 이 객체가 살아 있는 동안 유효하다.
    const VideoEntry* Find(std::string_view id) const;


    // 목록 순서대로의 항목을 돌려준다.
    //
    // 반환값: 이름의 바이트 순서로 정렬된 항목. 이 객체가 살아 있는 동안 유효하다.
    const std::vector<VideoEntry>& Entries() const { return entries_; }


private:
    // 읽어 들인 항목으로 목록을 만든다. LoadDirectory()에서만 호출한다.
    explicit VideoLibrary(std::vector<VideoEntry> entries) : entries_(std::move(entries)) {}


    // 이름의 바이트 순서로 정렬된 항목
    std::vector<VideoEntry> entries_;
};


// VideoLibrary::LoadDirectory의 성공 결과.
struct VideoLibraryLoadResult {
    // 하나 이상의 동영상이 있는 목록
    VideoLibrary library;


    // 건너뛴 파일마다 경로와 원인을 담은 경고 문구. 건너뛴 순서대로다.
    std::vector<std::string> warnings;
};


}  // namespace vms::media
