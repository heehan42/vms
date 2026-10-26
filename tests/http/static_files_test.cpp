#include "http/static_files.h"

#include <stdlib.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "test_support.h"

using vms::http::HttpStatus;
using vms::http::ReadFileContent;
using vms::http::ResolveStaticFilePath;
using vms::test::Expect;

namespace fs = std::filesystem;


namespace {


// 파일을 만들고 내용을 쓴다.
//
// path: 만들 파일 경로
// content: 쓸 내용
void WriteFile(const fs::path& path, std::string_view content) {
    std::ofstream(path) << content;
}


// 대상 해석이 지정한 상태 코드로 실패하는지 확인한다.
//
// web_root: 웹 루트
// target: 요청 대상
// expected_status: 기대하는 실패 상태 코드
// 반환값: 기대한 상태 코드로 실패하면 true
bool FailsWith(const fs::path& web_root, std::string_view target, HttpStatus expected_status) {
    const auto resolved = ResolveStaticFilePath(web_root, target);
    return !resolved && resolved.error() == expected_status;
}


// 루트, 쿼리가 붙은 경로, 하위 디렉터리의 파일을 웹 루트 안의 경로로 바꾸는지 검사한다.
//
// web_root: 테스트용 파일을 만들어 둔 임시 웹 루트
void TestResolvesFiles(const fs::path& web_root) {
    const auto index = ResolveStaticFilePath(web_root, "/");
    Expect(index && *index == web_root / "index.html", "/는 index.html이어야 한다");

    const auto script = ResolveStaticFilePath(web_root, "/app.js?v=1");
    Expect(script && *script == web_root / "app.js", "쿼리 문자열은 무시해야 한다");

    const auto nested = ResolveStaticFilePath(web_root, "/assets/style.css");
    Expect(nested && *nested == web_root / "assets" / "style.css", "하위 디렉터리의 파일");
}


// 잘못된 대상, 상위 경로, 없는 파일, 디렉터리를 알맞은 상태 코드로 거부하는지 검사한다.
//
// web_root: 테스트용 파일을 만들어 둔 임시 웹 루트
void TestRejectsInvalidTargets(const fs::path& web_root) {
    Expect(FailsWith(web_root, "app.js", HttpStatus::kBadRequest), "/로 시작하지 않는 대상은 400");
    Expect(FailsWith(web_root, "/../etc/passwd", HttpStatus::kForbidden), "상위 경로는 403");
    Expect(FailsWith(web_root, "/assets/../app.js", HttpStatus::kForbidden),
           "중간의 ..도 403");
    Expect(FailsWith(web_root, "/missing.js", HttpStatus::kNotFound), "없는 파일은 404");
    Expect(FailsWith(web_root, "/assets", HttpStatus::kNotFound), "디렉터리는 404");
    Expect(FailsWith(web_root, "/assets/", HttpStatus::kNotFound), "/로 끝나는 디렉터리도 404");
    Expect(FailsWith(web_root, "//etc/passwd", HttpStatus::kNotFound),
           "빈 구성 요소 뒤의 경로가 웹 루트 밖의 절대 경로가 되면 안 된다");
}


// 파일 내용 전체를 읽고, 없는 파일은 오류로 돌려주는지 검사한다.
//
// web_root: 테스트용 파일을 만들어 둔 임시 웹 루트
void TestReadsFileContent(const fs::path& web_root) {
    const auto content = ReadFileContent(web_root / "app.js");
    Expect(content && *content == "console.log('app');", "파일 내용 전체를 읽어야 한다");

    Expect(!ReadFileContent(web_root / "missing.js"), "없는 파일 읽기는 오류여야 한다");
}


}  // namespace


int main() {
    std::error_code error;
    std::string root_template = (fs::temp_directory_path(error) / "static_files_test.XXXXXX").string();
    if (error || ::mkdtemp(root_template.data()) == nullptr) {
        Expect(false, "임시 웹 루트 생성");
        return vms::test::ExitCode();
    }
    const fs::path web_root = root_template;

    WriteFile(web_root / "index.html", "<p>index</p>");
    WriteFile(web_root / "app.js", "console.log('app');");
    fs::create_directory(web_root / "assets", error);
    WriteFile(web_root / "assets" / "style.css", "body {}");

    TestResolvesFiles(web_root);
    TestRejectsInvalidTargets(web_root);
    TestReadsFileContent(web_root);

    fs::remove_all(web_root, error);
    return vms::test::ExitCode();
}
