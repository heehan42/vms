#include "core/event_loop.h"

#include <sys/epoll.h>

#include <array>
#include <cerrno>
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
                                             static_cast<int>(events.size()), -1);
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
    }
    return {};
}


void EventLoop::Stop() { is_stop_requested_ = true; }


}  // namespace vms::core
