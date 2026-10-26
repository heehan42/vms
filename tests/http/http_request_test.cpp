#include "http/http_request.h"

#include <string>

#include "test_support.h"

using vms::http::FindRequestHeaderEnd;
using vms::http::HttpStatus;
using vms::http::kMaxRequestHeaderSize;
using vms::http::ParseRequestHeader;
using vms::test::Expect;


namespace {


// 파싱 결과가 400 실패인지 확인한다.
//
// header: 파싱할 요청 헤더 영역
// 반환값: ParseRequestHeader가 kBadRequest를 반환하면 true
bool IsRejected(std::string_view header) {
    const auto request = ParseRequestHeader(header);
    return !request && request.error() == HttpStatus::kBadRequest;
}


// 헤더 끝을 찾으면 빈 줄까지의 크기를 돌려주고 본문은 제외하는지 검사한다.
void TestFindsHeaderEnd() {
    const std::string received = "GET / HTTP/1.1\r\nHost: a\r\n\r\nbody";
    const auto header_end = FindRequestHeaderEnd(received);
    Expect(header_end && *header_end && **header_end == received.size() - 4,
           "빈 줄까지의 크기를 돌려주고 본문은 제외해야 한다");
}


// 빈 줄이 아직 오지 않았으면 더 받아야 한다고 판단하는지 검사한다.
void TestWaitsForIncompleteHeader() {
    const auto header_end = FindRequestHeaderEnd("GET / HTTP/1.1\r\nHost: a\r\n");
    Expect(header_end && !*header_end, "빈 줄이 오지 않았으면 더 받아야 한다");
}


// 헤더 영역이 8 KiB 한도를 넘으면 400으로 판정하고, 한도와 같은 크기는 허용하는지 검사한다.
void TestRejectsOversizedHeader() {
    const std::string without_end(kMaxRequestHeaderSize + 1, 'a');
    const auto unfinished = FindRequestHeaderEnd(without_end);
    Expect(!unfinished && unfinished.error() == HttpStatus::kBadRequest,
           "빈 줄 없이 한도를 넘으면 400이어야 한다");

    const std::string at_limit = std::string(kMaxRequestHeaderSize - 4, 'a') + "\r\n\r\n";
    const auto exact = FindRequestHeaderEnd(at_limit);
    Expect(exact && *exact && **exact == kMaxRequestHeaderSize, "한도와 같은 크기는 허용해야 한다");

    const std::string over_limit = std::string(kMaxRequestHeaderSize - 3, 'a') + "\r\n\r\n";
    const auto over = FindRequestHeaderEnd(over_limit);
    Expect(!over && over.error() == HttpStatus::kBadRequest,
           "빈 줄이 있어도 헤더 영역이 한도를 넘으면 400이어야 한다");
}


// 정상 요청에서 메서드와 쿼리를 포함한 대상 원문을 꺼내는지 검사한다.
void TestParsesValidRequest() {
    const auto request =
        ParseRequestHeader("GET /app.js?v=1 HTTP/1.1\r\nHost: localhost:8080\r\nAccept: */*\r\n\r\n");
    if (!request) {
        Expect(false, "정상 요청 파싱");
        return;
    }
    Expect(request->method == "GET", "메서드");
    Expect(request->target == "/app.js?v=1", "쿼리를 포함한 대상 원문");
}


// 헤더가 없는 요청과 GET이 아닌 메서드도 문법상 허용하는지 검사한다.
void TestParsesRequestWithoutHeaders() {
    const auto request = ParseRequestHeader("POST / HTTP/1.0\r\n\r\n");
    Expect(request && request->method == "POST",
           "헤더가 없는 요청과 GET이 아닌 메서드도 문법상 허용해야 한다");
}


// 형식이 잘못된 요청 줄을 400으로 거부하는지 검사한다.
void TestRejectsMalformedRequestLine() {
    Expect(IsRejected("HELLO\r\n\r\n"), "공백이 없는 요청 줄");
    Expect(IsRejected("GET /\r\n\r\n"), "버전이 없는 요청 줄");
    Expect(IsRejected("GET / HTTP/2.0\r\n\r\n"), "HTTP/1.x가 아닌 버전");
    Expect(IsRejected("GET  / HTTP/1.1\r\n\r\n"), "빈 대상");
    Expect(IsRejected("G(T / HTTP/1.1\r\n\r\n"), "토큰이 아닌 메서드");
    Expect(IsRejected("GET / HTTP/1.1 extra\r\n\r\n"), "버전 뒤의 추가 내용");
}


// 형식이 잘못된 헤더 줄을 400으로 거부하는지 검사한다.
void TestRejectsMalformedHeaderLine() {
    Expect(IsRejected("GET / HTTP/1.1\r\nNoColon\r\n\r\n"), "콜론이 없는 헤더 줄");
    Expect(IsRejected("GET / HTTP/1.1\r\nBad Name: x\r\n\r\n"), "공백이 있는 헤더 이름");
    Expect(IsRejected("GET / HTTP/1.1\r\n: x\r\n\r\n"), "빈 헤더 이름");
    Expect(IsRejected("GET / HTTP/1.1\r\nHost: a\nX: b\r\n\r\n"), "줄 중간의 단독 LF");
}


}  // namespace


int main() {
    TestFindsHeaderEnd();
    TestWaitsForIncompleteHeader();
    TestRejectsOversizedHeader();
    TestParsesValidRequest();
    TestParsesRequestWithoutHeaders();
    TestRejectsMalformedRequestLine();
    TestRejectsMalformedHeaderLine();
    return vms::test::ExitCode();
}
