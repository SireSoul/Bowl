#pragma once

#include "bowl/core/types.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bowl {

std::optional<std::vector<u8>> read_file_bytes(const std::filesystem::path& path);
std::optional<std::string> read_file_text(const std::filesystem::path& path);
bool write_file_bytes(const std::filesystem::path& path, const void* data, usize size);

// Polls modification times. Portable (no OS watch APIs) and cheap at a few hundred files; call poll() a few times a second.
class FileWatcher {
public:
    using Callback = std::function<void(const std::filesystem::path&)>;
    using WatchId = u32;

    WatchId watch(const std::filesystem::path& path, Callback on_change);
    void unwatch(WatchId id);
    // Calls the callback of each file whose modification time changed since the last poll.
    void poll();
    usize size() const { return entries_.size(); }

private:
    struct Entry {
        WatchId id;
        std::filesystem::path path;
        std::filesystem::file_time_type stamp;
        Callback on_change;
    };
    std::vector<Entry> entries_;
    WatchId next_id_ = 1;
};

} // namespace bowl
