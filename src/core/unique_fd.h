#pragma once

#include <unistd.h>

#include <utility>


namespace vms::core {


// 파일 디스크립터 하나의 소유권을 갖고, 소멸할 때 닫는 RAII 래퍼.
//
// 이동만 가능하며, 이동하면 소유권이 대상 객체로 넘어가고 원본은 빈 상태(-1)가 된다.
// Get()으로 얻은 값은 이 객체가 fd를 소유하는 동안에만 유효하다.
class UniqueFd {
public:
    // fd를 소유하지 않은 빈 상태로 만든다.
    UniqueFd() = default;


    // 전달받은 fd의 소유권을 넘겨받는다.
    //
    // fd: 소유할 파일 디스크립터. 음수이면 빈 상태가 된다.
    explicit UniqueFd(int fd) : fd_(fd) {}


    // 소유한 fd가 있으면 닫는다.
    ~UniqueFd() { Reset(); }


    // 같은 fd를 두 객체가 닫지 않도록 복사를 금지한다.
    UniqueFd(const UniqueFd&) = delete;


    // 같은 fd를 두 객체가 닫지 않도록 복사 대입을 금지한다.
    UniqueFd& operator=(const UniqueFd&) = delete;


    // other가 소유한 fd를 넘겨받고, other를 빈 상태로 만든다.
    //
    // other: 소유권을 넘겨줄 객체
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.Release()) {}


    // 현재 fd를 닫고 other가 소유한 fd를 넘겨받는다.
    //
    // other: 소유권을 넘겨줄 객체
    // 반환값: 이 객체
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }


    // 소유한 fd 값을 돌려준다.
    //
    // 반환값: 소유한 fd. 빈 상태이면 -1
    //
    // 소유권은 넘기지 않으므로 호출자가 반환값을 닫으면 안 된다.
    int Get() const { return fd_; }


    // fd를 소유하고 있는지 확인한다.
    //
    // 반환값: fd를 소유하고 있으면 true
    bool IsValid() const { return fd_ >= 0; }


    // fd를 닫지 않고 소유권을 포기한다.
    //
    // 반환값: 지금까지 소유하던 fd. 빈 상태였으면 -1
    //
    // 반환된 fd는 호출자가 닫아야 한다.
    int Release() { return std::exchange(fd_, -1); }


    // 현재 fd를 닫고 새 fd의 소유권을 넘겨받는다.
    //
    // fd: 새로 소유할 파일 디스크립터. 생략하면 빈 상태가 된다.
    void Reset(int fd = -1) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }


private:
    // 소유한 파일 디스크립터. 빈 상태이면 -1이다.
    int fd_ = -1;
};


}  // namespace vms::core
