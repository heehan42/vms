#include "media/live_video_source.h"

#include <utility>
#include <vector>


namespace vms::media {


LiveVideoSource::SubscriptionId LiveVideoSource::Subscribe(FrameHandler frame_handler, EventHandler event_handler) {
    const SubscriptionId id = ++last_subscription_id_;
    subscribers_.emplace(id, std::make_shared<Subscriber>(Subscriber{.frame_handler = std::move(frame_handler),
                                                                     .event_handler = std::move(event_handler),
                                                                     .is_unsubscribed = false}));
    return id;
}


void LiveVideoSource::Unsubscribe(SubscriptionId id) {
    const auto found = subscribers_.find(id);
    if (found == subscribers_.end()) {
        return;
    }
    found->second->is_unsubscribed = true;
    subscribers_.erase(found);
}


void LiveVideoSource::PublishFrame(const LiveVideoFrame& frame) {
    for (const std::shared_ptr<Subscriber>& subscriber : SnapshotSubscribers()) {
        // 앞선 구독자의 핸들러가 이 구독자를 해지했을 수 있다.
        if (!subscriber->is_unsubscribed) {
            subscriber->frame_handler(frame);
        }
    }
}


void LiveVideoSource::PublishEvent(LiveVideoEvent event) {
    if (event == LiveVideoEvent::kConnected || event == LiveVideoEvent::kDisconnected) {
        is_connected_ = event == LiveVideoEvent::kConnected;
    }
    for (const std::shared_ptr<Subscriber>& subscriber : SnapshotSubscribers()) {
        if (!subscriber->is_unsubscribed) {
            subscriber->event_handler(event);
        }
    }
}


std::vector<std::shared_ptr<LiveVideoSource::Subscriber>> LiveVideoSource::SnapshotSubscribers() const {
    std::vector<std::shared_ptr<Subscriber>> snapshot;
    snapshot.reserve(subscribers_.size());
    for (const auto& [id, subscriber] : subscribers_) {
        snapshot.push_back(subscriber);
    }
    return snapshot;
}


}  // namespace vms::media
