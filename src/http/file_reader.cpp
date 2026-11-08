#include "http/file_reader.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <expected>
#include <format>
#include <print>
#include <span>
#include <utility>
#include <vector>

#include "core/system_error.h"
#include "core/unique_fd.h"


namespace vms::http {


namespace {


// 원인을 표준 오류에 남기고 500 결과를 만든다.
//
// task: 실패한 작업
// operation: 실패한 동작. 예: "열기"
// message: 실패 원인
// 반환값: 500 오류 응답을 담은 결과
FileReadResult FailWithInternalError(const FileReadTask& task, std::string_view operation,
                                     std::string_view message) {
    std::println(stderr, "파일 {} 실패 ({}): {}", operation, task.path.string(), message);
    return FileReadResult{.socket_fd = task.socket_fd,
                          .connection_id = task.connection_id,
                          .status = HttpStatus::kInternalServerError,
                          .response = BuildErrorResponse(HttpStatus::kInternalServerError)};
}


// 파일에서 지정한 위치부터 지정한 바이트 수를 모두 읽는다.
//
// file: 읽을 파일
// range: 읽을 위치와 바이트 수
// 반환값: 읽은 바이트. 읽기 오류면 시스템 오류 코드, 다 읽기 전에 파일 끝에 닿으면 빈 오류 코드
std::expected<std::string, std::error_code> ReadRange(const core::UniqueFd& file, ByteRange range) {
    std::string content(range.length, '\0');
    std::uint64_t read_total = 0;
    while (read_total < range.length) {
        const ssize_t read_size =
            ::pread(file.Get(), content.data() + read_total, range.length - read_total,
                    static_cast<off_t>(range.first_position + read_total));
        if (read_size < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(core::LastSystemError());
        }
        if (read_size == 0) {
            return std::unexpected(std::error_code());
        }
        read_total += static_cast<std::uint64_t>(read_size);
    }
    return content;
}


}  // namespace


FileReadResult ReadFileForResponse(const FileReadTask& task) {
    FileReadResult result{.socket_fd = task.socket_fd,
                          .connection_id = task.connection_id,
                          .status = HttpStatus::kOk,
                          .response = {}};
    const auto finish_with_error = [&](HttpStatus status, std::span<const HttpHeader> extra_headers = {}) {
        result.status = status;
        result.response = BuildErrorResponse(status, extra_headers);
        return std::move(result);
    };

    // 경로 확인 뒤 파일이 지워졌으면 ENOENT, 중간 구성 요소가 디렉터리가 아니면 ENOTDIR이다.
    const core::UniqueFd file(::open(task.path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (!file.IsValid()) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return finish_with_error(HttpStatus::kNotFound);
        }
        return FailWithInternalError(task, "열기", core::LastSystemError().message());
    }

    struct stat file_status {};
    if (::fstat(file.Get(), &file_status) != 0) {
        return FailWithInternalError(task, "정보 읽기", core::LastSystemError().message());
    }
    if (!S_ISREG(file_status.st_mode)) {
        return finish_with_error(HttpStatus::kNotFound);
    }
    const auto file_size = static_cast<std::uint64_t>(file_status.st_size);

    ByteRange range{.first_position = 0, .length = file_size};
    HttpStatus status = HttpStatus::kOk;
    if (task.range) {
        const auto resolved = ResolveByteRange(*task.range, file_size);
        if (!resolved) {
            const std::string content_range = std::format("bytes */{}", file_size);
            const std::array headers = {HttpHeader{"Content-Range", content_range}};
            return finish_with_error(HttpStatus::kRangeNotSatisfiable, headers);
        }
        range = *resolved;
        status = HttpStatus::kPartialContent;
    }

    const auto content = ReadRange(file, range);
    if (!content) {
        // 빈 오류 코드는 fstat 뒤 파일이 줄어들어 예상보다 일찍 끝에 닿았다는 뜻이다.
        const std::string message = content.error() ? content.error().message() : "파일이 예상보다 일찍 끝남";
        return FailWithInternalError(task, "읽기", message);
    }

    const std::string content_range = std::format("bytes {}-{}/{}", range.first_position,
                                                  range.first_position + range.length - 1, file_size);
    std::vector<HttpHeader> headers = {HttpHeader{"Accept-Ranges", "bytes"}};
    if (status == HttpStatus::kPartialContent) {
        headers.push_back(HttpHeader{"Content-Range", content_range});
    }
    result.status = status;
    result.response = BuildResponse(status, task.content_type, *content, headers);
    return result;
}


FileReader::FileReader(core::EventLoop& loop, ResultHandler result_handler, ReadFunction read_function)
    : loop_(loop),
      result_handler_(std::move(result_handler)),
      read_function_(std::move(read_function)),
      thread_([this] { RunThread(); }) {}


FileReader::~FileReader() {
    {
        std::lock_guard lock(mutex_);
        is_stop_requested_ = true;
        // 서버가 끝나면 응답을 기다리던 연결도 닫히므로, 남은 작업을 실행해도 보낼 곳이 없다.
        tasks_.clear();
    }
    work_available_.notify_one();
    thread_.join();
}


void FileReader::Push(FileReadTask task) {
    {
        std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    work_available_.notify_one();
}


void FileReader::RunThread() {
    while (true) {
        FileReadTask task;
        {
            std::unique_lock lock(mutex_);
            work_available_.wait(lock, [this] { return is_stop_requested_ || !tasks_.empty(); });
            if (is_stop_requested_) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }

        FileReadResult result = read_function_(task);
        loop_.Post([handler = result_handler_, result = std::move(result)]() mutable {
            handler(std::move(result));
        });
    }
}


}  // namespace vms::http
