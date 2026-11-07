#include "recording/recording_file_store.h"

#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <format>
#include <limits>
#include <print>
#include <utility>

#include "core/system_error.h"
#include "recording/fmp4.h"
#include "recording/recording_file_name.h"
#include "recording/recording_index.h"


namespace vms::recording {


namespace {


// 같은 시작 시각의 이름에 붙여 볼 최대 번호. 이만큼 겹치면 파일을 열지 않는다.
constexpr int kMaxCollisionNumber = 999;


// 새로 만드는 녹화 파일의 권한
constexpr mode_t kRecordingFileMode = 0644;


// 녹화 스레드의 로그 한 줄을 남긴다. 루프 스레드 로그와 한 줄 안에서 섞이지 않도록 완성된 문자열을 한 번에 쓴다.
void Log(const std::string& message) { std::println(stderr, "{}", message); }


// 바이트를 지정한 위치에 끝까지 쓴다. 짧게 쓰이면 남은 바이트를 이어서 쓴다.
//
// fd: 쓸 파일
// bytes: 쓸 바이트
// offset: 파일 안 시작 위치
// 반환값: 성공하면 빈 오류 코드, 실패하면 pwrite의 오류
std::error_code WriteFully(int fd, std::span<const std::uint8_t> bytes, std::uint64_t offset) {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t result =
            ::pwrite(fd, bytes.data() + written, bytes.size() - written, static_cast<off_t>(offset + written));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return core::LastSystemError();
        }
        written += static_cast<std::size_t>(result);
    }
    return {};
}


// 파일을 새로 만든다. 이미 있으면 실패한다.
//
// path: 만들 파일 경로
// 반환값: 쓰기 전용으로 연 파일. 실패하면 errno(EEXIST 포함)
std::expected<core::UniqueFd, std::error_code> CreateNewFile(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, kRecordingFileMode);
    if (fd < 0) {
        return std::unexpected(core::LastSystemError());
    }
    return core::UniqueFd(fd);
}


// 파일을 지운다. 이미 없으면 성공으로 본다.
//
// path: 지울 파일
void RemoveFile(const std::filesystem::path& path) {
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        Log(std::format("녹화 파일 삭제 실패 ({}): {}", path.string(), core::LastSystemError().message()));
    }
}


// 경로 앞부분에 확장자를 붙인다.
std::filesystem::path WithExtension(const std::filesystem::path& stem_path, std::string_view extension) {
    std::filesystem::path path = stem_path;
    path += extension;
    return path;
}


}  // namespace


std::expected<std::uint64_t, std::error_code> QueryFileSystemFreeSpace(const std::filesystem::path& path) {
    struct statvfs stats {};
    if (::statvfs(path.c_str(), &stats) != 0) {
        return std::unexpected(core::LastSystemError());
    }
    return static_cast<std::uint64_t>(stats.f_bavail) * stats.f_frsize;
}


RecordingFileStore::RecordingFileStore(RecordingOptions options, RecordedFileCatalog closed_files)
    : options_(std::move(options)), closed_files_(std::move(closed_files)) {}


RecordingFileStore::~RecordingFileStore() { CloseAllFiles(); }


void RecordingFileStore::CloseAllFiles() {
    // FinishFile이 항목을 지우므로 키를 참조로 넘기지 않고 복사한다.
    while (!open_files_.empty()) {
        const std::string camera_name = open_files_.begin()->first;
        FinishFile(camera_name);
    }
}


void RecordingFileStore::Execute(const RecordingTask& task) {
    if (const auto* open = std::get_if<OpenFileTask>(&task.action)) {
        OpenFile(task.camera_name, *open);
    } else if (const auto* write = std::get_if<WriteFragmentTask>(&task.action)) {
        WriteFragment(task.camera_name, *write);
    } else {
        CloseFile(task.camera_name);
    }
}


