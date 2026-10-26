#include "http/http_response.h"

#include <array>

#include "test_support.h"

using vms::http::BuildErrorResponse;
using vms::http::BuildResponse;
using vms::http::HttpHeader;
using vms::http::HttpStatus;
using vms::test::Expect;


namespace {


// 200 응답의 상태 줄, 기본 헤더, 본문을 정확히 만드는지 검사한다.
void TestBuildsOkResponse() {
    const std::string response = BuildResponse(HttpStatus::kOk, "text/html; charset=utf-8", "<p>hi</p>");
    Expect(response ==
               "HTTP/1.1 200 OK\r\n"
               "Content-Type: text/html; charset=utf-8\r\n"
               "Content-Length: 9\r\n"
               "Connection: close\r\n"
               "\r\n"
               "<p>hi</p>",
           "200 응답의 상태 줄, 기본 헤더, 본문");
}


// 오류 응답에 추가 헤더와 상태 문구 본문을 넣는지 검사한다.
void TestBuildsErrorResponseWithExtraHeader() {
    const std::array extra_headers = {HttpHeader{"Allow", "GET"}};
    const std::string response = BuildErrorResponse(HttpStatus::kMethodNotAllowed, extra_headers);
    Expect(response ==
               "HTTP/1.1 405 Method Not Allowed\r\n"
               "Content-Type: text/plain; charset=utf-8\r\n"
               "Content-Length: 23\r\n"
               "Connection: close\r\n"
               "Allow: GET\r\n"
               "\r\n"
               "405 Method Not Allowed\n",
           "405 응답에 추가 헤더와 상태 문구 본문이 들어가야 한다");
}


// 오류 상태 코드마다 올바른 상태 줄을 만드는지 검사한다.
void TestBuildsEachErrorStatusLine() {
    Expect(BuildErrorResponse(HttpStatus::kBadRequest).starts_with("HTTP/1.1 400 Bad Request\r\n"),
           "400 상태 줄");
    Expect(BuildErrorResponse(HttpStatus::kForbidden).starts_with("HTTP/1.1 403 Forbidden\r\n"),
           "403 상태 줄");
    Expect(BuildErrorResponse(HttpStatus::kNotFound).starts_with("HTTP/1.1 404 Not Found\r\n"),
           "404 상태 줄");
    Expect(BuildErrorResponse(HttpStatus::kInternalServerError)
               .starts_with("HTTP/1.1 500 Internal Server Error\r\n"),
           "500 상태 줄");
}


}  // namespace


int main() {
    TestBuildsOkResponse();
    TestBuildsErrorResponseWithExtraHeader();
    TestBuildsEachErrorStatusLine();
    return vms::test::ExitCode();
}
