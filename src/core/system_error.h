#pragma once

#include <cerrno>
#include <system_error>


namespace vms::core {


// 직전 시스템 호출이 남긴 errno를 std::error_code로 바꾼다.
//
// 반환값: errno 값과 system_category를 담은 오류 코드
//
// errno는 다음 시스템 호출이나 라이브러리 호출로 덮어쓰일 수 있으므로,
// 실패한 호출 바로 뒤에서 호출해야 한다.
inline std::error_code LastSystemError() {
    return std::error_code(errno, std::system_category());
}


}  // namespace vms::core
