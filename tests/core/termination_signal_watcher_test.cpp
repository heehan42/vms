#include "core/termination_signal_watcher.h"

#include <signal.h>
#include <unistd.h>

#include "core/event_loop.h"
#include "test_support.h"

using vms::core::EventLoop;
using vms::core::TerminationSignalWatcher;
using vms::test::Expect;


int main() {
    auto loop = EventLoop::Create();
    if (!loop) {
        Expect(false, "이벤트 루프 생성");
        return vms::test::ExitCode();
    }

    // 셸이 백그라운드 작업을 실행할 때처럼 SIGINT를 무시로 둔 상태에서 시작해도 받을 수 있어야 한다.
    ::signal(SIGINT, SIG_IGN);

    int received_signal = 0;
    auto watcher = TerminationSignalWatcher::Start(*loop, [&](int signal_number) {
        received_signal = signal_number;
        loop->Stop();
    });
    if (!watcher) {
        Expect(false, "종료 시그널 감시 시작");
        return vms::test::ExitCode();
    }

    // 시그널이 막혀 있으므로 프로세스가 종료되지 않고 signalfd에 대기 상태로 남는다.
    ::kill(::getpid(), SIGTERM);

    Expect(!loop->Run(), "시그널을 받은 뒤 Run이 오류 없이 반환되어야 한다");
    Expect(received_signal == SIGTERM, "핸들러가 SIGTERM 번호를 받아야 한다");

    ::kill(::getpid(), SIGINT);
    Expect(!loop->Run(), "SIGINT를 받은 뒤 Run이 오류 없이 반환되어야 한다");
    Expect(received_signal == SIGINT, "시작 전 무시로 설정된 SIGINT도 핸들러가 받아야 한다");
    return vms::test::ExitCode();
}
