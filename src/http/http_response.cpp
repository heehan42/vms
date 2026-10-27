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
        case HttpStatus::kLengthRequired:
            return "Length Required";
        case HttpStatus::kContentTooLarge:
            return "Content Too Large";
        case HttpStatus::kUnsupportedMediaType:
            return "Unsupported Media Type";
        case HttpStatus::kInternalServerError:
            return "Internal Server Error";
        case HttpStatus::kNotImplemented:
            return "Not Implemented";
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


std::string BuildErrorBody(HttpStatus status) {
    return std::format("{} {}\n", StatusCodeNumber(status), ReasonPhrase(status));
}


std::string BuildErrorResponse(HttpStatus status, std::span<const HttpHeader> extra_headers) {
    return BuildResponse(status, kErrorContentType, BuildErrorBody(status), extra_headers);
}


}  // namespace vms::http
