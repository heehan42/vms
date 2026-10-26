#pragma once

#include <print>
#include <source_location>
#include <string_view>


namespace vms::test {


// 현재 테스트 프로세스에서 실패한 검사 수를 담은 저장소를 돌려준다.
//
// 반환값: 실패 수에 대한 참조. 프로세스가 끝날 때까지 유효하다.
inline int& FailureCount() {
    static int failure_count = 0;
    return failure_count;
}


// 조건이 거짓이면 실패 위치와 설명을 표준 오류에 출력하고 실패 수를 늘린다.
//
// condition: 검사할 조건
// description: 실패 시 출력할 검사 내용
// location: 호출 위치. 기본값으로 호출한 곳이 채워지므로 직접 넘기지 않는다.
//
// 실패해도 테스트를 중단하지 않으므로, 이후 검사가 앞선 결과에 의존하면 호출자가 먼저 반환해야 한다.
inline void Expect(bool condition, std::string_view description,
                   std::source_location location = std::source_location::current()) {
    if (condition) {
        return;
    }
    ++FailureCount();
    std::println(stderr, "{}:{}: 실패: {}", location.file_name(), location.line(), description);
}


// 지금까지의 검사 결과를 테스트 프로세스의 종료 코드로 바꾼다.
//
// 반환값: 실패가 없으면 0, 있으면 1. main의 반환값으로 사용한다.
inline int ExitCode() {
    if (FailureCount() == 0) {
        return 0;
    }
    std::println(stderr, "실패한 검사: {}개", FailureCount());
    return 1;
}


}  // namespace vms::test
