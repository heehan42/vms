#include "media/h264_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <format>
#include <optional>
#include <utility>

#include "core/system_error.h"
#include "core/unique_fd.h"


namespace vms::media {


namespace {


// 시작 코드로 구분된 바이트를 NAL 단위로 나눈다.
//
// bytes: Annex-B 바이트
// 반환값: 시작 코드를 뺀 NAL 단위 뷰. bytes를 가리킨다. 첫 시작 코드 앞의 바이트와 빈 NAL은 버린다.
//
// 3바이트 시작 코드 00 00 01을 경계로 찾는다. 4바이트 시작 코드의 앞 00과 NAL 끝의 0 바이트
// (trailing_zero_8bits)는 NAL 본문의 끝이 0일 수 없다는 규칙에 따라 앞 NAL의 끝에서 잘라 낸다.
std::vector<std::span<const std::uint8_t>> SplitNalUnits(std::span<const std::uint8_t> bytes) {
    std::vector<std::span<const std::uint8_t>> nal_units;
    std::optional<std::size_t> nal_start;
    const auto finish_nal = [&](std::size_t end) {
        if (!nal_start) {
            return;
        }
        while (end > *nal_start && bytes[end - 1] == 0) {
            --end;
        }
        if (end > *nal_start) {
            nal_units.push_back(bytes.subspan(*nal_start, end - *nal_start));
        }
    };

    std::size_t position = 0;
    while (position + 3 <= bytes.size()) {
        if (bytes[position] == 0 && bytes[position + 1] == 0 && bytes[position + 2] == 1) {
            finish_nal(position);
            position += 3;
            nal_start = position;
            continue;
        }
        ++position;
    }
    finish_nal(bytes.size());
    return nal_units;
}


// NAL 단위가 슬라이스(유형 1 또는 5)인지 확인한다.
bool IsSlice(std::uint8_t nal_unit_type) {
    return nal_unit_type == std::to_underlying(H264NalUnitType::kNonIdrSlice) ||
           nal_unit_type == std::to_underlying(H264NalUnitType::kIdrSlice);
}


// 슬라이스가 픽처의 첫 슬라이스(first_mb_in_slice가 0)인지 확인한다.
//
// slice: 슬라이스 NAL 단위
// 반환값: NAL 헤더 다음 비트열이 1로 시작하면 true
//
// first_mb_in_slice는 슬라이스 헤더의 첫 ue(v) 값이고, 값 0의 ue(v) 부호는 비트 1 하나다.
// 첫 바이트에는 에뮬레이션 방지 바이트가 올 수 없으므로 최상위 비트만 보면 된다.
bool IsFirstSliceOfPicture(std::span<const std::uint8_t> slice) {
    return slice.size() >= 2 && (slice[1] & 0x80) != 0;
}


// NAL 단위가 이전 프레임에 슬라이스가 있을 때 새 프레임을 시작하는지 확인한다.
//
// nal_unit: 판정할 NAL 단위
// 반환값: AUD, SPS, PPS, SEI이거나 픽처의 첫 슬라이스이면 true
bool StartsNewFrame(std::span<const std::uint8_t> nal_unit) {
    const std::uint8_t type = NalUnitTypeOf(nal_unit);
    switch (static_cast<H264NalUnitType>(type)) {
        case H264NalUnitType::kAccessUnitDelimiter:
        case H264NalUnitType::kSps:
        case H264NalUnitType::kPps:
        case H264NalUnitType::kSei:
            return true;
        case H264NalUnitType::kNonIdrSlice:
        case H264NalUnitType::kIdrSlice:
            return IsFirstSliceOfPicture(nal_unit);
    }
    return false;
}


// NAL 단위를 프레임으로 묶는다.
//
// nal_units: 파일 순서대로의 NAL 단위
// 반환값: 슬라이스가 하나 이상 있는 프레임 목록. 슬라이스가 없는 끝 프레임은 버린다.
//
// 경계 판정에 쓰지 않는 유형(필러 데이터 등)은 현재 프레임에 넣는다.
std::vector<H264Frame> GroupFrames(const std::vector<std::span<const std::uint8_t>>& nal_units) {
    std::vector<H264Frame> frames;
    H264Frame current_frame;
    bool current_frame_has_slice = false;
    for (const std::span<const std::uint8_t> nal_unit : nal_units) {
        if (current_frame_has_slice && StartsNewFrame(nal_unit)) {
            frames.push_back(std::move(current_frame));
            current_frame = H264Frame{};
            current_frame_has_slice = false;
        }
        current_frame.nal_units.push_back(nal_unit);
        current_frame_has_slice = current_frame_has_slice || IsSlice(NalUnitTypeOf(nal_unit));
    }
    if (current_frame_has_slice) {
        frames.push_back(std::move(current_frame));
    }
    return frames;
}


// 프레임에 주어진 유형의 NAL 단위가 있는지 확인한다.
bool ContainsNalUnitType(const H264Frame& frame, H264NalUnitType type) {
    return std::ranges::any_of(frame.nal_units, [type](std::span<const std::uint8_t> nal_unit) {
        return NalUnitTypeOf(nal_unit) == std::to_underlying(type);
    });
}


}  // namespace


std::expected<H264File, std::string> H264File::Load(const std::filesystem::path& path) {
    const core::UniqueFd file(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!file.IsValid()) {
        return std::unexpected(
            std::format("동영상 파일을 열 수 없음 ({}): {}", path.string(), core::LastSystemError().message()));
    }

    // 연 파일의 크기를 확인해, 확인한 뒤 경로가 다른 파일로 바뀌어도 읽는 파일과 크기가 일치하게 한다.
    struct stat file_status{};
    if (::fstat(file.Get(), &file_status) != 0) {
        return std::unexpected(std::format("동영상 파일 정보를 읽을 수 없음 ({}): {}", path.string(),
                                           core::LastSystemError().message()));
    }
    if (!S_ISREG(file_status.st_mode)) {
        return std::unexpected(std::format("동영상 파일이 일반 파일이 아님 ({})", path.string()));
    }
    const auto file_size = static_cast<std::uintmax_t>(file_status.st_size);
    if (file_size > kMaxH264FileSize) {
        return std::unexpected(std::format("동영상 파일이 너무 큼 ({}): {}바이트, 최대 {}바이트", path.string(),
                                           file_size, kMaxH264FileSize));
    }

    // 읽는 동안 파일이 줄어들면 실제로 읽은 만큼만 쓴다.
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_size));
    std::size_t read_total = 0;
    while (read_total < bytes.size()) {
        const ssize_t read_size = ::read(file.Get(), bytes.data() + read_total, bytes.size() - read_total);
        if (read_size < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(std::format("동영상 파일을 읽을 수 없음 ({}): {}", path.string(),
                                               core::LastSystemError().message()));
        }
        if (read_size == 0) {
            break;
        }
        read_total += static_cast<std::size_t>(read_size);
    }
    bytes.resize(read_total);

    auto loaded = FromBytes(std::move(bytes));
    if (!loaded) {
        return std::unexpected(std::format("동영상 파일 형식 오류 ({}): {}", path.string(), loaded.error()));
    }
    return loaded;
}


