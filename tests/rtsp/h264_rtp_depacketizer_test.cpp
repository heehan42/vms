#include "rtsp/h264_rtp_depacketizer.h"

#include <cstdint>
#include <span>
#include <vector>

#include "webrtc/rtp_test_support.h"
#include "test_support.h"
#include "webrtc/h264_packetizer.h"
#include "webrtc/rtp_packet.h"

using vms::rtsp::H264RtpDepacketizer;
using vms::test::Expect;
using vms::webrtc::BuildRtpPacket;
using vms::webrtc::RtpHeader;

using Bytes = std::vector<std::uint8_t>;


namespace {


// 테스트 스트림의 payload type
constexpr std::uint8_t kPayloadType = 96;


// 테스트 스트림의 SSRC
constexpr std::uint32_t kSsrc = 0xFAEC15D2;


// 재조립기가 알린 프레임 하나의 복사본.
struct ReceivedFrame {
    // NAL 단위 복사본
    std::vector<Bytes> nal_units;


    // 카메라 RTP 타임스탬프
    std::uint32_t timestamp = 0;


    // 키프레임 여부
    bool is_keyframe = false;
};


// 재조립기와 그 핸들러가 기록한 결과.
struct Harness {
    // 알린 프레임
    std::vector<ReceivedFrame> frames;


    // 알린 손실 수
    int loss_count = 0;


    // 시험 대상. 핸들러가 이 객체의 frames, loss_count를 가리키므로 Harness는 이동하지 않는다.
    H264RtpDepacketizer depacketizer{
        kPayloadType,
        [this](const vms::media::H264Frame& frame, std::uint32_t timestamp, bool is_keyframe) {
            ReceivedFrame received{.nal_units = {}, .timestamp = timestamp, .is_keyframe = is_keyframe};
            for (const auto nal_unit : frame.nal_units) {
                received.nal_units.emplace_back(nal_unit.begin(), nal_unit.end());
            }
            frames.push_back(std::move(received));
        },
        [this] { ++loss_count; }};


