#pragma once

#include <string>
#include <string_view>


namespace vms::crypto {


// 현재 스레드의 OpenSSL 오류 큐에 쌓인 오류를 읽어 메시지로 만들고 큐를 비운다.
//
// operation: 실패한 작업 설명. 예: "인증서 서명"
// 반환값: "작업: 오류1; 오류2" 형식의 메시지. 큐가 비어 있으면 "작업: 원인 불명"
//
// OpenSSL 함수가 실패를 반환한 직후에 호출해야 한다. 큐를 비우므로 이전 실패의 오류가
// 다음 실패의 메시지에 섞이지 않는다.
std::string TakeOpenSslError(std::string_view operation);


}  // namespace vms::crypto
