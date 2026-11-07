#include "recording/recording_file_name.h"

#include <cstdint>

#include "test_support.h"

using vms::recording::FormatRecordingFileStem;
using vms::recording::ParseRecordingFileName;
using vms::recording::RecordingFileKind;
using vms::recording::SegmentNumberOf;
using vms::test::Expect;


namespace {


// 2026-11-04 10:01:05 UTC의 유닉스 초
constexpr std::int64_t kSampleSeconds = 1793786465;


// 시각 → 이름 → 시각 왕복과 초 아래 내림을 검사한다.
void TestRoundTrip() {
    const std::int64_t start_ms = kSampleSeconds * 1000 + 789;
    Expect(FormatRecordingFileStem(start_ms, 0) == "20261104T100105Z", "초 단위 이름");

    const auto parsed = ParseRecordingFileName("20261104T100105Z.mp4");
    Expect(parsed.has_value(), "규칙에 맞는 .mp4 이름 해석");
    if (!parsed) {
        return;
    }
    Expect(parsed->start_unix_ms == kSampleSeconds * 1000, "이름의 시각은 초 아래를 내린 값");
    Expect(parsed->collision_number == 0, "번호 없음");
    Expect(parsed->kind == RecordingFileKind::kVideo, ".mp4는 녹화 파일");
    Expect(parsed->stem == "20261104T100105Z", "확장자를 뺀 이름");
    Expect(FormatRecordingFileStem(parsed->start_unix_ms, 0) == parsed->stem, "다시 만들면 같은 이름");

    Expect(FormatRecordingFileStem(0, 0) == "19700101T000000Z", "유닉스 시각 0");
}


// 충돌 번호 접미사의 생성과 해석을 검사한다.
void TestCollisionSuffix() {
    Expect(FormatRecordingFileStem(kSampleSeconds * 1000, 1) == "20261104T100105Z-1", "-1 접미사 생성");
    Expect(FormatRecordingFileStem(kSampleSeconds * 1000, 12) == "20261104T100105Z-12", "-12 접미사 생성");

    const auto first = ParseRecordingFileName("20261104T100105Z-1.idx");
    Expect(first && first->collision_number == 1 && first->kind == RecordingFileKind::kIndex,
           "-1 접미사의 .idx 해석");
    const auto twelfth = ParseRecordingFileName("20261104T100105Z-12.mp4");
    Expect(twelfth && twelfth->collision_number == 12 && twelfth->stem == "20261104T100105Z-12",
           "-12 접미사 해석");
}


// 규칙에 맞지 않는 이름을 거부하는지 검사한다.
void TestRejectInvalidNames() {
    const char* const invalid_names[] = {
        "20261104T100105Z.txt",   "20261104T100105Z",       "20261104T100105.mp4",   "20261104X100105Z.mp4",
        "2026110T100105Z.mp4",    "20261304T100105Z.mp4",   "20260230T100105Z.mp4",  "20261104T240105Z.mp4",
        "20261104T106005Z.mp4",   "20261104T100160Z.mp4",   "20261104T100105Z-0.mp4", "20261104T100105Z-01.mp4",
        "20261104T100105Z-.mp4",  "20261104T100105Z-a.mp4", "20261104T100105Z_1.mp4", "2026a104T100105Z.mp4",
        ".mp4",
    };
    for (const char* name : invalid_names) {
        Expect(!ParseRecordingFileName(name).has_value(), name);
    }
}


// 단위 구간 번호를 경계 직전·직후와 단위 60·3600으로 검사한다.
void TestSegmentNumber() {
    const std::int64_t minute_boundary_ms = 1793786460LL * 1000;  // 10:01:00
    Expect(SegmentNumberOf(minute_boundary_ms - 1, 60) == 1793786460 / 60 - 1, "단위 60 경계 직전");
    Expect(SegmentNumberOf(minute_boundary_ms, 60) == 1793786460 / 60, "단위 60 경계");
    Expect(SegmentNumberOf(minute_boundary_ms + 59999, 60) == 1793786460 / 60, "단위 60 다음 경계 직전");

    const std::int64_t hour_boundary_ms = 1793786400LL * 1000;  // 10:00:00
    Expect(SegmentNumberOf(hour_boundary_ms - 1, 3600) == 1793786400 / 3600 - 1, "단위 3600 경계 직전");
    Expect(SegmentNumberOf(hour_boundary_ms, 3600) == 1793786400 / 3600, "단위 3600 경계");
    Expect(SegmentNumberOf(minute_boundary_ms, 3600) == 1793786400 / 3600, "단위 3600 안의 시각");

    Expect(SegmentNumberOf(-1, 60) == -1, "1970년 이전 시각은 내림");
}


}  // namespace


int main() {
    TestRoundTrip();
    TestCollisionSuffix();
    TestRejectInvalidNames();
    TestSegmentNumber();
    return vms::test::ExitCode();
}