    // 헤더 값과 페이로드로 RTP 패킷을 만들어 넣는다.
    //
    // sequence_number: 순번
    // timestamp: 타임스탬프
    // marker: marker 비트
    // payload: 페이로드
    // ssrc: SSRC
    // payload_type: payload type
    void Feed(std::uint16_t sequence_number, std::uint32_t timestamp, bool marker, std::span<const std::uint8_t> payload,
              std::uint32_t ssrc = kSsrc, std::uint8_t payload_type = kPayloadType) {
        depacketizer.HandlePacket(BuildRtpPacket(RtpHeader{.marker = marker,
                                                           .payload_type = payload_type,
                                                           .sequence_number = sequence_number,
                                                           .timestamp = timestamp,
                                                           .ssrc = ssrc},
                                                 payload));
    }
};


// 헤더 바이트 뒤에 0이 아닌 본문을 채운 NAL을 만든다.
//
// header: NAL 헤더
// size: NAL 전체 크기
// 반환값: 만든 NAL
Bytes MakeNal(std::uint8_t header, std::size_t size) {
    Bytes nal_unit{header};
    for (std::size_t index = 1; index < size; ++index) {
        nal_unit.push_back(static_cast<std::uint8_t>(index % 251 + 1));
    }
    return nal_unit;
}


// NAL 하나를 FU-A 페이로드 조각으로 나눈다.
//
// nal_unit: 나눌 NAL
// fragment_count: 조각 수
// 반환값: 순서대로의 FU-A 페이로드
std::vector<Bytes> SplitIntoFuA(const Bytes& nal_unit, std::size_t fragment_count) {
    const std::size_t body_size = nal_unit.size() - 1;
    const std::size_t chunk = (body_size + fragment_count - 1) / fragment_count;
    std::vector<Bytes> payloads;
    for (std::size_t offset = 1; offset < nal_unit.size(); offset += chunk) {
        const std::size_t end = std::min(offset + chunk, nal_unit.size());
        std::uint8_t fu_header = nal_unit[0] & 0x1F;
        if (offset == 1) {
            fu_header |= 0x80;
        }
        if (end == nal_unit.size()) {
            fu_header |= 0x40;
        }
        Bytes payload{static_cast<std::uint8_t>((nal_unit[0] & 0xE0) | 28), fu_header};
        payload.insert(payload.end(), nal_unit.begin() + static_cast<std::ptrdiff_t>(offset),
                       nal_unit.begin() + static_cast<std::ptrdiff_t>(end));
        payloads.push_back(std::move(payload));
    }
    return payloads;
}


// NAL 목록을 STAP-A 페이로드 하나로 묶는다.
//
// nal_units: 묶을 NAL
// 반환값: STAP-A 페이로드
Bytes MakeStapA(const std::vector<Bytes>& nal_units) {
    Bytes payload{0x78};
    for (const Bytes& nal_unit : nal_units) {
        payload.push_back(static_cast<std::uint8_t>(nal_unit.size() >> 8));
        payload.push_back(static_cast<std::uint8_t>(nal_unit.size()));
        payload.insert(payload.end(), nal_unit.begin(), nal_unit.end());
    }
    return payload;
}


// 단일 NAL 슬라이스는 키프레임이 아니고, STAP-A(SPS·PPS)와 IDR은 세 NAL의 키프레임인지 검사한다.
void TestSingleNalAndStapA() {
    Harness harness;
    const Bytes sps = MakeNal(0x67, 8);
    const Bytes pps = MakeNal(0x68, 4);
    const Bytes idr = MakeNal(0x65, 500);
    const Bytes slice = MakeNal(0x41, 300);

    harness.Feed(10, 3000, false, MakeStapA({sps, pps}));
    harness.Feed(11, 3000, true, idr);
    harness.Feed(12, 6000, true, slice);

    Expect(harness.loss_count == 0, "손실 없음");
    if (harness.frames.size() != 2) {
        Expect(false, "프레임 두 개");
        return;
    }
    Expect(harness.frames[0].nal_units == std::vector<Bytes>{sps, pps, idr} && harness.frames[0].is_keyframe &&
               harness.frames[0].timestamp == 3000,
           "STAP-A와 IDR은 SPS, PPS, IDR 순서의 키프레임");
    Expect(harness.frames[1].nal_units == std::vector<Bytes>{slice} && !harness.frames[1].is_keyframe &&
               harness.frames[1].timestamp == 6000,
           "단일 NAL 슬라이스는 키프레임이 아님");
}


// 3000바이트 IDR을 FU-A 3조각으로 받으면 원래 NAL 하나로 재조립하는지 검사한다.
void TestFuAReassembly() {
    Harness harness;
    const Bytes idr = MakeNal(0x65, 3000);
    const std::vector<Bytes> fragments = SplitIntoFuA(idr, 3);
    for (std::size_t index = 0; index < fragments.size(); ++index) {
        harness.Feed(static_cast<std::uint16_t>(100 + index), 9000, index + 1 == fragments.size(), fragments[index]);
    }
    Expect(fragments.size() == 3 && harness.frames.size() == 1 && harness.frames[0].nal_units == std::vector<Bytes>{idr},
           "FU-A 3조각은 원래 NAL 하나");
}


// FU-A 가운데 조각이 빠지면 그 프레임을 버리고 손실을 한 번 알리며, 다음 프레임은 받는지 검사한다.
void TestMissingMiddleFragment() {
    Harness harness;
    const std::vector<Bytes> fragments = SplitIntoFuA(MakeNal(0x65, 3000), 3);
    harness.Feed(100, 9000, false, fragments[0]);
    harness.Feed(102, 9000, true, fragments[2]);
    Expect(harness.frames.empty() && harness.loss_count == 1, "가운데 조각이 빠진 프레임은 버리고 손실 한 번");

    const Bytes slice = MakeNal(0x41, 100);
    harness.Feed(103, 12000, true, slice);
    Expect(harness.frames.size() == 1 && harness.frames[0].nal_units == std::vector<Bytes>{slice},
           "손실 뒤 다음 프레임은 받음");
}


// marker 없이 타임스탬프가 바뀌면 앞 프레임을 내보내는지 검사한다.
void TestTimestampChangeEndsFrame() {
    Harness harness;
    const Bytes first = MakeNal(0x41, 100);
    const Bytes second = MakeNal(0x41, 120);
    harness.Feed(1, 3000, false, first);
    Expect(harness.frames.empty(), "marker 전에는 내보내지 않음");
    harness.Feed(2, 6000, true, second);
    Expect(harness.frames.size() == 2 && harness.frames[0].nal_units == std::vector<Bytes>{first} &&
               harness.frames[0].timestamp == 3000 && harness.loss_count == 0,
           "타임스탬프가 바뀌면 앞 프레임 완료");
}


// 순번 65535 다음 0은 연속으로 보는지 검사한다.
void TestSequenceNumberWrap() {
    Harness harness;
    harness.Feed(65534, 3000, true, MakeNal(0x41, 50));
    harness.Feed(65535, 6000, true, MakeNal(0x41, 50));
    harness.Feed(0, 9000, true, MakeNal(0x41, 50));
    Expect(harness.frames.size() == 3 && harness.loss_count == 0, "65535 다음 0은 손실이 아님");
}


// 다른 payload type의 패킷은 손실로 치지 않고 무시하는지 검사한다.
void TestIgnoresOtherPayloadType() {
    Harness harness;
    harness.Feed(1, 3000, true, MakeNal(0x41, 50));
    harness.Feed(2, 3000, true, MakeNal(0x41, 50), kSsrc, 97);
    harness.Feed(2, 6000, true, MakeNal(0x41, 50));
    Expect(harness.frames.size() == 2 && harness.loss_count == 0, "다른 payload type은 무시");
}


// SSRC가 바뀌면 손실을 알리고 새 스트림의 순번을 기준으로 다시 시작하는지 검사한다.
void TestSsrcChange() {
    Harness harness;
    harness.Feed(1, 3000, false, MakeNal(0x41, 50));
    harness.Feed(5000, 777, true, MakeNal(0x41, 60), kSsrc + 1);
    harness.Feed(5001, 3777, true, MakeNal(0x41, 70), kSsrc + 1);
    Expect(harness.loss_count == 1, "SSRC 변경은 손실 한 번");
    Expect(harness.frames.size() == 2 && harness.frames[0].timestamp == 777 && harness.frames[1].timestamp == 3777,
           "만들던 프레임은 버리고 새 스트림의 프레임을 받음");
}


// 4MiB를 넘는 프레임은 버리고 손실로 알리는지 검사한다.
void TestOversizedFrame() {
    Harness harness;
    const Bytes huge = MakeNal(0x65, vms::rtsp::kMaxDepacketizedFrameSize + 10);
    const std::vector<Bytes> fragments = SplitIntoFuA(huge, 80);
    for (std::size_t index = 0; index < fragments.size(); ++index) {
        harness.Feed(static_cast<std::uint16_t>(index), 3000, index + 1 == fragments.size(), fragments[index]);
    }
    Expect(harness.frames.empty() && harness.loss_count == 1, "4MiB 초과 프레임은 버리고 손실 한 번");

    harness.Feed(static_cast<std::uint16_t>(fragments.size()), 6000, true, MakeNal(0x41, 50));
    Expect(harness.frames.size() == 1, "다음 프레임은 받음");
}


// 서버의 PacketizeH264Frame으로 만든 패킷을 다시 재조립하면 원래 NAL 목록(AUD 제외)과 같은지 검사한다.
void TestRoundTripWithPacketizer() {
    Harness harness;
    const vms::media::H264File video = vms::test::MakeSyntheticH264File();
    std::uint16_t sequence_number = 40000;
    for (std::size_t index = 0; index < video.Frames().size(); ++index) {
        const auto payloads = vms::webrtc::PacketizeH264Frame(video.Frames()[index]);
        for (std::size_t packet_index = 0; packet_index < payloads.size(); ++packet_index) {
            harness.Feed(sequence_number++, static_cast<std::uint32_t>(index * 3000),
                         packet_index + 1 == payloads.size(), payloads[packet_index]);
        }
    }

    if (harness.frames.size() != video.Frames().size()) {
        Expect(false, "프레임 수가 같아야 한다");
        return;
    }
    for (std::size_t index = 0; index < video.Frames().size(); ++index) {
        Expect(harness.frames[index].nal_units == vms::test::TransmittedNalUnits(video.Frames()[index]),
               "재조립한 NAL 목록은 원래 프레임과 같음");
    }
    Expect(harness.frames[0].is_keyframe && !harness.frames[1].is_keyframe, "첫 프레임만 키프레임");
}


}  // namespace


int main() {
    TestSingleNalAndStapA();
    TestFuAReassembly();
    TestMissingMiddleFragment();
    TestTimestampChangeEndsFrame();
    TestSequenceNumberWrap();
    TestIgnoresOtherPayloadType();
    TestSsrcChange();
    TestOversizedFrame();
    TestRoundTripWithPacketizer();
    return vms::test::ExitCode();
}