void RecordingFileStore::OpenFile(const std::string& camera_name, const OpenFileTask& task) {
    if (open_files_.contains(camera_name)) {
        FinishFile(camera_name);
    }
    DeleteWhileLowOnSpace();

    const auto init_segment = BuildFmp4InitSegment(task.sps, task.pps);
    if (!init_segment) {
        Log(std::format("녹화 파일을 열 수 없음: {} ({})", camera_name, init_segment.error()));
        return;
    }

    // 한 번 닫은 파일은 다시 열지 않으므로, 같은 이름이 있으면 번호를 붙여 새로 만든다. .idx만 남아 있는 경우도
    // 같은 이름으로 보고 다음 번호로 넘어간다.
    OpenRecording recording;
    for (int collision_number = 0; collision_number <= kMaxCollisionNumber; ++collision_number) {
        const std::string stem = FormatRecordingFileStem(task.start_unix_ms, collision_number);
        const std::filesystem::path stem_path = StemPath(camera_name, stem);
        auto video_fd = CreateNewFile(WithExtension(stem_path, kRecordingVideoExtension));
        if (!video_fd) {
            if (video_fd.error() == std::errc::file_exists) {
                continue;
            }
            Log(std::format("녹화 파일 생성 실패 ({}): {}", stem_path.string(), video_fd.error().message()));
            return;
        }
        auto index_fd = CreateNewFile(WithExtension(stem_path, kRecordingIndexExtension));
        if (!index_fd) {
            RemoveFile(WithExtension(stem_path, kRecordingVideoExtension));
            if (index_fd.error() == std::errc::file_exists) {
                continue;
            }
            Log(std::format("색인 파일 생성 실패 ({}): {}", stem_path.string(), index_fd.error().message()));
            return;
        }
        recording.stem = stem;
        recording.video_fd = *std::move(video_fd);
        recording.index_fd = *std::move(index_fd);
        break;
    }
    if (!recording.video_fd.IsValid()) {
        Log(std::format("녹화 파일 이름이 {}번까지 모두 겹침: {}/{}", kMaxCollisionNumber, camera_name,
                        FormatRecordingFileStem(task.start_unix_ms, 0)));
        return;
    }

    const auto header = EncodeRecordingIndexHeader(RecordingIndexHeader{.start_unix_ms = task.start_unix_ms});
    std::error_code error = WriteWithSpaceRetry(recording.video_fd.Get(), *init_segment, 0);
    if (!error) {
        error = WriteWithSpaceRetry(recording.index_fd.Get(), header, 0);
    }
    if (error) {
        Log(std::format("녹화 파일 시작 부분 쓰기 실패 ({}/{}): {}", camera_name, recording.stem, error.message()));
        const std::filesystem::path stem_path = StemPath(camera_name, recording.stem);
        RemoveFile(WithExtension(stem_path, kRecordingVideoExtension));
        RemoveFile(WithExtension(stem_path, kRecordingIndexExtension));
        return;
    }

    recording.start_unix_ms = task.start_unix_ms;
    recording.video_size = init_segment->size();
    recording.index_size = header.size();
    recording.has_unsynced_data = true;
    open_files_.emplace(camera_name, std::move(recording));
}


void RecordingFileStore::WriteFragment(const std::string& camera_name, const WriteFragmentTask& task) {
    // 열기나 앞 조각 쓰기에 실패한 카메라는 다음 열기 작업까지 조각을 버린다.
    const auto found = open_files_.find(camera_name);
    if (found == open_files_.end() || task.frames.empty()) {
        return;
    }
    OpenRecording& recording = found->second;

    const std::vector<std::uint8_t> fragment = BuildFmp4Fragment(task.frames, recording.next_sequence_number);
    std::uint64_t duration = 0;
    for (const Fmp4Frame& frame : task.frames) {
        duration += frame.duration;
    }
    const RecordingIndexRecord record{
        .start_time = task.frames.front().decode_time,
        .duration = static_cast<std::uint32_t>(duration),
        .offset = recording.video_size,
        .size = static_cast<std::uint32_t>(fragment.size()),
        .fragment_crc32 = Crc32(fragment),
    };

    // 조각을 끝까지 쓴 뒤에 레코드를 쓴다. 그래서 프로세스만 죽은 경우 레코드가 있으면 조각도 있다.
    std::error_code error = WriteWithSpaceRetry(recording.video_fd.Get(), fragment, recording.video_size);
    if (!error) {
        error = WriteWithSpaceRetry(recording.index_fd.Get(), EncodeRecordingIndexRecord(record), recording.index_size);
    }
    if (error) {
        Log(std::format("녹화 쓰기 실패, 파일을 닫음 ({}/{}): {}", camera_name, recording.stem, error.message()));
        FinishFile(camera_name);
        return;
    }

    recording.video_size += fragment.size();
    recording.index_size += kRecordingIndexRecordSize;
    recording.end_time = record.start_time + record.duration;
    ++recording.next_sequence_number;
    ++recording.fragment_count;
    recording.has_unsynced_data = true;
}


