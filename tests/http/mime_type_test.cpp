#include "http/mime_type.h"

#include "test_support.h"

using vms::http::ContentTypeForPath;
using vms::test::Expect;


int main() {
    Expect(ContentTypeForPath("web/index.html") == "text/html; charset=utf-8", ".html");
    Expect(ContentTypeForPath("app.js") == "text/javascript; charset=utf-8", ".js");
    Expect(ContentTypeForPath("style.css") == "text/css; charset=utf-8", ".css");
    Expect(ContentTypeForPath("data.json") == "application/json", ".json");
    Expect(ContentTypeForPath("logo.svg") == "image/svg+xml", ".svg");
    Expect(ContentTypeForPath("image.png") == "image/png", ".png");
    Expect(ContentTypeForPath("favicon.ico") == "image/x-icon", ".ico");
    Expect(ContentTypeForPath("data.bin") == "application/octet-stream", "알 수 없는 확장자");
    Expect(ContentTypeForPath("README") == "application/octet-stream", "확장자 없음");
    return vms::test::ExitCode();
}
