#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>


namespace vms::recording {


// 다 쓰고 닫은 녹화 파일 하나(.mp4와 .idx 짝).
struct RecordedFile {
    // 확장자를 뺀 파일 이름
    std::string stem;


    // 파일 시작 시각(유닉스 밀리초). 색인 헤더의 값이다.
    std::int64_t start_unix_ms = 0;


    // 파일이 덮는 시간(90kHz). 마지막 색인 레코드의 시작 + 길이다.
    std::uint64_t duration = 0;
};


// 카메라 폴더별 닫힌 녹화 파일 목록.
//
// 키는 녹화 디렉터리 아래 카메라 폴더 이름(등록되지 않은 카메라 포함)이고, 값은 시작 시각 오름차순의 파일 목록이다.
// 시작할 때 복구가 만들고, 녹화 스레드가 보관 분량과 비상 삭제 판단에 쓰며 갱신한다.
using RecordedFileCatalog = std::map<std::string, std::vector<RecordedFile>>;


}  // namespace vms::recording
