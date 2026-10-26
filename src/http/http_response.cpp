#include "http/http_response.h"

#include <format>
#include <utility>


namespace vms::http {


int StatusCodeNumber(HttpStatus status) { return std::to_underlying(status); }


std::string_view ReasonPhrase(HttpStatus status) {
    switch (status) {
        case HttpStatus::kOk:
            return "OK";
        case HttpStatus::kBadRequest:
            return "Bad Request";
        case HttpStatus::kForbidden:
            return "Forbidden";
        case HttpStatus::kNotFound:
            return "Not Found";
        case HttpStatus::kMethodNotAllowed:
            return "Method Not Allowed";
        case HttpStatus::kInternalServerError:
            return "Internal Server Error";
    }
    return "Unknown";
}


std::string BuildResponse(HttpStatus status, std::string_view content_type, std::string_view body,
                          std::span<const HttpHeader> extra_headers) {
    std::string response = std::format(
        "HTTP/1.1 {} {}\r\n"
        "Content-Type: {}\r\n"
        "Content-Length: {}\r\n"
        "Connection: close\r\n",
        StatusCodeNumber(status), ReasonPhrase(status), content_type, body.size());
    for (const HttpHeader& header : extra_headers) {
        std::format_to(std::back_inserter(response), "{}: {}\r\n", header.name, header.value);
    }
    response += "\r\n";
    response += body;
    return response;
}


std::string BuildErrorResponse(HttpStatus status, std::span<const HttpHeader> extra_headers) {
    const std::string body =
        std::format("{} {}\n", StatusCodeNumber(status), ReasonPhrase(status));
    return BuildResponse(status, "text/plain; charset=utf-8", body, extra_headers);
}


}  // namespace vms::http