std::expected<H264File, std::string> H264File::FromBytes(std::vector<std::uint8_t> bytes) {
    const std::vector<std::span<const std::uint8_t>> nal_units = SplitNalUnits(bytes);
    if (nal_units.empty()) {
        return std::unexpected("H.264 NAL 단위가 없음(시작 코드 00 00 01을 찾지 못함)");
    }

    std::vector<H264Frame> frames = GroupFrames(nal_units);
    if (frames.empty()) {
        return std::unexpected("슬라이스가 있는 프레임이 없음");
    }

    // 브라우저가 첫 프레임부터 디코딩하려면 매개변수 집합과 IDR이 모두 있어야 한다. 반복 재생도
    // 첫 프레임으로 돌아가므로 같은 조건이 반복 시작점을 보장한다.
    const H264Frame& first_frame = frames.front();
    if (!ContainsNalUnitType(first_frame, H264NalUnitType::kSps)) {
        return std::unexpected("첫 프레임에 SPS가 없음");
    }
    if (!ContainsNalUnitType(first_frame, H264NalUnitType::kPps)) {
        return std::unexpected("첫 프레임에 PPS가 없음");
    }
    if (!ContainsNalUnitType(first_frame, H264NalUnitType::kIdrSlice)) {
        return std::unexpected("첫 프레임에 IDR 슬라이스가 없음");
    }
    return H264File(std::move(bytes), std::move(frames));
}


}  // namespace vms::media
