#include "media/h264_file.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "test_support.h"

using vms::media::H264File;
using vms::media::H264Frame;
using vms::test::Expect;

namespace fs = std::filesystem;


namespace {


// 시작 코드를 뺀 합성 NAL 단위. 첫 바이트가 NAL 헤더다.
using NalUnit = std::vector<std::uint8_t>;


// SPS(유형 7)
const NalUnit kSps = {0x67, 0x42, 0xC0, 0x1F, 0xDA};


// PPS(유형 8)
const NalUnit kPps = {0x68, 0xCE, 0x3C, 0x80};


// first_mb_in_slice가 0인 IDR 슬라이스(유형 5). 두 번째 바이트의 최상위 비트가 1이다.
const NalUnit kIdrSlice = {0x65, 0x88, 0x84, 0x21, 0x10};


// first_mb_in_slice가 0인 일반 슬라이스(유형 1)
const NalUnit kSlice = {0x41, 0x9A, 0x02, 0x03};


// first_mb_in_slice가 0이 아닌 일반 슬라이스. 두 번째 바이트의 최상위 비트가 0이다.
const NalUnit kContinuationSlice = {0x41, 0x40, 0x07, 0x08};


// SEI(유형 6)
const NalUnit kSei = {0x06, 0x05, 0x01, 0xFF, 0x80};


// AUD(유형 9)
const NalUnit kAud = {0x09, 0xF0};


// NAL 단위를 시작 코드로 이어 붙인다.
//
// nal_units: 이어 붙일 NAL 단위
// use_long_start_code: NAL마다 4바이트 시작 코드를 쓸지 여부. 0번째부터 순서대로 대응하며, 없으면 3바이트
// 반환값: Annex-B 바이트
std::vector<std::uint8_t> JoinAnnexB(std::initializer_list<NalUnit> nal_units,
                                     std::initializer_list<bool> use_long_start_code = {}) {
    std::vector<std::uint8_t> bytes;
    std::size_t index = 0;
    for (const NalUnit& nal_unit : nal_units) {
        const bool is_long = index < use_long_start_code.size() && use_long_start_code.begin()[index];
        if (is_long) {
            bytes.push_back(0x00);
        }
        bytes.insert(bytes.end(), {0x00, 0x00, 0x01});
        bytes.insert(bytes.end(), nal_unit.begin(), nal_unit.end());
        ++index;
    }
    return bytes;
}


// 프레임의 NAL 단위가 기대한 NAL 단위와 같은지 확인한다.
//
// frame: 검사할 프레임
// expected_nal_units: 기대하는 NAL 단위 목록
// 반환값: 개수와 각 내용이 모두 같으면 true
bool FrameEquals(const H264Frame& frame, std::initializer_list<NalUnit> expected_nal_units) {
    if (frame.nal_units.size() != expected_nal_units.size()) {
        return false;
    }
    return std::ranges::equal(frame.nal_units, expected_nal_units,
                              [](std::span<const std::uint8_t> actual, const NalUnit& expected) {
                                  return std::ranges::equal(actual, expected);
                              });
}


// 3바이트와 4바이트 시작 코드가 섞여도 NAL 경계와 내용이 맞고, 끝의 0 바이트는 NAL에서 빠지는지 검사한다.
void TestSplitsMixedStartCodes() {
    std::vector<std::uint8_t> bytes = JoinAnnexB({kSps, kPps, kIdrSlice}, {true, false, true});
    // 시작 코드 앞의 쓰레기 바이트와 마지막 NAL 뒤의 trailing zero는 버려야 한다.
    bytes.insert(bytes.begin(), {0xAB, 0xCD});
    bytes.insert(bytes.end(), {0x00, 0x00});

    const auto file = H264File::FromBytes(std::move(bytes));
    if (!file) {
        Expect(false, "섞인 시작 코드 데이터가 읽혀야 한다");
        return;
    }
    Expect(file->Frames().size() == 1, "SPS, PPS, IDR은 프레임 하나여야 한다");
    Expect(FrameEquals(file->Frames()[0], {kSps, kPps, kIdrSlice}),
           "NAL 내용은 시작 코드와 끝 0 바이트를 빼야 한다");
}


// SPS, PPS, IDR, 슬라이스, 슬라이스는 프레임 3개가 된다.
void TestGroupsParameterSetsAndSlices() {
    const auto file = H264File::FromBytes(JoinAnnexB({kSps, kPps, kIdrSlice, kSlice, kSlice}));
    if (!file) {
        Expect(false, "정상 데이터가 읽혀야 한다");
        return;
    }
    const auto& frames = file->Frames();
    Expect(frames.size() == 3, "프레임은 3개여야 한다");
    Expect(frames.size() == 3 && FrameEquals(frames[0], {kSps, kPps, kIdrSlice}) &&
               FrameEquals(frames[1], {kSlice}) && FrameEquals(frames[2], {kSlice}),
           "첫 프레임은 SPS·PPS·IDR, 나머지는 슬라이스 하나씩");
}


// first_mb_in_slice가 0이 아닌 슬라이스는 앞 프레임에 속한다.
void TestContinuationSliceJoinsFrame() {
    const auto file =
        H264File::FromBytes(JoinAnnexB({kSps, kPps, kIdrSlice, kSlice, kContinuationSlice, kSlice}));
    if (!file) {
        Expect(false, "여러 슬라이스 데이터가 읽혀야 한다");
        return;
    }
    const auto& frames = file->Frames();
    Expect(frames.size() == 3 && FrameEquals(frames[1], {kSlice, kContinuationSlice}),
           "이어지는 슬라이스는 같은 프레임이어야 한다");
}


// SEI와 AUD는 새 프레임을 시작하고, 끝의 슬라이스 없는 프레임은 버린다.
void TestSeiAndAudStartFrames() {
    const auto file = H264File::FromBytes(
        JoinAnnexB({kAud, kSps, kPps, kIdrSlice, kSei, kSlice, kAud, kSlice, kSps, kPps}));
    if (!file) {
        Expect(false, "SEI·AUD가 있는 데이터가 읽혀야 한다");
        return;
    }
    const auto& frames = file->Frames();
    Expect(frames.size() == 3, "끝의 SPS·PPS만 있는 프레임은 버려 3개여야 한다");
    Expect(frames.size() == 3 && FrameEquals(frames[0], {kAud, kSps, kPps, kIdrSlice}) &&
               FrameEquals(frames[1], {kSei, kSlice}) && FrameEquals(frames[2], {kAud, kSlice}),
           "AUD와 SEI가 새 프레임의 시작이어야 한다");
}


// 시작 코드가 없거나 첫 프레임 조건을 만족하지 않으면 원인을 알린다.
void TestRejectsInvalidData() {
    const std::string_view text = "this is not an h264 stream\n";
    const auto no_start_code = H264File::FromBytes(std::vector<std::uint8_t>(text.begin(), text.end()));
    Expect(!no_start_code && no_start_code.error().contains("NAL 단위가 없음"), "시작 코드 없음");

    const auto no_idr = H264File::FromBytes(JoinAnnexB({kSps, kPps, kSlice, kSlice}));
    Expect(!no_idr && no_idr.error().contains("IDR"), "첫 프레임에 IDR 없음");

    const auto no_sps = H264File::FromBytes(JoinAnnexB({kPps, kIdrSlice, kSlice}));
    Expect(!no_sps && no_sps.error().contains("SPS"), "첫 프레임에 SPS 없음");

    const auto no_slice = H264File::FromBytes(JoinAnnexB({kSps, kPps}));
    Expect(!no_slice && no_slice.error().contains("슬라이스"), "슬라이스 없음");
}


// 이동한 객체의 프레임이 계속 같은 버퍼를 가리키는지 검사한다.
void TestMovePreservesFrames() {
    auto file = H264File::FromBytes(JoinAnnexB({kSps, kPps, kIdrSlice}));
    if (!file) {
        Expect(false, "정상 데이터가 읽혀야 한다");
        return;
    }
    const H264File moved = *std::move(file);
    Expect(FrameEquals(moved.Frames()[0], {kSps, kPps, kIdrSlice}), "이동 뒤에도 프레임 내용이 같아야 한다");
}


// 임시 파일로 정상 읽기, 없는 경로, 크기 상한을 넘는 파일을 검사한다.
//
// directory: 테스트 파일을 만들 임시 디렉터리
void TestLoadFromFile(const fs::path& directory) {
    const fs::path valid_path = directory / "valid.h264";
    const std::vector<std::uint8_t> bytes = JoinAnnexB({kSps, kPps, kIdrSlice, kSlice}, {true, true, true, true});
    std::ofstream(valid_path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    const auto valid = H264File::Load(valid_path);
    Expect(valid && valid->Frames().size() == 2, "파일에서 프레임 2개를 읽어야 한다");

    const auto missing = H264File::Load(directory / "missing.h264");
    Expect(!missing && missing.error().contains("missing.h264"), "없는 파일은 경로를 담은 오류여야 한다");

    // 실제로 디스크를 쓰지 않도록 크기만 늘린 희소 파일을 만든다.
    const fs::path large_path = directory / "large.h264";
    const int large_file = ::open(large_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    const bool is_large_file_ready =
        large_file >= 0 && ::ftruncate(large_file, static_cast<off_t>(vms::media::kMaxH264FileSize + 1)) == 0;
    if (large_file >= 0) {
        ::close(large_file);
    }
    Expect(is_large_file_ready, "희소 파일 생성");
    const auto large = H264File::Load(large_path);
    Expect(!large && large.error().contains("너무 큼"), "64MiB를 넘는 파일은 크기 오류여야 한다");
}


}  // namespace


int main() {
    TestSplitsMixedStartCodes();
    TestGroupsParameterSetsAndSlices();
    TestContinuationSliceJoinsFrame();
    TestSeiAndAudStartFrames();
    TestRejectsInvalidData();
    TestMovePreservesFrames();

    std::error_code error;
    std::string directory_template = (fs::temp_directory_path(error) / "h264_file_test.XXXXXX").string();
    if (error || ::mkdtemp(directory_template.data()) == nullptr) {
        Expect(false, "임시 디렉터리 생성");
        return vms::test::ExitCode();
    }
    const fs::path directory = directory_template;
    TestLoadFromFile(directory);
    fs::remove_all(directory, error);
    return vms::test::ExitCode();
}
