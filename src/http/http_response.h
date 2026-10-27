#pragma once

#include <span>
#include <string>
#include <string_view>


namespace vms::http {


// 서버가 응답에 사용하는 HTTP 상태 코드.
//
// 열거값의 정수값이 그대로 상태 코드 숫자다.
enum class HttpStatus {
    // 요청한 파일을 응답함
    kOk = 200,

    // 요청 줄이나 헤더 형식이 잘못되었거나 헤더 영역이 너무 큼
    kBadRequest = 400,

    // 웹 루트 밖을 가리키는 경로
    kForbidden = 403,

    // 파일이 없거나 일반 파일이 아님
    kNotFound = 404,

    // 지원하지 않는 메서드
    kMethodNotAllowed = 405,

    // 본문이 필요한 경로에 Content-Length가 없음
    kLengthRequired = 411,

    // 본문이 허용 크기를 넘음
    kContentTooLarge = 413,

    // 엔드포인트가 받지 않는 본문 형식(Content-Type)
    kUnsupportedMediaType = 415,

    // 확인한 파일을 읽지 못했거나 엔드포인트 처리 중 내부 오류
    kInternalServerError = 500,

    // 지원하지 않는 전송 방식(Transfer-Encoding)
    kNotImplemented = 501,
};


// 응답에 추가로 넣을 헤더 한 줄.
//
// 이름과 값은 문자열을 소유하지 않으므로, 응답 바이트를 만드는 동안 원본이 살아 있어야 한다.
struct HttpHeader {
    // 헤더 이름. 예: "Allow"
    std::string_view name;


    // 헤더 값. 예: "GET"
    std::string_view value;
};


// 상태 코드의 숫자값을 돌려준다.
//
// status: 상태 코드
// 반환값: 200, 404 같은 숫자값
int StatusCodeNumber(HttpStatus status);


// 상태 코드의 표준 사유 문구를 돌려준다.
//
// status: 상태 코드
// 반환값: "OK", "Not Found" 같은 문구. 정적 문자열을 가리키므로 수명 제한이 없다.
std::string_view ReasonPhrase(HttpStatus status);


// 상태 줄, 헤더, 본문을 이어 붙인 HTTP/1.1 응답 바이트를 만든다.
//
// status: 응답 상태 코드
// content_type: Content-Type 헤더 값
// body: 응답 본문
// extra_headers: 기본 헤더 뒤에 추가할 헤더
// 반환값: 소켓에 그대로 보낼 응답 바이트
//
// Content-Type, Content-Length, Connection: close는 항상 포함한다.
// 응답 후 연결을 닫는 서버 정책에 맞춰 keep-alive는 지원하지 않는다.
std::string BuildResponse(HttpStatus status, std::string_view content_type, std::string_view body,
                          std::span<const HttpHeader> extra_headers = {});


// 오류 응답 본문의 Content-Type
inline constexpr std::string_view kErrorContentType = "text/plain; charset=utf-8";


// 오류 응답 본문을 만든다.
//
// status: 오류 상태 코드
// 반환값: "404 Not Found\n" 형식의 본문
std::string BuildErrorBody(HttpStatus status);


// 상태 코드와 사유 문구를 본문으로 하는 오류 응답 바이트를 만든다.
//
// status: 오류 상태 코드
// extra_headers: 추가할 헤더. 예: 405 응답의 Allow
// 반환값: 소켓에 그대로 보낼 응답 바이트. 본문은 "404 Not Found\n" 형식의 text/plain이다.
std::string BuildErrorResponse(HttpStatus status, std::span<const HttpHeader> extra_headers = {});


}  // namespace vms::http
