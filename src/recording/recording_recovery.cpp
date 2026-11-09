#include "recording/recording_recovery.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <format>
#include <map>
#include <print>
#include <span>
#include <system_error>
#include <utility>

#include "core/system_error.h"
#include "core/unique_fd.h"
#include "recording/recording_file_name.h"
#include "recording/recording_index.h"


namespace vms::recording {


namespace {


namespace fs = std::filesystem;


// 같은 이름을 가진 녹화 파일과 색인 파일의 존재 여부.
struct FilePair {
    // .mp4가 있음
    bool has_video = false;


    // .idx가 있음
    bool has_index = false;
};


// 파일 짝 하나를 검사한 결과.
enum class PairOutcome {
    // 고칠 것이 없음
    kKept,

    // 유효하지 않은 꼬리를 잘라 냄
    kTruncated,

    // 짝을 지움
    kDeleted,

    // 읽거나 고치지 못해 그대로 둠
    kFailed,
};


// 경로 앞부분에 확장자를 붙인다.
fs::path WithExtension(const fs::path& stem_path, std::string_view extension) {
    fs::path path = stem_path;
    path += extension;
    return path;
}


// 파일을 지운다. 이미 없으면 성공으로 본다.
//
// 반환값: 지웠거나 없었으면 true
bool RemoveFile(const fs::path& path) {
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        std::println(stderr, "{}", std::format("녹화 복구 중 삭제 실패 ({}): {}", path.string(),
                                               core::LastSystemError().message()));
        return false;
    }
    return true;
}


// 위치를 지정해 정확히 그 길이만큼 읽는다.
//
// fd: 읽을 파일
// buffer: 채울 버퍼
// offset: 읽기 시작 위치
// 반환값: 성공하면 빈 오류 코드. 파일 끝에 먼저 닿으면 io_error
std::error_code ReadFully(int fd, std::span<std::uint8_t> buffer, std::uint64_t offset) {
    std::size_t read_bytes = 0;
    while (read_bytes < buffer.size()) {
        const ssize_t result = ::pread(fd, buffer.data() + read_bytes, buffer.size() - read_bytes,
                                       static_cast<off_t>(offset + read_bytes));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return core::LastSystemError();
        }
        if (result == 0) {
            return std::make_error_code(std::errc::io_error);
        }
        read_bytes += static_cast<std::size_t>(result);
    }
    return {};
}


// 파일을 읽기·쓰기로 열고 크기를 구한다.
//
// path: 열 파일
// size: 파일 크기를 받을 변수
// 반환값: 연 파일. 실패하면 오류 코드
std::expected<core::UniqueFd, std::error_code> OpenForRepair(const fs::path& path, std::uint64_t& size) {
    core::UniqueFd fd(::open(path.c_str(), O_RDWR | O_CLOEXEC));
    if (!fd.IsValid()) {
        return std::unexpected(core::LastSystemError());
    }
    struct stat status {};
    if (::fstat(fd.Get(), &status) != 0) {
        return std::unexpected(core::LastSystemError());
    }
    size = static_cast<std::uint64_t>(status.st_size);
    return fd;
}


// 파일 짝 하나를 검사하고 고친다.
//
// stem_path: 확장자를 뺀 파일 짝 경로. 두 파일이 모두 있어야 한다.
// stem: 확장자를 뺀 파일 이름
// recovered: 남긴 경우 파일 정보를 받을 변수
// 반환값: 검사 결과
PairOutcome RecoverPair(const fs::path& stem_path, const std::string& stem, RecordedFile& recovered) {
    const fs::path video_path = WithExtension(stem_path, kRecordingVideoExtension);
    const fs::path index_path = WithExtension(stem_path, kRecordingIndexExtension);
    const auto fail = [&](std::string_view action, const std::error_code& error) {
        std::println(stderr, "{}", std::format("녹화 복구 실패, 그대로 둠 ({}, {}): {}", stem_path.string(), action,
                                               error.message()));
        return PairOutcome::kFailed;
    };
    const auto delete_pair = [&] {
        RemoveFile(video_path);
        RemoveFile(index_path);
        return PairOutcome::kDeleted;
    };

    std::uint64_t video_size = 0;
    std::uint64_t index_size = 0;
    auto video_fd = OpenForRepair(video_path, video_size);
    if (!video_fd) {
        return fail("녹화 파일 열기", video_fd.error());
    }
    auto index_fd = OpenForRepair(index_path, index_size);
    if (!index_fd) {
        return fail("색인 파일 열기", index_fd.error());
    }

    // 색인은 레코드당 40바이트라 작으므로 한 번에 읽는다.
    std::vector<std::uint8_t> index_bytes(index_size);
    if (const std::error_code error = ReadFully(index_fd->Get(), index_bytes, 0)) {
        return fail("색인 파일 읽기", error);
    }
    const auto header = DecodeRecordingIndexHeader(index_bytes);
    if (!header) {
        return delete_pair();
    }

    // 처음으로 유효하지 않은 레코드에서 멈추고, 그 앞까지만 믿는다.
    std::size_t valid_count = 0;
    std::uint64_t valid_video_end = 0;
    RecordingIndexRecord last_valid_record;
    std::vector<RecordedFragment> valid_fragments;
    std::vector<std::uint8_t> fragment;
    for (std::size_t offset = kRecordingIndexHeaderSize; offset + kRecordingIndexRecordSize <= index_bytes.size();
         offset += kRecordingIndexRecordSize) {
        const auto record = DecodeRecordingIndexRecord(std::span(index_bytes).subspan(offset));
        if (!record || record->offset > video_size || record->size > video_size - record->offset) {
            break;
        }
        fragment.resize(record->size);
        if (const std::error_code error = ReadFully(video_fd->Get(), fragment, record->offset)) {
            return fail("조각 읽기", error);
        }
        if (Crc32(fragment) != record->fragment_crc32) {
            break;
        }
        ++valid_count;
        valid_video_end = record->offset + record->size;
        last_valid_record = *record;
        valid_fragments.push_back(RecordedFragment{
            .start_time = record->start_time, .duration = record->duration, .offset = record->offset,
            .size = record->size});
    }
    if (valid_count == 0) {
        return delete_pair();
    }

    const std::uint64_t valid_index_end = kRecordingIndexHeaderSize + valid_count * kRecordingIndexRecordSize;
    const bool needs_truncation = video_size > valid_video_end || index_size > valid_index_end;
    if (needs_truncation) {
        if (::ftruncate(video_fd->Get(), static_cast<off_t>(valid_video_end)) != 0) {
            return fail("녹화 파일 자르기", core::LastSystemError());
        }
        if (::ftruncate(index_fd->Get(), static_cast<off_t>(valid_index_end)) != 0) {
            return fail("색인 파일 자르기", core::LastSystemError());
        }
        if (::fsync(video_fd->Get()) != 0 || ::fsync(index_fd->Get()) != 0) {
            return fail("동기화", core::LastSystemError());
        }
    }

    recovered = RecordedFile{.stem = stem,
                             .start_unix_ms = header->start_unix_ms,
                             .duration = last_valid_record.start_time + last_valid_record.duration,
                             .fragments = std::move(valid_fragments)};
    return needs_truncation ? PairOutcome::kTruncated : PairOutcome::kKept;
}


