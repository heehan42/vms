#include "media/live_video_source.h"

#include <cstdint>
#include <vector>

#include "media/camera_list.h"
#include "test_support.h"

using vms::media::CameraList;
using vms::media::H264Frame;
using vms::media::LiveVideoEvent;
using vms::media::LiveVideoFrame;
using vms::media::LiveVideoSource;
using vms::test::Expect;


namespace {


// 핸들러가 기록할 필요가 없을 때 쓰는 빈 이벤트 핸들러
void IgnoreEvent(LiveVideoEvent) {}


// 구독자 둘이 같은 프레임을 받고, 해지한 구독자는 받지 않는지 검사한다.
void TestFanOutAndUnsubscribe() {
    LiveVideoSource source;
    std::vector<std::uint32_t> first_received;
    std::vector<std::uint32_t> second_received;
    const auto first = source.Subscribe(
        [&](const LiveVideoFrame& frame) { first_received.push_back(frame.rtp_timestamp); }, IgnoreEvent);
    source.Subscribe([&](const LiveVideoFrame& frame) { second_received.push_back(frame.rtp_timestamp); },
                     IgnoreEvent);

    const H264Frame frame;
    source.PublishFrame(LiveVideoFrame{.frame = frame, .rtp_timestamp = 3000, .is_keyframe = true});
    source.Unsubscribe(first);
    source.Unsubscribe(first);
    source.PublishFrame(LiveVideoFrame{.frame = frame, .rtp_timestamp = 6000, .is_keyframe = false});

    Expect(first_received == std::vector<std::uint32_t>{3000}, "해지한 구독자는 해지 뒤 프레임을 받지 않음");
    Expect(second_received == std::vector<std::uint32_t>{3000, 6000}, "남은 구독자는 모두 받음");
}


// 핸들러 안에서 자기 자신과 뒤의 구독자를 해지해도 안전하고, 해지된 뒤의 구독자는 호출되지 않는지 검사한다.
void TestUnsubscribeInsideHandler() {
    LiveVideoSource source;
    int first_count = 0;
    int second_count = 0;
    int third_count = 0;
    LiveVideoSource::SubscriptionId first = 0;
    LiveVideoSource::SubscriptionId second = 0;
    first = source.Subscribe(
        [&](const LiveVideoFrame&) {
            ++first_count;
            source.Unsubscribe(first);
            source.Unsubscribe(second);
        },
        IgnoreEvent);
    second = source.Subscribe([&](const LiveVideoFrame&) { ++second_count; }, IgnoreEvent);
    source.Subscribe([&](const LiveVideoFrame&) { ++third_count; }, IgnoreEvent);

    const H264Frame frame;
    source.PublishFrame(LiveVideoFrame{.frame = frame, .rtp_timestamp = 0, .is_keyframe = false});
    source.PublishFrame(LiveVideoFrame{.frame = frame, .rtp_timestamp = 1, .is_keyframe = false});
    Expect(first_count == 1, "자기 자신을 해지한 구독자는 한 번만 받음");
    Expect(second_count == 0, "같은 전달 중 앞에서 해지된 구독자는 받지 않음");
    Expect(third_count == 2, "나머지 구독자는 계속 받음");
}


// IsConnected()가 연결 이벤트를 따라가고, 이벤트가 구독자에게 전달되며, 손실은 연결 상태를 바꾸지 않는지 검사한다.
void TestConnectionEvents() {
    LiveVideoSource source;
    Expect(!source.IsConnected(), "처음에는 연결되지 않음");

    std::vector<LiveVideoEvent> events;
    std::vector<bool> connected_at_event;
    source.Subscribe([](const LiveVideoFrame&) {},
                     [&](LiveVideoEvent event) {
                         events.push_back(event);
                         connected_at_event.push_back(source.IsConnected());
                     });
    source.PublishEvent(LiveVideoEvent::kConnected);
    source.PublishEvent(LiveVideoEvent::kLoss);
    source.PublishEvent(LiveVideoEvent::kDisconnected);

    Expect(events == std::vector{LiveVideoEvent::kConnected, LiveVideoEvent::kLoss, LiveVideoEvent::kDisconnected},
           "이벤트를 순서대로 전달");
    Expect(connected_at_event == std::vector<bool>{true, true, false}, "알리기 전에 연결 상태를 바꿈");
    Expect(!source.IsConnected(), "끊긴 뒤 연결되지 않음");
}


// 카메라 목록이 등록 순서를 지키고 이름으로 찾으며, 소스 주소가 추가 뒤에도 유지되는지 검사한다.
void TestCameraList() {
    CameraList cameras;
    LiveVideoSource* const front = &cameras.Add("front", "rtsp://127.0.0.1:8554/cam").source;
    for (int index = 0; index < 20; ++index) {
        cameras.Add("cam" + std::to_string(index), "rtsp://127.0.0.1/cam");
    }
    Expect(cameras.Entries().size() == 21 && cameras.Entries().front()->name == "front", "등록 순서");
    Expect(cameras.Find("front") != nullptr && &cameras.Find("front")->source == front, "추가 뒤에도 소스 주소 유지");
    Expect(cameras.Find("missing") == nullptr, "없는 이름");
}


}  // namespace


int main() {
    TestFanOutAndUnsubscribe();
    TestUnsubscribeInsideHandler();
    TestConnectionEvents();
    TestCameraList();
    return vms::test::ExitCode();
}
