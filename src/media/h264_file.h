#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>


namespace vms::media {


// 동영상 파일로 받아들이는 최대 크기(바이트). 시작할 때 파일 전체를 메모리에 읽으므로 실수로 큰 파일을
// 지정하는 것을 막는다.
inline constexpr std::uintmax_t kMaxH264FileSize = 64 * 1024 * 1024;


// H.264 NAL 단위 유형(nal_unit_type) 중 이 모듈이 구분하는 값.
enum class H264NalUnitType : std::uint8_t {
    // IDR이 아닌 슬라이스
    kNonIdrSlice = 1,

    // IDR 슬라이스. 이 프레임부터 앞 프레임 없이 디코딩할 수 있다.
    kIdrSlice = 5,

    // SEI(부가 정보)
    kSei = 6,

    // SPS(시퀀스 매개변수 집합)
    kSps = 7,

    // PPS(픽처 매개변수 집합)
    kPps = 8,

    // AUD(액세스 단위 구분자)
    kAccessUnitDelimiter = 9,
};


// NAL 단위의 유형 값을 꺼낸다.
//
// nal_unit: 시작 코드를 뺀 NAL 단위. 비어 있지 않아야 한다.
// 반환값: NAL 헤더 하위 5비트의 값
inline std::uint8_t NalUnitTypeOf(std::span<const std::uint8_t> nal_unit) { return nal_unit[0] & 0x1F; }


// 액세스 단위(프레임) 하나를 이루는 H.264 NAL 단위 목록.
//
// 파일, 카메라 등 소스의 형식과 무관하게 패킷화와 송신이 함께 쓰는 프레임 단위다. 각 뷰는 시작 코드를 뺀
// NAL 단위 하나(NAL 헤더 포함)이며, 프레임을 만든 소스가 소유한 버퍼를 가리키므로 그 소스가 살아 있는
// 동안만 유효하다.
struct H264Frame {
    // 디코딩 순서대로의 NAL 단위
    std::vector<std::span<const std::uint8_t>> nal_units;
};


// 프레임이 키프레임인지 확인한다.
//
// frame: 확인할 프레임
// 반환값: SPS, PPS, IDR 슬라이스를 모두 포함하면 true
//
// 앞 프레임 없이 디코딩을 시작할 수 있는 프레임의 기준이다. 파일의 첫 프레임 검증과 카메라 송신 시작에 함께 쓴다.
bool IsH264Keyframe(const H264Frame& frame);


// 시작할 때 전체를 읽어 프레임으로 나눈 Annex-B H.264 파일.
//
// 파일 바이트와 그 바이트를 가리키는 프레임 목록을 소유한다. std::vector는 이동해도 버퍼 주소가
// 바뀌지 않으므로 이동은 허용하고, 복사하면 새 객체의 뷰가 원래 객체의 버퍼를 가리키게 되므로 복사는
// 막는다. 모든 세션이 읽기 전용으로 공유한다.
class H264File {
public:
    // 파일을 읽어 프레임으로 나누고 검증한다.
    //
    // path: Annex-B 형식 H.264 파일 경로
    // 반환값: 읽은 파일. 열거나 읽을 수 없거나, 크기가 kMaxH264FileSize를 넘거나, FromBytes의 검증에
    //   실패하면 경로와 원인을 담은 오류 메시지
    static std::expected<H264File, std::string> Load(const std::filesystem::path& path);


    // 메모리의 Annex-B 바이트를 NAL 단위로 나누고 프레임으로 묶어 검증한다.
    //
    // bytes: 시작 코드(00 00 01 또는 00 00 00 01)로 구분된 NAL 단위. 소유권을 넘겨받는다.
    // 반환값: 나눈 파일. 다음 경우 원인 메시지
    //   - NAL 단위가 하나도 없음
    //   - 슬라이스가 있는 프레임이 하나도 없음
    //   - 첫 프레임에 SPS, PPS, IDR 슬라이스 중 하나가 없음
    //
    // 새 프레임은 이전 프레임에 슬라이스가 있는 상태에서 AUD·SPS·PPS·SEI나 first_mb_in_slice가 0인
    // 슬라이스가 올 때 시작한다. 슬라이스가 없는 끝 프레임은 버린다.
    static std::expected<H264File, std::string> FromBytes(std::vector<std::uint8_t> bytes);


    // 프레임이 바이트 버퍼를 가리키므로 복사를 금지한다.
    H264File(const H264File&) = delete;


    // 프레임이 바이트 버퍼를 가리키므로 복사 대입을 금지한다.
    H264File& operator=(const H264File&) = delete;


    // 버퍼 주소가 유지되므로 이동은 허용한다.
    H264File(H264File&&) = default;


    // 버퍼 주소가 유지되므로 이동 대입은 허용한다.
    H264File& operator=(H264File&&) = default;


    // 파일 순서대로의 프레임을 돌려준다.
    //
    // 반환값: 하나 이상인 프레임 목록. 첫 프레임은 SPS, PPS, IDR 슬라이스를 포함한다.
    //   목록과 각 NAL 뷰는 이 객체가 살아 있는 동안 유효하다.
    const std::vector<H264Frame>& Frames() const { return frames_; }


private:
    // 바이트와 그 바이트를 가리키는 프레임으로 객체를 만든다. FromBytes()에서만 호출한다.
    H264File(std::vector<std::uint8_t> bytes, std::vector<H264Frame> frames)
        : bytes_(std::move(bytes)), frames_(std::move(frames)) {}


    // 파일 전체 바이트. frames_의 뷰가 가리킨다.
    std::vector<std::uint8_t> bytes_;


    // bytes_를 나눈 프레임 목록
    std::vector<H264Frame> frames_;
};


}  // namespace vms::media
