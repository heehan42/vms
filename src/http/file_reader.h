#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "core/event_loop.h"
#include "http/byte_range.h"
#include "http/http_response.h"


namespace vms::http {


// 파일 응답 하나를 만드는 데 필요한 정보. 루프 스레드가 만들어 파일 읽기 스레드에 넘긴다.
struct FileReadTask {
    // 결과를 돌려보낼 연결의 소켓 fd
    int socket_fd = -1;


    // 결과를 돌려보낼 연결의 번호. fd 번호는 연결이 닫힌 뒤 새 연결에 다시 쓰일 수 있으므로,
    // 결과를 받은 쪽은 fd와 이 번호가 모두 같은 연결에만 응답한다.
    std::uint64_t connection_id = 0;


    // 보낼 파일. 경로 안전성은 작업을 만든 쪽이 확인한 값이다.
    std::filesystem::path path;


    // 응답의 Content-Type
    std::string content_type;


    // 요청에 적용할 범위. 빈 optional이면 파일 전체를 200으로 보낸다.
    std::optional<ByteRangeSpec> range;
};


// 파일 읽기 작업 하나를 실행해 만든 결과. 파일 읽기 스레드가 만들어 루프 스레드에 넘긴다.
struct FileReadResult {
    // 결과를 돌려보낼 연결의 소켓 fd. FileReadTask::socket_fd와 같다.
    int socket_fd = -1;


    // 결과를 돌려보낼 연결의 번호. FileReadTask::connection_id와 같다.
    std::uint64_t connection_id = 0;


    // 응답 상태 코드. 요청 로그에 쓴다.
    HttpStatus status = HttpStatus::kOk;


    // 소켓에 그대로 보낼 응답 바이트(상태 줄, 헤더, 본문)
    std::string response;
};


// 파일을 열고 읽어 파일 읽기 작업 하나의 응답 바이트를 만든다.
//
// task: 실행할 파일 읽기 작업
// 반환값: 상태 코드와 응답 바이트를 담은 결과
//   - 200: 범위 없이 파일 전체. Accept-Ranges: bytes를 넣는다.
//   - 206: 범위의 바이트만. Accept-Ranges: bytes와 Content-Range: bytes 시작-끝/크기를 넣는다.
//   - 404: 파일이 없거나(ENOENT, ENOTDIR) 일반 파일이 아님
//   - 416: 범위가 파일 안에 한 바이트도 없음. Content-Range: bytes */크기를 넣는다.
//   - 500: 그 밖의 열기 실패(EACCES 등), fstat·pread 실패, 파일이 fstat 뒤 줄어듦. 원인을 표준 오류에 남긴다.
//
// 디스크를 기다릴 수 있으므로 파일 읽기 스레드에서만 호출한다. 소켓과 이벤트 루프에는 접근하지 않는다.
// FIFO를 만나도 멈추지 않도록 O_NONBLOCK으로 연다. 응답할 바이트 범위 전체를 메모리로 읽는다.
FileReadResult ReadFileForResponse(const FileReadTask& task);


// 파일 읽기 작업을 들어온 순서대로 하나씩 실행하는 파일 읽기 스레드와 그 작업 큐.
//
// 루프 스레드는 Push()로 작업을 넣고 바로 돌아간다. 파일 읽기 스레드는 실행 함수로 결과를 만들고,
// EventLoop::Post로 결과 처리 함수를 루프 스레드에서 호출한다.
// 생성자에서 스레드를 시작하고 소멸자에서 join하므로, 연결된 EventLoop보다 먼저 소멸해야 한다.
class FileReader {
public:
    // 파일 읽기 결과를 받는 함수. 루프 스레드에서 호출된다.
    //
    // 파일 읽기 스레드가 결과마다 이 함수를 복사해 루프 작업에 담으므로 복사할 수 있어야 하고,
    // 복사가 여러 스레드에서 일어나도 안전해야 한다. 복사본은 FileReader가 소멸한 뒤에 호출될 수도
    // 있으므로, 함수가 가리키는 객체가 아직 살아 있는지는 함수 안에서 확인해야 한다.
    using ResultHandler = std::function<void(FileReadResult result)>;


    // 파일 읽기 작업 하나로 결과를 만드는 함수. 파일 읽기 스레드에서 호출된다.
    //
    // 기본값은 ReadFileForResponse다. 테스트는 읽기를 원하는 시점까지 멈춰 두는 함수를 넘긴다.
    using ReadFunction = std::move_only_function<FileReadResult(const FileReadTask& task)>;


    // 파일 읽기 스레드를 시작한다.
    //
    // loop: 결과를 넘길 이벤트 루프. 이 객체보다 오래 살아 있어야 한다.
    // result_handler: 결과를 받을 함수
    // read_function: 작업을 실행할 함수. 이 객체가 소유하며 파일 읽기 스레드에서만 호출한다.
    FileReader(core::EventLoop& loop, ResultHandler result_handler,
               ReadFunction read_function = ReadFileForResponse);


    // 큐에 남은 작업을 실행하지 않고 버리고, 실행 중인 작업이 끝나기를 기다려 스레드를 join한다.
    //
    // 실행 중이던 작업의 결과는 루프 작업으로 넘어가며, 루프가 다음에 돌 때 결과 처리 함수가 호출된다.
    ~FileReader();


    // 스레드가 이 객체의 주소를 쓰므로 복사를 금지한다.
    FileReader(const FileReader&) = delete;


    // 스레드가 이 객체의 주소를 쓰므로 복사 대입을 금지한다.
    FileReader& operator=(const FileReader&) = delete;


    // 파일 읽기 작업을 큐 끝에 넣는다.
    //
    // task: 실행할 작업
    //
    // 루프 스레드에서 호출한다. 작업 수에 상한은 두지 않는다.
    void Push(FileReadTask task);


private:
    // 파일 읽기 스레드의 본체. 정지 요청이 올 때까지 작업을 꺼내 실행하고 결과를 루프에 넘긴다.
    void RunThread();


    // 결과를 넘길 이벤트 루프
    core::EventLoop& loop_;


    // 결과를 받을 함수. 생성 뒤 바뀌지 않으며 파일 읽기 스레드가 복사만 한다.
    const ResultHandler result_handler_;


    // 작업을 실행할 함수. 파일 읽기 스레드만 호출한다.
    ReadFunction read_function_;


    // tasks_와 is_stop_requested_를 보호하는 잠금
    std::mutex mutex_;


    // 작업이 들어왔거나 정지가 요청되었음을 파일 읽기 스레드에 알린다.
    std::condition_variable work_available_;


    // 실행을 기다리는 작업. mutex_로 보호한다.
    std::deque<FileReadTask> tasks_;


    // 소멸자가 정지를 요청했는지 여부. mutex_로 보호한다.
    bool is_stop_requested_ = false;


    // 파일 읽기 스레드. 다른 멤버를 모두 초기화한 뒤 시작하도록 마지막에 선언한다.
    std::thread thread_;
};


}  // namespace vms::http
