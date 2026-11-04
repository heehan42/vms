#pragma once

#include <cstdint>
#include <optional>
#include <span>


namespace vms::media {


// SPS에서 읽은 출력 영상 크기(픽셀).
struct H264Resolution {
    // 가로 크기
    std::uint32_t width = 0;


    // 세로 크기
    std::uint32_t height = 0;
};


// SPS NAL 단위에서 잘라 내기(cropping)를 반영한 영상 크기를 읽는다.
//
// sps: 시작 코드를 뺀 SPS NAL 단위(NAL 헤더 포함). 에뮬레이션 방지 바이트가 들어 있어도 된다.
// 반환값: 영상 크기. SPS가 아니거나 잘렸거나 값이 범위를 벗어나면 빈 optional
//
// 로그에 해상도를 남기는 데만 쓰며, 크기 계산에 필요한 앞부분 필드만 읽는다(ITU-T H.264 7.3.2.1.1).
std::optional<H264Resolution> ParseH264SpsResolution(std::span<const std::uint8_t> sps);


}  // namespace vms::media
