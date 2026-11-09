#include "playback/recording_library.h"

#include <algorithm>
#include <print>
#include <utility>
#include <variant>

#include "core/json.h"


namespace vms::playback {


RecordingLibrary::RecordingLibrary(recording::RecordedFileCatalog catalog) {
    for (auto& [camera_folder, files] : catalog) {
        if (!core::IsValidUtf8(camera_folder)) {
            std::println(stderr, "녹화 폴더 이름이 UTF-8이 아니어서 재생 목록에서 뺌: {}", camera_folder);
            continue;
        }
        if (files.empty()) {
            continue;
        }
        folders_.emplace(camera_folder, std::move(files));
    }
}


void RecordingLibrary::Apply(recording::RecordingCatalogEvent event) {
    if (auto* closed = std::get_if<recording::RecordedFileClosed>(&event)) {
        if (!core::IsValidUtf8(closed->camera_folder)) {
            return;
        }
        std::vector<recording::RecordedFile>& files = folders_[closed->camera_folder];
        const auto position =
            std::ranges::upper_bound(files, closed->file.start_unix_ms, {}, &recording::RecordedFile::start_unix_ms);
        files.insert(position, std::move(closed->file));
        return;
    }

    const auto& deleted = std::get<recording::RecordedFileDeleted>(event);
    const auto folder = folders_.find(deleted.camera_folder);
    if (folder == folders_.end()) {
        return;
    }
    std::erase_if(folder->second, [&](const recording::RecordedFile& file) { return file.stem == deleted.stem; });
    if (folder->second.empty()) {
        folders_.erase(folder);
    }
}


const std::vector<recording::RecordedFile>* RecordingLibrary::FindFolder(std::string_view camera_folder) const {
    const auto folder = folders_.find(camera_folder);
    return folder == folders_.end() ? nullptr : &folder->second;
}


const recording::RecordedFile* RecordingLibrary::FindFile(std::string_view camera_folder,
                                                          std::string_view stem) const {
    const std::vector<recording::RecordedFile>* files = FindFolder(camera_folder);
    if (files == nullptr) {
        return nullptr;
    }
    const auto file = std::ranges::find(*files, stem, &recording::RecordedFile::stem);
    return file == files->end() ? nullptr : &*file;
}


}  // namespace vms::playback
