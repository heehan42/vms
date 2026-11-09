#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>


namespace vms::recording {


// 닫힌 녹화 파일 안의 조각(moof+mdat) 하나의 위치와 길이.
//
// 색인 레코드에서 CRC를 뺀 값이다. 재생 목록이 조각마다 바이트 범위와 길이를 쓰는 데 필요하다.
struct RecordedFragment {
    // 조각 첫 프레임의 재생 시각(파일 첫 프레임으로부터의 경과 시간, 1/90000초 단위)
    std::uint64_t start_time = 0;


    // 조각이 덮는 시간(1/90000초 단위)
    std::uint32_t duration = 0;


    // 녹화 파일 안에서 조각이 시작하는 위치(파일 시작으로부터의 바이트 수)
    std::uint64_t offset = 0;


    // 조각의 크기(바이트)
    std::uint32_t size = 0;
};


// 다 쓰고 닫은 녹화 파일 하나(.mp4와 .idx 짝).
struct RecordedFile {
    // 확장자를 뺀 파일 이름
    std::string stem;


    // 파일 시작 시각(유닉스 밀리초). 색인 헤더의 값이다.
    std::int64_t start_unix_ms = 0;


    // 파일이 덮는 시간(90kHz). 마지막 색인 레코드의 시작 + 길이다.
    std::uint64_t duration = 0;


    // 파일 안 조각 목록. 파일 안 순서이며, 디스크의 유효한 색인 레코드와 같다.
    //
    // 녹화 스레드의 목록(RecordingFileStore)은 보관 분량과 비상 삭제 판단에 시작 시각과 길이만 쓰므로 이 목록을
    // 비워 둔다. 이벤트 루프 스레드의 재생 목록(playback::RecordingLibrary)만 채운 값을 갖는다.
    std::vector<RecordedFragment> fragments;
};


// 카메라 폴더별 닫힌 녹화 파일 목록.
//
// 키는 녹화 디렉터리 아래 카메라 폴더 이름(등록되지 않은 카메라 포함)이고, 값은 시작 시각 오름차순의 파일 목록이다.
// 시작할 때 복구가 만들고, 녹화 스레드가 보관 분량과 비상 삭제 판단에 쓰며 갱신한다.
using RecordedFileCatalog = std::map<std::string, std::vector<RecordedFile>>;


}  // namespace vms::recording