void RecordingFileStore::CloseFile(const std::string& camera_name) {
    if (open_files_.contains(camera_name)) {
        FinishFile(camera_name);
    }
}


void RecordingFileStore::FinishFile(const std::string& camera_name) {
    const auto found = open_files_.find(camera_name);
    OpenRecording recording = std::move(found->second);
    open_files_.erase(found);
    const std::filesystem::path stem_path = StemPath(camera_name, recording.stem);

    if (recording.fragment_count == 0) {
        recording.video_fd.Reset();
        recording.index_fd.Reset();
        RemoveFile(WithExtension(stem_path, kRecordingVideoExtension));
        RemoveFile(WithExtension(stem_path, kRecordingIndexExtension));
        return;
    }

    // 쓰기 도중 실패했다면 마지막으로 완전히 쓴 조각·레코드 뒤에 반쯤 쓴 바이트가 있을 수 있어 잘라 낸다.
    // 정상적으로 닫는 경우에는 이미 그 길이이므로 바뀌지 않는다.
    if (::ftruncate(recording.video_fd.Get(), static_cast<off_t>(recording.video_size)) != 0 ||
        ::ftruncate(recording.index_fd.Get(), static_cast<off_t>(recording.index_size)) != 0) {
        Log(std::format("녹화 파일 자르기 실패 ({}): {}", stem_path.string(), core::LastSystemError().message()));
    }
    if (::fdatasync(recording.video_fd.Get()) != 0 || ::fdatasync(recording.index_fd.Get()) != 0) {
        Log(std::format("녹화 파일 동기화 실패 ({}): {}", stem_path.string(), core::LastSystemError().message()));
    }
    recording.video_fd.Reset();
    recording.index_fd.Reset();

    std::vector<RecordedFile>& files = closed_files_[camera_name];
    const RecordedFile closed{.stem = recording.stem, .start_unix_ms = recording.start_unix_ms,
                              .duration = recording.end_time};
    files.insert(std::ranges::upper_bound(files, closed.start_unix_ms, {}, &RecordedFile::start_unix_ms), closed);
    ApplyRetention(camera_name);
}


void RecordingFileStore::SyncOpenFiles() {
    for (auto& [camera_name, recording] : open_files_) {
        if (!recording.has_unsynced_data) {
            continue;
        }
        // 녹화 파일을 먼저 내려야, 디스크에 있는 레코드가 가리키는 조각도 디스크에 있다.
        if (::fdatasync(recording.video_fd.Get()) != 0 || ::fdatasync(recording.index_fd.Get()) != 0) {
            Log(std::format("녹화 파일 동기화 실패 ({}/{}): {}", camera_name, recording.stem,
                            core::LastSystemError().message()));
        }
        recording.has_unsynced_data = false;
    }
}


