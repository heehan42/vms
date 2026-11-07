#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "recording/recorded_file.h"


namespace vms::recording {


// 카메라 폴더 하나의 복구 결과 요약. 로그에 쓴다.
struct CameraRecoveryReport {
    // 카메라 폴더 이름
    std::string camera_folder;


    // 남긴 파일 짝 수(잘라 낸 것 포함)
    std::size_t kept_count = 0;


    // 유효하지 않은 꼬리를 잘라 낸 파일 짝 수
    std::size_t truncated_count = 0;


    // 지운 파일 수. 짝 없는 파일은 하나로, 유효한 조각이 없거나 헤더가 깨진 짝은 둘로 센다.
    std::size_t deleted_count = 0;


    // 읽거나 고치지 못해 그대로 두고 목록에서 뺀 파일 짝 수
    std::size_t failed_count = 0;
};


// 녹화 디렉터리 전체의 복구 결과.
struct RecordingRecoveryResult {
    // 카메라 폴더별 남은 닫힌 파일 목록. RecordingWriter의 초기 목록이 된다.
    RecordedFileCatalog files;


    // 카메라 폴더 이름 순서의 요약
    std::vector<CameraRecoveryReport> reports;
};


// 녹화 디렉터리의 모든 카메라 폴더를 검사해 믿을 수 있는 데까지만 남긴다.
//
// directory: 녹화 디렉터리. 있는 디렉터리여야 한다.
// 반환값: 복구 결과. 디렉터리를 열거나 훑을 수 없으면 원인 메시지
//
// 카메라 폴더마다 다음을 한다.
//   1. 이름 규칙에 맞는 .mp4·.idx를 짝짓고, 짝이 없는 파일은 지운다.
//   2. 색인 헤더가 깨진 짝은 지운다.
//   3. 레코드를 앞에서부터 확인해(레코드 CRC, 조각 범위, 조각 CRC32) 처음 실패한 레코드에서 멈춘다.
//   4. 유효한 레코드가 없으면 짝을 지우고, 있으면 마지막 유효 조각·레코드 뒤를 잘라 낸 뒤 fsync한다.
// 이름 규칙에 맞지 않는 항목과 녹화 디렉터리 바로 아래의 일반 파일은 건드리지 않는다. 녹화 스레드와 이벤트 루프를
// 시작하기 전에 메인 스레드에서 동기로 부른다. 파일 전체를 읽어 CRC를 계산하므로 녹화 분량에 비례해 시간이 걸린다.
std::expected<RecordingRecoveryResult, std::string> RecoverRecordings(const std::filesystem::path& directory);


}  // namespace vms::recording
