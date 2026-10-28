#include "core/event_loop.h"

#include <sys/epoll.h>

#include <array>
#include <cerrno>
#include <limits>
#include <utility>

#include "core/system_error.h"


namespace vms::core {


std::expected<EventLoop, std::error_code> EventLoop::Create() {
    UniqueFd epoll_fd(::epoll_create1(EPOLL_CLOEXEC));
    if (!epoll_fd.IsValid()) {
        return std::unexpected(LastSystemError());
    }
    return EventLoop(std::move(epoll_fd));
}


EventLoop::EventLoop(UniqueFd epoll_fd) : epoll_fd_(std::move(epoll_fd)) {}


std::error_code EventLoop::Add(int fd, std::uint32_t events, Handler handler) {
    if (registrations_.contains(fd)) {
        return std::make_error_code(std::errc::file_exists);
    }

    auto registration = std::make_unique<Registration>(std::move(handler));
    epoll_event event{.events = events, .data = {.ptr = registration.get()}};
    if (::epoll_ctl(epoll_fd_.Get(), EPOLL_CTL_ADD, fd, &event) != 0) {
        return LastSystemError();
    }

    registrations_.emplace(fd, std::move(registration));
    return {};
}


std::error_code EventLoop::Modify(int fd, std::uint32_t events) {
    const auto found = registrations_.find(fd);
    if (found == registrations_.end()) {
        return std::make_error_code(std::errc::no_such_file_or_directory);
    }

    epoll_event event{.events = events, .data = {.ptr = found->second.get()}};
    if (::epoll_ctl(epoll_fd_.Get(), EPOLL_CTL_MOD, fd, &event) != 0) {
        return LastSystemError();
    }
    return {};
}


void EventLoop::Remove(int fd) {
    const auto found = registrations_.find(fd);
    if (found == registrations_.end()) {
        return;
    }

    // 이미 닫힌 fd는 커널이 epoll에서 자동으로 제거하므로 실패해도 무시한다.
    ::epoll_ctl(epoll_fd_.Get(), EPOLL_CTL_DEL, fd, nullptr);

    // 같은 이벤트 묶음에 이 등록을 가리키는 이벤트가 남아 있을 수 있으므로,
    // 표시만 해 두고 메모리는 묶음 처리가 끝날 때 해제한다.
    found->second->is_removed = true;
    removed_registrations_.push_back(std::move(found->second));
    registrations_.erase(found);
}


std::error_code EventLoop::Run() {
    std::array<epoll_event, 64> events;

    is_stop_requested_ = false;
    while (!is_stop_requested_) {
        const int event_count = ::epoll_wait(epoll_fd_.Get(), events.data(),
                                             static_cast<int>(events.size()), ComputeWaitTimeout());
        if (event_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return LastSystemError();
        }

        for (int index = 0; index < event_count; ++index) {
            auto* registration = static_cast<Registration*>(events[index].data.ptr);
            if (registration->is_removed) {
                continue;
            }
            registration->handler(events[index].events);
        }

        removed_registrations_.clear();

        RunExpiredTimers();
    }
    return {};
}


void EventLoop::Stop() { is_stop_requested_ = true; }


EventLoop::TimerId EventLoop::AddTimer(std::chrono::milliseconds delay, TimerHandler handler) {
    const TimerId id = ++last_timer_id_;
    const auto deadline = std::chrono::steady_clock::now() + delay;
    timers_.emplace(TimerKey(deadline, id), std::move(handler));
    timer_deadlines_.emplace(id, deadline);
    return id;
}


void EventLoop::CancelTimer(TimerId id) {
    const auto found = timer_deadlines_.find(id);
    if (found == timer_deadlines_.end()) {
        return;
    }
    timers_.erase(TimerKey(found->second, id));
    timer_deadlines_.erase(found);
}


int EventLoop::ComputeWaitTimeout() const {
    if (timers_.empty()) {
        return -1;
    }

    const auto remaining = timers_.begin()->first.first - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
        return 0;
    }
    const auto remaining_milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining);
    if (remaining_milliseconds.count() > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(remaining_milliseconds.count());
}


void EventLoop::RunExpiredTimers() {
    const auto now = std::chrono::steady_clock::now();
    // 이 시점 이후에 발급된 ID는 핸들러 안에서 새로 등록한 타이머다. 다음 반복으로 미뤄
    // 0초 타이머를 반복 등록해도 fd 이벤트가 굶지 않게 한다. 새 타이머의 기한은 now 이후이고
    // 기한이 같으면 ID 순서이므로, 처음 만난 새 타이머 뒤에는 이번에 실행할 타이머가 없다.
    const TimerId last_id_before_run = last_timer_id_;

    while (!timers_.empty()) {
        const auto first = timers_.begin();
        const auto [deadline, id] = first->first;
        if (deadline > now || id > last_id_before_run) {
            break;
        }

        // 실행 전에 테이블에서 빼므로, 핸들러 안에서 같은 ID를 취소해도 아무 일도 없다.
        TimerHandler handler = std::move(first->second);
        timers_.erase(first);
        timer_deadlines_.erase(id);
        handler();
    }
}


}  // namespace vms::core
