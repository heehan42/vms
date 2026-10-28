#include "core/event_loop.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "core/unique_fd.h"
#include "test_support.h"

using vms::core::EventLoop;
using vms::core::UniqueFd;
using vms::test::Expect;

using namespace std::chrono_literals;


namespace {


// 논블로킹 eventfd를 만든다.
//
// 반환값: 새 eventfd. 실패하면 빈 UniqueFd
UniqueFd CreateEventFd() { return UniqueFd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)); }


// eventfd에 값을 써서 읽기 가능 상태로 만든다.
//
// fd: 대상 eventfd
void Signal(const UniqueFd& fd) {
    const std::uint64_t value = 1;
    [[maybe_unused]] const ssize_t written = ::write(fd.Get(), &value, sizeof(value));
}


// eventfd의 값을 읽어 읽기 가능 상태를 해제한다.
//
// fd: 대상 eventfd
void Drain(const UniqueFd& fd) {
    std::uint64_t value = 0;
    [[maybe_unused]] const ssize_t read_size = ::read(fd.Get(), &value, sizeof(value));
}


// 등록한 fd에 이벤트가 발생하면 핸들러가 이벤트 비트와 함께 호출되는지 검사한다.
void TestHandlerReceivesEvent() {
    auto loop = EventLoop::Create();
    if (!loop) {
        Expect(false, "이벤트 루프 생성");
        return;
    }
    UniqueFd event_fd = CreateEventFd();

    std::uint32_t received_events = 0;
    loop->Add(event_fd.Get(), EPOLLIN, [&](std::uint32_t events) {
        received_events = events;
        Drain(event_fd);
        loop->Stop();
    });
    Signal(event_fd);

    Expect(!loop->Run(), "Stop 후 Run이 오류 없이 반환되어야 한다");
    Expect((received_events & EPOLLIN) != 0, "핸들러가 EPOLLIN을 받아야 한다");
    loop->Remove(event_fd.Get());
}


// 이미 등록된 fd를 다시 등록하면 실패하는지 검사한다.
void TestAddRejectsDuplicateFd() {
    auto loop = EventLoop::Create();
    if (!loop) {
        Expect(false, "이벤트 루프 생성");
        return;
    }
    UniqueFd event_fd = CreateEventFd();

    Expect(!loop->Add(event_fd.Get(), EPOLLIN, [](std::uint32_t) {}), "첫 등록은 성공해야 한다");
    Expect(static_cast<bool>(loop->Add(event_fd.Get(), EPOLLIN, [](std::uint32_t) {})),
           "같은 fd를 다시 등록하면 실패해야 한다");
    loop->Remove(event_fd.Get());
}


// 같은 이벤트 묶음에서 먼저 실행된 핸들러가 다른 fd의 등록을 해제하면,
// 해제된 fd의 핸들러가 호출되지 않는지 검사한다.
void TestRemovedHandlerIsSkippedInSameBatch() {
    auto loop = EventLoop::Create();
    if (!loop) {
        Expect(false, "이벤트 루프 생성");
        return;
    }
    UniqueFd first_fd = CreateEventFd();
    UniqueFd second_fd = CreateEventFd();

    int handler_call_count = 0;
    loop->Add(first_fd.Get(), EPOLLIN, [&](std::uint32_t) {
        ++handler_call_count;
        loop->Remove(second_fd.Get());
        loop->Stop();
    });
    loop->Add(second_fd.Get(), EPOLLIN, [&](std::uint32_t) {
        ++handler_call_count;
        loop->Remove(first_fd.Get());
        loop->Stop();
    });
    Signal(first_fd);
    Signal(second_fd);

    loop->Run();
    Expect(handler_call_count == 1, "해제된 fd의 핸들러는 같은 묶음에서 호출되지 않아야 한다");
    loop->Remove(first_fd.Get());
    loop->Remove(second_fd.Get());
}


// 핸들러가 자기 자신의 등록을 해제한 뒤에도 캡처한 상태를 안전하게 쓸 수 있고,
// 이후에는 다시 호출되지 않는지 검사한다.
void TestHandlerCanRemoveItself() {
    auto loop = EventLoop::Create();
    if (!loop) {
        Expect(false, "이벤트 루프 생성");
        return;
    }
    UniqueFd self_removing_fd = CreateEventFd();
    UniqueFd stop_fd = CreateEventFd();

    int self_removing_call_count = 0;
    std::string captured_after_remove;
    loop->Add(self_removing_fd.Get(), EPOLLIN,
              [&, label = std::string("self-removing handler")](std::uint32_t) {
                  ++self_removing_call_count;
                  loop->Remove(self_removing_fd.Get());
                  captured_after_remove = label;
                  Signal(stop_fd);
              });
    loop->Add(stop_fd.Get(), EPOLLIN, [&](std::uint32_t) { loop->Stop(); });
    Signal(self_removing_fd);

    loop->Run();
    Expect(self_removing_call_count == 1, "자기 해제한 핸들러는 한 번만 호출되어야 한다");
    Expect(captured_after_remove == "self-removing handler",
           "해제 직후에도 핸들러의 캡처 상태가 유지되어야 한다");
    loop->Remove(stop_fd.Get());
}