void RecordingFileStore::ApplyRetention(const std::string& camera_name) {
    std::vector<RecordedFile>& files = closed_files_[camera_name];
    const std::uint64_t retention = static_cast<std::uint64_t>(options_.retention_seconds) * kRecordingTimescale;
    std::uint64_t total_duration = 0;
    for (const RecordedFile& file : files) {
        total_duration += file.duration;
    }

    // 끊긴 시간은 어느 파일에도 들지 않으므로 파일 길이의 합만 센다.
    while (total_duration > retention && !files.empty()) {
        const RecordedFile oldest = files.front();
        files.erase(files.begin());
        total_duration -= oldest.duration;
        const std::filesystem::path stem_path = StemPath(camera_name, oldest.stem);
        RemoveFile(WithExtension(stem_path, kRecordingVideoExtension));
        RemoveFile(WithExtension(stem_path, kRecordingIndexExtension));
        Log(std::format("보관 분량 초과로 삭제: {}/{}", camera_name, oldest.stem));
    }
}


void RecordingFileStore::DeleteWhileLowOnSpace() {
    while (true) {
        const auto free_bytes = QueryFreeSpace();
        if (!free_bytes) {
            Log(std::format("여유 공간 조회 실패 ({}): {}", options_.directory.string(), free_bytes.error().message()));
            return;
        }
        if (*free_bytes >= options_.min_free_bytes) {
            return;
        }
        if (!DeleteOldestSegment()) {
            // 열린 파일은 지우지 않으므로 그대로 연다. 그 뒤 공간 부족은 쓰기 실패 경로가 처리한다.
            Log(std::format("여유 공간 부족({}바이트)이지만 지울 닫힌 녹화 파일이 없음", *free_bytes));
            return;
        }
    }
}


bool RecordingFileStore::DeleteOldestSegment() {
    std::int64_t oldest_start = std::numeric_limits<std::int64_t>::max();
    for (const auto& [camera_name, files] : closed_files_) {
        if (!files.empty()) {
            oldest_start = std::min(oldest_start, files.front().start_unix_ms);
        }
    }
    if (oldest_start == std::numeric_limits<std::int64_t>::max()) {
        return false;
    }

    // 모든 카메라가 같은 단위 구간 경계를 쓰므로, 가장 오래된 파일의 구간을 카메라와 관계없이 함께 지운다.
    const std::int64_t segment_number = SegmentNumberOf(oldest_start, options_.segment_seconds);
    std::size_t deleted_count = 0;
    for (auto& [camera_name, files] : closed_files_) {
        const auto removed = std::ranges::remove_if(files, [&](const RecordedFile& file) {
            if (SegmentNumberOf(file.start_unix_ms, options_.segment_seconds) != segment_number) {
                return false;
            }
            const std::filesystem::path stem_path = StemPath(camera_name, file.stem);
            RemoveFile(WithExtension(stem_path, kRecordingVideoExtension));
            RemoveFile(WithExtension(stem_path, kRecordingIndexExtension));
            ++deleted_count;
            return true;
        });
        files.erase(removed.begin(), removed.end());
    }
    const std::int64_t segment_start_ms = segment_number * options_.segment_seconds * 1000;
    Log(std::format("비상 삭제: 단위 구간 {}, 파일 {}개", FormatRecordingFileStem(segment_start_ms, 0), deleted_count));
    return true;
}


std::error_code RecordingFileStore::WriteWithSpaceRetry(int fd, std::span<const std::uint8_t> bytes,
                                                        std::uint64_t offset) {
    const std::error_code error = WriteFully(fd, bytes, offset);
    if (error != std::errc::no_space_on_device) {
        return error;
    }

    // 여유 공간 조회 결과와 관계없이 가장 오래된 구간을 한 번은 지워 공간을 만든 뒤 다시 쓴다.
    Log("디스크 공간 부족으로 쓰기 실패, 비상 삭제 후 다시 씀");
    if (DeleteOldestSegment()) {
        DeleteWhileLowOnSpace();
    }
    return WriteFully(fd, bytes, offset);
}


std::expected<std::uint64_t, std::error_code> RecordingFileStore::QueryFreeSpace() const {
    if (options_.query_free_space) {
        return options_.query_free_space(options_.directory);
    }
    return QueryFileSystemFreeSpace(options_.directory);
}


std::filesystem::path RecordingFileStore::StemPath(const std::string& camera_name, const std::string& stem) const {
    return options_.directory / camera_name / stem;
}


}  // namespace vms::recording
