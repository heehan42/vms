#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/unique_fd.h"


namespace vms::core {


// epoll로 여러 fd의 이벤트를 기다리고, fd마다 등록된 핸들러를 호출하는 단일 스레드 이벤트 루프.
//
// fd 이벤트 외에 일회성 타이머도 제공한다. 반복이 필요하면 핸들러 안에서 다시 등록한다.
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


    // 타이머를 가리키는 ID. 1부터 1씩 늘어나며, 실행되었거나 취소된 ID는 재사용하지 않는다.
    using TimerId = std::uint64_t;


    // 타이머 기한이 지났을 때 호출되는 핸들러.
    using TimerHandler = std::move_only_function<void()>;


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


    // 일회성 타이머를 등록한다.
    //
    // delay: 지금부터 핸들러를 실행할 때까지의 시간. 0이하이면 다음 반복에서 실행한다.
    // handler: 기한이 지났을 때 한 번 호출할 핸들러
    // 반환값: 취소에 쓸 타이머 ID
    //
    // 기한은 steady_clock으로 재며, 기한이 같은 타이머는 등록 순서대로 실행된다.
    // 타이머 핸들러 안에서 등록한 타이머는 기한이 이미 지났더라도 다음 반복에서 실행된다.
    TimerId AddTimer(std::chrono::milliseconds delay, TimerHandler handler);


    // 등록된 타이머를 취소한다. 이미 실행되었거나 취소된 ID, 없는 ID이면 아무것도 하지 않는다.
    //
    // id: AddTimer가 돌려준 타이머 ID
    //
    // 핸들러 객체는 즉시 파괴된다. 상태와 무관하게 호출할 수 있으므로, 소유자는 소멸자에서
    // 보관한 ID를 모두 취소하면 된다.
    void CancelTimer(TimerId id);


    // Stop()이 호출될 때까지 이벤트를 기다리고 핸들러를 호출한다.
    //
    // 반환값: Stop()으로 끝나면 빈 오류 코드, epoll_wait가 실패하면 시스템 오류 코드
    //
    // 한 번 반복할 때마다 fd 이벤트 묶음을 먼저 처리한 뒤 기한이 지난 타이머를 실행한다.
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


    // 타이머 테이블의 정렬 키. 기한이 같으면 ID가 작은(먼저 등록한) 타이머가 앞선다.
    using TimerKey = std::pair<std::chrono::steady_clock::time_point, TimerId>;


    // 생성된 epoll 인스턴스로 루프를 만든다. Create()에서만 호출한다.
    //
    // epoll_fd: 소유권을 넘겨받을 epoll 인스턴스
    explicit EventLoop(UniqueFd epoll_fd);


    // 가장 이른 타이머 기한까지 남은 시간을 epoll_wait의 대기 시간으로 바꾼다.
    //
    // 반환값: 밀리초 단위로 올림한 대기 시간. 기한이 지났으면 0, 타이머가 없으면 -1(무한 대기)
    //
    // 내림하면 기한 직전에 깨어나 아무것도 실행하지 않는 공회전이 생기므로 올림한다.
    int ComputeWaitTimeout() const;


    // 기한이 지난 타이머를 기한 순서대로 테이블에서 빼고 실행한다.
    //
    // 실행 도중 새로 등록된 타이머는 이번 호출에서 실행하지 않는다.
    void RunExpiredTimers();


    // 이벤트 대기에 사용하는 epoll 인스턴스
    UniqueFd epoll_fd_;


    // fd별 등록 정보. 등록 해제 시 removed_registrations_로 옮겨진다.
    std::unordered_map<int, std::unique_ptr<Registration>> registrations_;


    // 현재 이벤트 묶음 처리 중에 해제된 등록 정보.
    //
    // 묶음 처리가 끝나면 비워서 핸들러 객체를 파괴한다.
    std::vector<std::unique_ptr<Registration>> removed_registrations_;


    // 등록된 타이머. (기한, ID) 순서로 정렬되어 begin()이 가장 먼저 실행할 타이머다.
    std::map<TimerKey, TimerHandler> timers_;


    // 타이머 ID별 기한. CancelTimer가 timers_의 키를 찾는 데 쓰며, timers_와 항목이 항상 같다.
    std::unordered_map<TimerId, std::chrono::steady_clock::time_point> timer_deadlines_;


    // 마지막으로 발급한 타이머 ID. 0은 발급하지 않는다.
    TimerId last_timer_id_ = 0;


    // Stop()이 호출되어 Run()이 현재 이벤트 묶음 뒤에 반환해야 하는지 여부
    bool is_stop_requested_ = false;
};


}  // namespace vms::core
