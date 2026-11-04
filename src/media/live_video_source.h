#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "media/h264_file.h"


namespace vms::media {


// 실시간 소스가 구독자에게 넘기는 프레임 하나.
//
// frame의 NAL 뷰는 소스에 프레임을 넣은 쪽(재조립기)의 버퍼를 가리키므로 Publish 호출 동안만 유효하다. 구독자는
// 핸들러 안에서 패킷화를 끝내야 하며 뷰를 보관하지 않는다.
struct LiveVideoFrame {
    // 재조립한 프레임
    const H264Frame& frame;


    // 카메라 RTP 타임스탬프(90kHz). 한 카메라 연결 안에서 연속이며, 다시 연결하면 원점이 바뀔 수 있다.
    std::uint32_t rtp_timestamp;


    // SPS, PPS, IDR 슬라이스를 모두 포함하면 true
    bool is_keyframe;
};


// 실시간 소스의 상태 변화.
enum class LiveVideoEvent {
    // 카메라에 연결되어 RTP를 받기 시작함. 이 뒤의 타임스탬프는 이전 연결과 이어지지 않을 수 있다.
    kConnected,

    // 카메라 연결이 끊김
    kDisconnected,

    // 연결은 유지되지만 패킷이 빠져 만들던 프레임을 버림. 다음 키프레임까지 디코딩할 수 없다.
    kLoss,
};


// 카메라 하나의 프레임과 상태 변화를 구독자 여럿에게 나눠 주는 실시간 소스.
//
// 프레임을 미리 갖고 있지 않고, 넣는 쪽(RTSP 클라이언트)이 PublishFrame·PublishEvent를 부르면 그 자리에서 구독자의
// 핸들러를 구독 순서대로 동기로 호출한다. 모든 호출은 이벤트 루프 스레드 하나에서 일어난다. 핸들러 안에서 자기
// 자신이나 다른 구독자를 해지해도 안전하며, 해지된 구독자는 그 뒤로 호출되지 않는다. 핸들러 안에서 새로 구독한
// 구독자는 진행 중인 전달을 받지 않는다. 구독자가 이 객체를 참조하므로 구독자보다 오래 살아 있어야 한다.
class LiveVideoSource {
public:
    // 구독을 가리키는 ID. 1부터 1씩 늘어나며 재사용하지 않는다.
    using SubscriptionId = std::uint64_t;


    // 프레임을 받는 핸들러. 인자의 뷰는 호출 동안만 유효하다.
    using FrameHandler = std::move_only_function<void(const LiveVideoFrame& frame)>;


    // 상태 변화를 받는 핸들러.
    using EventHandler = std::move_only_function<void(LiveVideoEvent event)>;


    // 연결되지 않은 상태의 소스를 만든다.
    LiveVideoSource() = default;


    // 구독자가 주소를 참조하므로 복사를 금지한다.
    LiveVideoSource(const LiveVideoSource&) = delete;


    // 구독자가 주소를 참조하므로 복사 대입을 금지한다.
    LiveVideoSource& operator=(const LiveVideoSource&) = delete;


    // 구독자를 추가한다.
    //
    // frame_handler: 프레임마다 호출할 핸들러
    // event_handler: 상태 변화마다 호출할 핸들러
    // 반환값: 해지에 쓸 구독 ID
    //
    // 핸들러는 Unsubscribe할 때까지 이 객체가 소유한다. 구독자는 소멸하기 전에 반드시 해지해야 한다.
    SubscriptionId Subscribe(FrameHandler frame_handler, EventHandler event_handler);


    // 구독을 해지한다. 없거나 이미 해지한 ID이면 아무것도 하지 않는다.
    //
    // id: Subscribe가 돌려준 구독 ID
    //
    // 핸들러 안에서 불러도 된다. 실행 중인 핸들러 객체는 그 호출이 끝난 뒤 파괴된다.
    void Unsubscribe(SubscriptionId id);


    // 프레임을 모든 구독자에게 넘긴다.
    //
    // frame: 넘길 프레임. 호출 동안만 참조한다.
    void PublishFrame(const LiveVideoFrame& frame);


    // 상태 변화를 기록하고 모든 구독자에게 알린다.
    //
    // event: 알릴 상태 변화. kConnected·kDisconnected는 IsConnected()를 먼저 바꾼 뒤 알린다.
    void PublishEvent(LiveVideoEvent event);


    // 카메라에 연결되어 있는지 확인한다.
    //
    // 반환값: 마지막 연결 상태 변화가 kConnected이면 true. 처음에는 false
    bool IsConnected() const { return is_connected_; }


    // 구독 중인 구독자 수를 돌려준다. 구독 해지를 확인하는 데 쓴다.
    std::size_t SubscriberCount() const { return subscribers_.size(); }


private:
    // 구독자 하나의 핸들러와 해지 여부.
    //
    // 전달 도중 해지되어 테이블에서 빠져도 실행 중인 핸들러가 파괴되지 않도록, 전달은 shared_ptr 사본을 잡고 한다.
    struct Subscriber {
        // 프레임 핸들러
        FrameHandler frame_handler;


        // 상태 변화 핸들러
        EventHandler event_handler;


        // Unsubscribe로 해지되어 더 호출하면 안 되는지 여부
        bool is_unsubscribed = false;
    };


    // 전달할 대상을 고정한 사본을 만든다.
    //
    // 반환값: 지금 구독 중인 구독자. 구독 순서대로다.
    std::vector<std::shared_ptr<Subscriber>> SnapshotSubscribers() const;


    // 구독 테이블. 키는 구독 ID이므로 순회 순서가 구독 순서다.
    std::map<SubscriptionId, std::shared_ptr<Subscriber>> subscribers_;


    // 마지막으로 발급한 구독 ID. 0은 발급하지 않는다.
    SubscriptionId last_subscription_id_ = 0;


    // 카메라 연결 상태
    bool is_connected_ = false;
};


}  // namespace vms::media