// 카메라 폴더 하나를 복구한다.
//
// folder: 카메라 폴더 경로
// files: 남긴 파일을 시작 시각 순서로 받을 목록
// 반환값: 요약. 폴더를 훑을 수 없으면 원인 메시지
std::expected<CameraRecoveryReport, std::string> RecoverCameraFolder(const fs::path& folder,
                                                                     std::vector<RecordedFile>& files) {
    CameraRecoveryReport report{.camera_folder = folder.filename().string()};

    // 이름 규칙에 맞는 일반 파일만 짝짓는다. 맞지 않는 항목은 사용자의 파일일 수 있으므로 건드리지 않는다.
    std::map<std::string, FilePair> pairs;
    std::error_code error;
    for (fs::directory_iterator entry(folder, error), end; !error && entry != end; entry.increment(error)) {
        const auto name = ParseRecordingFileName(entry->path().filename().string());
        if (!name || !entry->is_regular_file()) {
            continue;
        }
        FilePair& pair = pairs[name->stem];
        (name->kind == RecordingFileKind::kVideo ? pair.has_video : pair.has_index) = true;
    }
    if (error) {
        return std::unexpected(std::format("카메라 폴더를 훑을 수 없음 ({}): {}", folder.string(), error.message()));
    }

    for (const auto& [stem, pair] : pairs) {
        const fs::path stem_path = folder / stem;
        if (!pair.has_video || !pair.has_index) {
            const fs::path orphan =
                WithExtension(stem_path, pair.has_video ? kRecordingVideoExtension : kRecordingIndexExtension);
            if (RemoveFile(orphan)) {
                ++report.deleted_count;
            }
            continue;
        }

        RecordedFile recovered;
        switch (RecoverPair(stem_path, stem, recovered)) {
            case PairOutcome::kKept:
                ++report.kept_count;
                files.push_back(std::move(recovered));
                break;
            case PairOutcome::kTruncated:
                ++report.kept_count;
                ++report.truncated_count;
                files.push_back(std::move(recovered));
                break;
            case PairOutcome::kDeleted:
                report.deleted_count += 2;
                break;
            case PairOutcome::kFailed:
                ++report.failed_count;
                break;
        }
    }
    std::ranges::sort(files, {}, &RecordedFile::start_unix_ms);
    return report;
}


}  // namespace


std::expected<RecordingRecoveryResult, std::string> RecoverRecordings(const fs::path& directory) {
    RecordingRecoveryResult result;
    std::error_code error;
    std::vector<fs::path> folders;
    for (fs::directory_iterator entry(directory, error), end; !error && entry != end; entry.increment(error)) {
        // 녹화 디렉터리 바로 아래의 일반 파일은 카메라 폴더가 아니므로 무시한다.
        if (entry->is_directory()) {
            folders.push_back(entry->path());
        }
    }
    if (error) {
        return std::unexpected(std::format("녹화 디렉터리를 훑을 수 없음 ({}): {}", directory.string(), error.message()));
    }
    std::ranges::sort(folders);

    for (const fs::path& folder : folders) {
        std::vector<RecordedFile>& files = result.files[folder.filename().string()];
        auto report = RecoverCameraFolder(folder, files);
        if (!report) {
            return std::unexpected(report.error());
        }
        result.reports.push_back(*std::move(report));
    }
    return result;
}


}  // namespace vms::recording
