#pragma once

#include <expected>
#include <functional>
#include <system_error>

#include "core/event_loop.h"
#include "core/unique_fd.h"


namespace vms::core {


// SIGINT, SIGTERM을 signalfd로 받아 이벤트 루프 안에서 핸들러로 전달한다.
//
// sigaction으로 등록하는 시그널 핸들러 대신 signalfd를 쓰므로, 이 객체의 핸들러에서는
// async-signal-safe 제약 없이 일반 코드(로그 출력, 루프 종료 요청 등)를 실행할 수 있다.
// signalfd와 루프 등록을 소유하며, 소멸할 때 등록을 해제하고 fd를 닫는다.
// 연결된 EventLoop보다 먼저 소멸해야 한다.
class TerminationSignalWatcher {
public:
    // 종료 시그널을 받았을 때 호출되는 핸들러.
    //
    // 받은 시그널 번호(SIGINT 또는 SIGTERM)를 인자로 받는다.
    using Handler = std::move_only_function<void(int signal_number)>;


    // 종료 시그널의 기본 동작을 막고 signalfd를 루프에 등록한다.
    //
    // loop: signalfd를 등록할 이벤트 루프. 반환된 객체보다 오래 살아 있어야 한다.
    // on_signal: 시그널을 받을 때마다 루프 스레드에서 호출할 핸들러
    // 반환값: 감시 객체. 시그널 마스크 변경, signalfd 생성, 등록에 실패하면 시스템 오류 코드
    //
    // 프로세스의 시그널 마스크를 바꾸므로, 다른 스레드를 만들기 전에 호출해야
    // 모든 스레드가 같은 마스크를 물려받는다. 이후 SIGINT, SIGTERM으로는 프로세스가
    // 바로 종료되지 않고, 핸들러가 종료 처리를 맡는다.
    // 무시(SIG_IGN)로 설정된 시그널은 signalfd로 전달되지 않으므로, 두 시그널의 처리 방식을
    // 기본 동작(SIG_DFL)으로 되돌린 뒤 막는다.
    static std::expected<TerminationSignalWatcher, std::error_code> Start(EventLoop& loop,
                                                                          Handler on_signal);


    // 루프 등록을 해제하고 signalfd를 닫는다.
    ~TerminationSignalWatcher();


    // other의 등록과 signalfd 소유권을 넘겨받는다.
    //
    // other: 소유권을 넘겨줄 객체. 이동 후에는 소멸 시 아무것도 하지 않는다.
    TerminationSignalWatcher(TerminationSignalWatcher&& other) noexcept = default;


    // 기존 등록을 해제하는 처리가 필요해지므로 이동 대입은 지원하지 않는다.
    TerminationSignalWatcher& operator=(TerminationSignalWatcher&&) = delete;


private:
    // 등록을 마친 signalfd로 감시 객체를 만든다. Start()에서만 호출한다.
    //
    // loop: signal_fd가 등록된 이벤트 루프
    // signal_fd: 소유권을 넘겨받을 signalfd
    TerminationSignalWatcher(EventLoop& loop, UniqueFd signal_fd);


    // signal_fd_가 등록된 이벤트 루프
    EventLoop* loop_;


    // SIGINT, SIGTERM을 읽는 signalfd. 이동된 뒤에는 비어 있다.
    UniqueFd signal_fd_;
};


}  // namespace vms::core
