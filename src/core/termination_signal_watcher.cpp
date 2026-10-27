#include "core/termination_signal_watcher.h"

#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <utility>

#include "core/system_error.h"


namespace vms::core {


std::expected<TerminationSignalWatcher, std::error_code> TerminationSignalWatcher::Start(
    EventLoop& loop, Handler on_signal) {
    sigset_t signals;
    ::sigemptyset(&signals);
    ::sigaddset(&signals, SIGINT);
    ::sigaddset(&signals, SIGTERM);

    // 무시(SIG_IGN)로 설정된 시그널은 막아 두어도 대기 상태가 되지 않고 버려져 signalfd에 오지 않는다.
    // 셸이 백그라운드 작업의 SIGINT를 무시로 바꿔 실행하는 경우가 있으므로 기본 동작으로 되돌린다.
    struct sigaction default_action{};
    default_action.sa_handler = SIG_DFL;
    ::sigemptyset(&default_action.sa_mask);
    if (::sigaction(SIGINT, &default_action, nullptr) != 0 ||
        ::sigaction(SIGTERM, &default_action, nullptr) != 0) {
        return std::unexpected(LastSystemError());
    }

    // 시그널을 막아 두어야 기본 동작(프로세스 종료) 대신 signalfd에 대기 상태로 쌓인다.
    if (::sigprocmask(SIG_BLOCK, &signals, nullptr) != 0) {
        return std::unexpected(LastSystemError());
    }

    UniqueFd signal_fd(::signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC));
    if (!signal_fd.IsValid()) {
        return std::unexpected(LastSystemError());
    }

    const int raw_signal_fd = signal_fd.Get();
    const std::error_code add_error =
        loop.Add(raw_signal_fd, EPOLLIN,
                 [raw_signal_fd, on_signal = std::move(on_signal)](std::uint32_t) mutable {
                     signalfd_siginfo signal_info;
                     while (::read(raw_signal_fd, &signal_info, sizeof(signal_info)) ==
                            sizeof(signal_info)) {
                         on_signal(static_cast<int>(signal_info.ssi_signo));
                     }
                 });
    if (add_error) {
        return std::unexpected(add_error);
    }
    return TerminationSignalWatcher(loop, std::move(signal_fd));
}


TerminationSignalWatcher::TerminationSignalWatcher(EventLoop& loop, UniqueFd signal_fd)
    : loop_(&loop), signal_fd_(std::move(signal_fd)) {}


TerminationSignalWatcher::~TerminationSignalWatcher() {
    if (signal_fd_.IsValid()) {
        loop_->Remove(signal_fd_.Get());
    }
}


}  // namespace vms::core
