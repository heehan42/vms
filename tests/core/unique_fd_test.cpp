#include "core/unique_fd.h"

#include <fcntl.h>
#include <unistd.h>

#include <utility>

#include "test_support.h"

using vms::core::UniqueFd;
using vms::test::Expect;


namespace {


// fd가 아직 열려 있는지 확인한다.
//
// fd: 확인할 파일 디스크립터
// 반환값: 열려 있으면 true
bool IsOpen(int fd) { return ::fcntl(fd, F_GETFD) != -1; }


// 소멸할 때 소유한 fd를 닫는지 검사한다.
void TestClosesOnDestruction() {
    int raw_fd = -1;
    {
        UniqueFd fd(::dup(STDERR_FILENO));
        raw_fd = fd.Get();
        Expect(IsOpen(raw_fd), "생성 직후 fd가 열려 있어야 한다");
    }
    Expect(!IsOpen(raw_fd), "소멸 후 fd가 닫혀야 한다");
}


// 이동 생성 후 원본은 빈 상태가 되고, fd는 이동 대상이 소멸할 때만 닫히는지 검사한다.
void TestMoveTransfersOwnership() {
    UniqueFd source(::dup(STDERR_FILENO));
    const int raw_fd = source.Get();

    {
        UniqueFd target(std::move(source));
        Expect(!source.IsValid(), "이동 후 원본은 빈 상태여야 한다");
        Expect(target.Get() == raw_fd, "이동 대상이 fd를 소유해야 한다");

        source.Reset();
        Expect(IsOpen(raw_fd), "빈 원본을 정리해도 이동된 fd는 열려 있어야 한다");
    }
    Expect(!IsOpen(raw_fd), "이동 대상이 소멸하면 fd가 닫혀야 한다");
}


// 이동 대입 시 기존 fd를 닫고 새 fd를 소유하는지 검사한다.
void TestMoveAssignmentClosesPreviousFd() {
    UniqueFd target(::dup(STDERR_FILENO));
    const int previous_fd = target.Get();
    UniqueFd source(::dup(STDERR_FILENO));
    const int moved_fd = source.Get();

    target = std::move(source);
    Expect(!IsOpen(previous_fd), "이동 대입 시 기존 fd가 닫혀야 한다");
    Expect(target.Get() == moved_fd, "이동 대입 후 새 fd를 소유해야 한다");
}


// Release()가 fd를 닫지 않고 소유권만 포기하는지 검사한다.
void TestReleaseKeepsFdOpen() {
    UniqueFd fd(::dup(STDERR_FILENO));
    const int raw_fd = fd.Release();

    Expect(!fd.IsValid(), "Release 후에는 빈 상태여야 한다");
    Expect(IsOpen(raw_fd), "Release한 fd는 닫히지 않아야 한다");
    ::close(raw_fd);
}


}  // namespace


int main() {
    TestClosesOnDestruction();
    TestMoveTransfersOwnership();
    TestMoveAssignmentClosesPreviousFd();
    TestReleaseKeepsFdOpen();
    return vms::test::ExitCode();
}