// Modify()로 감시 이벤트를 바꾸고, 등록되지 않은 fd 변경은 실패하는지 검사한다.
void TestModifyChangesWatchedEvents() {
    auto loop = EventLoop::Create();
    if (!loop) {
        Expect(false, "이벤트 루프 생성");
        return;
    }
    UniqueFd event_fd = CreateEventFd();

    // eventfd는 항상 쓰기 가능하므로 EPOLLOUT으로 바꾸면 즉시 핸들러가 호출된다.
    std::uint32_t received_events = 0;
    loop->Add(event_fd.Get(), EPOLLIN, [&](std::uint32_t events) {
        received_events = events;
        loop->Stop();
    });
    Expect(!loop->Modify(event_fd.Get(), EPOLLOUT), "등록된 fd의 이벤트 변경은 성공해야 한다");

    loop->Run();
    Expect((received_events & EPOLLOUT) != 0, "변경한 이벤트(EPOLLOUT)를 받아야 한다");
    Expect(static_cast<bool>(loop->Modify(-1, EPOLLIN)), "등록되지 않은 fd 변경은 실패해야 한다");
    loop->Remove(event_fd.Get());
}


// 타이머가 기한 순서대로 실행되고, 기한이 같으면 등록 순서대로 실행되는지 검사한다.
void TestTimersRunInDeadlineOrder() {
    auto loop = EventLoop::Create();
    if (!loop) {
        Expect(false, "이벤트 루프 생성");
        return;
    }

    std::vector<int> run_order;
    loop->AddTimer(30ms, [&] {
        run_order.push_back(3);
        loop->Stop();
    });
    loop->AddTimer(10ms, [&] { run_order.push_back(1); });
    loop->AddTimer(0ms, [&] { run_order.push_back(0); });
    // 같은 기한을 만들려고 음수 지연을 쓴다. 둘 다 이미 지났으므로 등록 순서대로 실행되어야 한다.
    loop->AddTimer(-1000ms, [&] { run_order.push_back(-2); });
    loop->AddTimer(-1000ms, [&] { run_order.push_back(-1); });

    const auto started_at = std::chrono::steady_clock::now();
    Expect(!loop->Run(), "Stop 후 Run이 오류 없이 반환되어야 한다");
    Expect(std::chrono::steady_clock::now() - started_at >= 30ms, "가장 늦은 기한까지 기다려야 한다");
    Expect(run_order == std::vector<int>{-2, -1, 0, 1, 3}, "기한 순서, 같은 기한은 등록 순서");
}


// 취소한 타이머는 실행되지 않고, 없는 ID의 취소는 무시되는지 검사한다.
void TestCancelledTimerDoesNotRun() {
    auto loop = EventLoop::Create();
    if (!loop) {
        Expect(false, "이벤트 루프 생성");
        return;
    }

    bool is_cancelled_timer_run = false;
    const EventLoop::TimerId cancelled = loop->AddTimer(5ms, [&] { is_cancelled_timer_run = true; });
    loop->AddTimer(20ms, [&] { loop->Stop(); });
    loop->CancelTimer(cancelled);
    loop->CancelTimer(cancelled);
    loop->CancelTimer(9999);

    loop->Run();
    Expect(!is_cancelled_timer_run, "취소한 타이머는 실행되지 않아야 한다");
}


// 핸들러 안에서 자기 자신을 취소해도 안전하고, 새로 등록한 0초 타이머는 다음 반복에 실행되는지 검사한다.
void TestTimerHandlerCanCancelSelfAndAddTimer() {
    auto loop = EventLoop::Create();
    if (!loop) {
        Expect(false, "이벤트 루프 생성");
        return;
    }
    UniqueFd event_fd = CreateEventFd();

    // 0초 타이머가 같은 반복에서 실행되면 fd 핸들러보다 먼저 기록된다.
    std::vector<std::string> run_order;
    EventLoop::TimerId self_id = 0;
    self_id = loop->AddTimer(0ms, [&] {
        run_order.push_back("first");
        loop->CancelTimer(self_id);
        Signal(event_fd);
        loop->AddTimer(0ms, [&] {
            run_order.push_back("second");
            loop->Stop();
        });
    });
    loop->Add(event_fd.Get(), EPOLLIN, [&](std::uint32_t) {
        run_order.push_back("fd");
        Drain(event_fd);
    });

    loop->Run();
    Expect(run_order == std::vector<std::string>{"first", "fd", "second"},
           "새 0초 타이머는 다음 반복에서 fd 이벤트 뒤에 실행되어야 한다");
    loop->Remove(event_fd.Get());
}


// 타이머를 기다리는 동안 fd 이벤트도 처리하는지 검사한다.
void TestTimersAndFdEventsTogether() {
    auto loop = EventLoop::Create();
    if (!loop) {
        Expect(false, "이벤트 루프 생성");
        return;
    }
    UniqueFd event_fd = CreateEventFd();

    int fd_event_count = 0;
    bool is_timer_run = false;
    loop->Add(event_fd.Get(), EPOLLIN, [&](std::uint32_t) {
        ++fd_event_count;
        Drain(event_fd);
    });
    loop->AddTimer(10ms, [&] { Signal(event_fd); });
    loop->AddTimer(40ms, [&] {
        is_timer_run = true;
        loop->Stop();
    });
    Signal(event_fd);

    loop->Run();
    Expect(fd_event_count == 2, "타이머 대기 중과 타이머가 일으킨 fd 이벤트를 모두 처리해야 한다");
    Expect(is_timer_run, "fd 이벤트 뒤에도 타이머가 실행되어야 한다");
    loop->Remove(event_fd.Get());
}


}  // namespace


int main() {
    TestHandlerReceivesEvent();
    TestAddRejectsDuplicateFd();
    TestRemovedHandlerIsSkippedInSameBatch();
    TestHandlerCanRemoveItself();
    TestModifyChangesWatchedEvents();
    TestTimersRunInDeadlineOrder();
    TestCancelledTimerDoesNotRun();
    TestTimerHandlerCanCancelSelfAndAddTimer();
    TestTimersAndFdEventsTogether();
    return vms::test::ExitCode();
}
