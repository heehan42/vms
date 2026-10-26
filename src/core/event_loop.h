#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "core/unique_fd.h"


namespace vms::core {


// epoll로 여러 fd의 이벤트를 기다리고, fd마다 등록된 핸들러를 호출하는 단일 스레드 이벤트 루프.
//
// 루프는 등록된 fd를 소유하지 않는다. fd를 가진 객체가 등록을 해제한 뒤 fd를 닫아야 한다.
// level-triggered 방식이므로 핸들러가 데이터를 다 읽지 않아도 다음 대기에서 다시 호출된다.
// 핸들러는 Run()을 호출한 스레드에서만 실행되며, 루프의 메서드도 같은 스레드에서만 호출해야 한다.
// 핸들러가 루프를 참조하므로 등록을 시작한 뒤에는 EventLoop 객체를 이동하면 안 된다.
class EventLoop {
public:
    // fd에 이벤트가 발생했을 때 호출되는 핸들러.
    //
    // 발생한 epoll 이벤트 비트(EPOLLIN, EPOLLOUT, EPOLLERR 등)를 인자로 받는다.
    using Handler = std::move_only_function<void(std::uint32_t events)>;


    // epoll 인스턴스를 만들어 이벤트 루프를 생성한다.
    //
    // 반환값: 생성된 이벤트 루프. epoll 생성에 실패하면 시스템 오류 코드
    static std::expected<EventLoop, std::error_code> Create();


    // fd를 감시 대상으로 등록한다.
    //
    // fd: 감시할 파일 디스크립터. 이미 등록된 fd이면 실패한다.
    // events: 감시할 epoll 이벤트 비트
    // handler: 이벤트 발생 시 호출할 핸들러
    // 반환값: 성공하면 빈 오류 코드, 실패하면 epoll_ctl의 시스템 오류 코드
    std::error_code Add(int fd, std::uint32_t events, Handler handler);


    // 등록된 fd의 감시 이벤트를 바꾼다.
    //
    // fd: 등록된 파일 디스크립터
    // events: 새로 감시할 epoll 이벤트 비트
    // 반환값: 성공하면 빈 오류 코드, 등록되지 않은 fd이거나 epoll_ctl이 실패하면 오류 코드
    std::error_code Modify(int fd, std::uint32_t events);


    // fd의 등록을 해제한다. 등록되지 않은 fd이면 아무것도 하지 않는다.
    //
    // fd: 해제할 파일 디스크립터
    //
    // 해제 즉시 이 fd에 대한 남은 이벤트는 전달되지 않는다. 핸들러 객체는 현재 처리 중인
    // 이벤트 묶음이 끝난 뒤 파괴하므로, 핸들러 안에서 자기 자신의 등록을 해제해도 안전하다.
    // fd를 닫기 전에 호출해야 같은 번호로 새로 열린 fd와 혼동되지 않는다.
    void Remove(int fd);


    // Stop()이 호출될 때까지 이벤트를 기다리고 핸들러를 호출한다.
    //
    // 반환값: Stop()으로 끝나면 빈 오류 코드, epoll_wait가 실패하면 시스템 오류 코드
    std::error_code Run();


    // 루프 종료를 요청한다.
    //
    // 현재 처리 중인 이벤트 묶음의 핸들러를 모두 호출한 뒤 Run()이 반환된다.
    void Stop();


private:
    // 등록된 fd 하나의 핸들러와 해제 여부.
    //
    // epoll_event.data.ptr가 이 객체를 가리키므로, 해제되어도 현재 이벤트 묶음 처리가
    // 끝날 때까지 메모리를 유지해야 한다.
    struct Registration {
        // 이벤트가 발생했을 때 호출할 핸들러
        Handler handler;


        // Remove()로 해제되어 더 이상 핸들러를 호출하면 안 되는지 여부
        bool is_removed = false;
    };


    // 생성된 epoll 인스턴스로 루프를 만든다. Create()에서만 호출한다.
    //
    // epoll_fd: 소유권을 넘겨받을 epoll 인스턴스
    explicit EventLoop(UniqueFd epoll_fd);


    // 이벤트 대기에 사용하는 epoll 인스턴스
    UniqueFd epoll_fd_;


    // fd별 등록 정보. 등록 해제 시 removed_registrations_로 옮겨진다.
    std::unordered_map<int, std::unique_ptr<Registration>> registrations_;


    // 현재 이벤트 묶음 처리 중에 해제된 등록 정보.
    //
    // 묶음 처리가 끝나면 비워서 핸들러 객체를 파괴한다.
    std::vector<std::unique_ptr<Registration>> removed_registrations_;


    // Stop()이 호출되어 Run()이 현재 이벤트 묶음 뒤에 반환해야 하는지 여부
    bool is_stop_requested_ = false;
};


}  // namespace vms::core
