#include "http/file_reader.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <latch>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/event_loop.h"
#include "http/byte_range.h"
#include "test_support.h"

using vms::core::EventLoop;
using vms::http::FileReader;
using vms::http::FileReadResult;
using vms::http::FileReadTask;
using vms::http::HttpStatus;
using vms::http::ParseRangeHeader;
using vms::http::ReadFileForResponse;
using vms::test::Expect;

namespace fs = std::filesystem;
using namespace std::chrono_literals;


namespace {


// 응답 바이트에 헤더 줄 하나가 들어 있는지 확인한다.
//
// response: 응답 바이트
// header_line: CRLF를 뺀 헤더 줄. 예: "Accept-Ranges: bytes"
// 반환값: 헤더 영역에 그 줄이 있으면 true
bool HasHeader(std::string_view response, std::string_view header_line) {
    const std::string_view header = response.substr(0, response.find("\r\n\r\n") + 2);
    return header.contains(std::string("\r\n") + std::string(header_line) + "\r\n");
}


// 응답 본문을 돌려준다.
//
// response: 응답 바이트
// 반환값: 빈 줄 뒤의 바이트. 빈 줄이 없으면 빈 문자열
std::string_view Body(std::string_view response) {
    const std::size_t header_end = response.find("\r\n\r\n");
    return header_end == std::string_view::npos ? std::string_view() : response.substr(header_end + 4);
}


// 경로와 Range 헤더 값으로 파일 읽기 작업을 만든다.
//
// path: 읽을 파일
// range: Range 헤더 값. 빈 문자열이면 범위 없음
// 반환값: Content-Type이 application/octet-stream인 작업
FileReadTask MakeTask(const fs::path& path, std::string_view range = {}) {
    return FileReadTask{.socket_fd = 7,
                        .connection_id = 3,
                        .path = path,
                        .content_type = "application/octet-stream",
                        .range = range.empty() ? std::nullopt : ParseRangeHeader(range)};
}


// 범위 없는 요청에 파일 전체를 200으로 보내는지 검사한다.
//
// content: 크기 100인 테스트 파일의 내용
// path: 테스트 파일 경로
void TestReadsWholeFile(const std::string& content, const fs::path& path) {
    const FileReadResult result = ReadFileForResponse(MakeTask(path));
    Expect(result.socket_fd == 7 && result.connection_id == 3, "작업의 연결 식별 정보를 결과에 옮긴다");
    Expect(result.status == HttpStatus::kOk, "범위 없음은 200");
    Expect(result.response.starts_with("HTTP/1.1 200 OK\r\n"), "200 상태 줄");
    Expect(HasHeader(result.response, "Accept-Ranges: bytes"), "200의 Accept-Ranges");
    Expect(HasHeader(result.response, "Content-Type: application/octet-stream"), "작업의 Content-Type");
    Expect(!result.response.contains("Content-Range"), "200에는 Content-Range가 없다");
    Expect(Body(result.response) == content, "본문은 파일 전체");
}


// 범위 요청에 206과 Content-Range로 그 범위만 보내는지 검사한다.
//
// content: 크기 100인 테스트 파일의 내용
// path: 테스트 파일 경로
void TestReadsRange(const std::string& content, const fs::path& path) {
    const FileReadResult first_ten = ReadFileForResponse(MakeTask(path, "bytes=0-9"));
    Expect(first_ten.status == HttpStatus::kPartialContent, "bytes=0-9는 206");
    Expect(first_ten.response.starts_with("HTTP/1.1 206 Partial Content\r\n"), "206 상태 줄");
    Expect(HasHeader(first_ten.response, "Content-Range: bytes 0-9/100"), "206의 Content-Range");
    Expect(HasHeader(first_ten.response, "Content-Length: 10"), "206의 Content-Length");
    Expect(HasHeader(first_ten.response, "Accept-Ranges: bytes"), "206의 Accept-Ranges");
    Expect(Body(first_ten.response) == content.substr(0, 10), "본문은 처음 10바이트");

    const FileReadResult suffix = ReadFileForResponse(MakeTask(path, "bytes=-20"));
    Expect(HasHeader(suffix.response, "Content-Range: bytes 80-99/100"), "bytes=-20의 Content-Range");
    Expect(Body(suffix.response) == content.substr(80), "본문은 마지막 20바이트");
}


// 파일 밖의 범위에 416과 Content-Range: bytes */크기로 응답하는지 검사한다.
//
// path: 크기 100인 테스트 파일 경로
void TestRejectsUnsatisfiableRange(const fs::path& path) {
    const FileReadResult result = ReadFileForResponse(MakeTask(path, "bytes=100-"));
    Expect(result.status == HttpStatus::kRangeNotSatisfiable, "bytes=100-는 416");
    Expect(result.response.starts_with("HTTP/1.1 416 Range Not Satisfiable\r\n"), "416 상태 줄");
    Expect(HasHeader(result.response, "Content-Range: bytes */100"), "416의 Content-Range");
}


// 없는 경로, 디렉터리, FIFO에 404로 응답하고, FIFO를 열 때 멈추지 않는지 검사한다.
//
// web_root: 테스트용 임시 디렉터리
void TestRejectsNonRegularFiles(const fs::path& web_root) {
    Expect(ReadFileForResponse(MakeTask(web_root / "missing.bin")).status == HttpStatus::kNotFound,
           "없는 파일은 404");
    Expect(ReadFileForResponse(MakeTask(web_root / "missing" / "a.bin")).status == HttpStatus::kNotFound,
           "없는 디렉터리 안의 파일은 404");
    Expect(ReadFileForResponse(MakeTask(web_root)).status == HttpStatus::kNotFound, "디렉터리는 404");

    // 쓰는 쪽이 없는 FIFO를 O_NONBLOCK 없이 열면 이 호출이 반환되지 않아 테스트가 끝나지 않는다.
    const fs::path fifo_path = web_root / "pipe";
    if (::mkfifo(fifo_path.c_str(), 0600) != 0) {
        Expect(false, "FIFO 생성");
        return;
    }
    Expect(ReadFileForResponse(MakeTask(fifo_path)).status == HttpStatus::kNotFound, "FIFO는 404");
}


// 읽기 권한이 없는 파일에 500으로 응답하는지 검사한다. root는 권한과 관계없이 읽으므로 건너뛴다.
//
// web_root: 테스트용 임시 디렉터리
void TestRejectsUnreadableFile(const fs::path& web_root) {
    if (::geteuid() == 0) {
        return;
    }
    const fs::path path = web_root / "secret.bin";
    std::ofstream(path) << "secret";
    std::error_code error;
    fs::permissions(path, fs::perms::none, error);
    Expect(ReadFileForResponse(MakeTask(path)).status == HttpStatus::kInternalServerError,
           "읽기 권한 없는 파일은 500");
}


// 작업 번호만 결과에 옮기는 실행 함수. 파일 시스템에 접근하지 않는다.
//
// task: 실행할 작업
// 반환값: 작업의 연결 번호를 담은 200 결과
FileReadResult EchoConnectionId(const FileReadTask& task) {
    return FileReadResult{
        .socket_fd = task.socket_fd, .connection_id = task.connection_id, .status = HttpStatus::kOk, .response = {}};
}


// 넣은 작업의 결과가 루프 스레드에서 넣은 순서대로 전달되는지 검사한다.
void TestDeliversResultsOnLoopThreadInOrder() {
    auto loop = EventLoop::Create();
    if (!loop) {
        Expect(false, "이벤트 루프 생성");
        return;
    }

    constexpr std::uint64_t kTaskCount = 20;
    std::vector<std::uint64_t> delivered_ids;
    bool is_on_loop_thread = true;
    const std::thread::id loop_thread_id = std::this_thread::get_id();
    {
        FileReader reader(
            *loop,
            [&](FileReadResult result) {
                is_on_loop_thread = is_on_loop_thread && std::this_thread::get_id() == loop_thread_id;
                delivered_ids.push_back(result.connection_id);
                if (delivered_ids.size() == kTaskCount) {
                    loop->Stop();
                }
            },
            EchoConnectionId);
        for (std::uint64_t id = 1; id <= kTaskCount; ++id) {
            reader.Push(FileReadTask{.socket_fd = 1, .connection_id = id, .path = {}, .content_type = {}, .range = {}});
        }
        const EventLoop::TimerId timeout = loop->AddTimer(2000ms, [&] { loop->Stop(); });
        loop->Run();
        loop->CancelTimer(timeout);
    }

    bool is_in_order = delivered_ids.size() == kTaskCount;
    for (std::uint64_t index = 0; is_in_order && index < kTaskCount; ++index) {
        is_in_order = delivered_ids[index] == index + 1;
    }
    Expect(is_in_order, "결과는 넣은 순서대로 모두 전달되어야 한다");
    Expect(is_on_loop_thread, "결과는 루프 스레드에서 전달되어야 한다");
}


// 실행 함수가 멈춰 있는 동안에도 루프의 타이머가 제때 실행되는지 검사한다.
void TestLoopRunsWhileReadIsBlocked() {
    auto loop = EventLoop::Create();
    if (!loop) {
        Expect(false, "이벤트 루프 생성");
        return;
    }

    // 타이머가 실행 함수를 풀어 주므로, 타이머가 늦으면 결과도 늦게 도착한다.
    std::latch read_release(1);
    std::optional<std::chrono::steady_clock::duration> timer_delay;
    bool is_result_delivered = false;
    {
        FileReader reader(
            *loop,
            [&](FileReadResult) {
                is_result_delivered = true;
                loop->Stop();
            },
            [&](const FileReadTask& task) {
                read_release.wait();
                return EchoConnectionId(task);
            });
        reader.Push(FileReadTask{.socket_fd = 1, .connection_id = 1, .path = {}, .content_type = {}, .range = {}});

        const auto started_at = std::chrono::steady_clock::now();
        loop->AddTimer(20ms, [&] {
            timer_delay = std::chrono::steady_clock::now() - started_at;
            read_release.count_down();
        });
        const EventLoop::TimerId timeout = loop->AddTimer(2000ms, [&] { loop->Stop(); });
        loop->Run();
        loop->CancelTimer(timeout);
    }

    Expect(timer_delay && *timer_delay < 500ms, "실행 함수가 멈춰 있어도 타이머는 제때 실행되어야 한다");
    Expect(is_result_delivered, "풀어 준 뒤 결과가 전달되어야 한다");
}


// 실행 중인 작업이 있을 때 소멸하면 그 작업이 끝난 뒤 반환되고, 큐에 남은 작업은 실행되지 않는지 검사한다.
void TestDestructorWaitsForRunningTaskAndDropsQueued() {
    auto loop = EventLoop::Create();
    if (!loop) {
        Expect(false, "이벤트 루프 생성");
        return;
    }

    std::latch read_started(1);
    std::latch read_release(1);
    std::atomic<int> read_count = 0;
    std::atomic<bool> is_read_finished = false;
    std::thread releasing_thread;
    {
        FileReader reader(
            *loop, [](FileReadResult) {},
            [&](const FileReadTask& task) {
                ++read_count;
                read_started.count_down();
                read_release.wait();
                is_read_finished = true;
                return EchoConnectionId(task);
            });
        reader.Push(FileReadTask{.socket_fd = 1, .connection_id = 1, .path = {}, .content_type = {}, .range = {}});
        reader.Push(FileReadTask{.socket_fd = 1, .connection_id = 2, .path = {}, .content_type = {}, .range = {}});
        read_started.wait();

        // 소멸자가 실행 중인 작업을 기다리는 동안 다른 스레드가 풀어 준다.
        releasing_thread = std::thread([&] {
            std::this_thread::sleep_for(30ms);
            read_release.count_down();
        });
    }

    Expect(is_read_finished, "소멸자는 실행 중인 작업이 끝난 뒤 반환되어야 한다");
    Expect(read_count == 1, "큐에 남은 작업은 실행되지 않아야 한다");
    releasing_thread.join();
}


}  // namespace


int main() {
    std::error_code error;
    std::string root_template = (fs::temp_directory_path(error) / "file_reader_test.XXXXXX").string();
    if (error || ::mkdtemp(root_template.data()) == nullptr) {
        Expect(false, "임시 디렉터리 생성");
        return vms::test::ExitCode();
    }
    const fs::path web_root = root_template;

    std::string content;
    for (int index = 0; index < 100; ++index) {
        content.push_back(static_cast<char>('A' + index % 26));
    }
    const fs::path file_path = web_root / "video.bin";
    std::ofstream(file_path, std::ios::binary) << content;

    TestReadsWholeFile(content, file_path);
    TestReadsRange(content, file_path);
    TestRejectsUnsatisfiableRange(file_path);
    TestRejectsNonRegularFiles(web_root);
    TestRejectsUnreadableFile(web_root);
    TestDeliversResultsOnLoopThreadInOrder();
    TestLoopRunsWhileReadIsBlocked();
    TestDestructorWaitsForRunningTaskAndDropsQueued();

    fs::remove_all(web_root, error);
    return vms::test::ExitCode();
}
