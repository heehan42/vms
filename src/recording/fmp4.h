#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>


namespace vms::recording {


// 조각에 넣을 프레임(샘플) 하나.
//
// NAL 바이트를 값으로 소유하므로, 이벤트 루프에서 만든 뒤 녹화 스레드로 옮겨도 된다.
struct Fmp4Frame {
    // 디코딩 순서대로의 NAL 단위. 각 항목은 시작 코드를 뺀 NAL 하나(NAL 헤더 포함)다.
    std::vector<std::vector<std::uint8_t>> nal_units;


    // 파일 시작 기준 재생 시각(90kHz). B 프레임이 없으므로 디코딩 시각과 같다.
    std::uint64_t decode_time = 0;


    // 다음 프레임까지의 길이(90kHz)
    std::uint32_t duration = 0;
};


// NAL 단위를 조각의 샘플 데이터에 넣는지 확인한다.
//
// nal_unit: 시작 코드를 뺀 NAL 단위
// 반환값: 비어 있지 않고 AUD·SPS·PPS가 아니면 true
//
// SPS·PPS는 초기화 구간의 avcC에만 두고, AUD는 길이 접두 형식에서 필요 없으므로 조각에서 뺀다.
bool IsFragmentNalUnit(std::span<const std::uint8_t> nal_unit);


// 초기화 구간(ftyp + moov)을 만든다.
//
// sps: 시작 코드를 뺀 SPS NAL. 해상도는 SPS에서, 프로파일·호환성·레벨은 NAL 헤더 뒤 3바이트에서 읽는다.
// pps: 시작 코드를 뺀 PPS NAL
// 반환값: 초기화 구간 바이트. SPS가 4바이트보다 짧거나 해상도를 읽을 수 없거나, 매개변수 집합이 65535바이트를
//   넘거나 PPS가 비었으면 원인 메시지
//
// 영상 트랙 하나(track_id 1, 시간 단위 90000)만 두며, 프레임 표는 비우고 mvex로 조각을 쓴다는 것을 알린다.
std::expected<std::vector<std::uint8_t>, std::string> BuildFmp4InitSegment(std::span<const std::uint8_t> sps,
                                                                           std::span<const std::uint8_t> pps);


// GOP 하나를 담은 조각(moof + mdat)을 만든다.
//
// frames: 디코딩 순서대로의 프레임. 하나 이상이며 첫 프레임이 키프레임이어야 한다.
// sequence_number: 파일 안 조각 번호(1부터)
// 반환값: 조각 바이트
//
// tfdt는 첫 프레임의 decode_time이고, trun에는 프레임마다 길이·크기·플래그를 둔다(첫 프레임만 sync). mdat에는 각
// 프레임의 NAL 중 IsFragmentNalUnit이 true인 것을 4바이트 빅 엔디언 길이와 함께 순서대로 이어 붙인다.
std::vector<std::uint8_t> BuildFmp4Fragment(std::span<const Fmp4Frame> frames, std::uint32_t sequence_number);


}  // namespace vms::recording
