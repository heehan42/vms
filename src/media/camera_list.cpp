#include "media/camera_list.h"

#include <utility>


namespace vms::media {


CameraEntry& CameraList::Add(std::string name, std::string url) {
    auto entry = std::make_unique<CameraEntry>();
    entry->name = std::move(name);
    entry->url = std::move(url);
    entries_.push_back(std::move(entry));
    return *entries_.back();
}


CameraEntry* CameraList::Find(std::string_view name) const {
    for (const std::unique_ptr<CameraEntry>& entry : entries_) {
        if (entry->name == name) {
            return entry.get();
        }
    }
    return nullptr;
}


}  // namespace vms::media
