#include "bowl/core/file.hpp"

#include <fstream>

namespace bowl {

namespace fs = std::filesystem;

std::optional<std::vector<u8>> read_file_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return std::nullopt;
    auto size = static_cast<usize>(in.tellg());
    std::vector<u8> bytes(size);
    in.seekg(0);
    if (size && !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) return std::nullopt;
    return bytes;
}

std::optional<std::string> read_file_text(const fs::path& path) {
    auto bytes = read_file_bytes(path);
    if (!bytes) return std::nullopt;
    return std::string(bytes->begin(), bytes->end());
}

bool write_file_bytes(const fs::path& path, const void* data, usize size) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

static fs::file_time_type stamp_of(const fs::path& path) {
    std::error_code ec;
    auto t = fs::last_write_time(path, ec);
    return ec ? fs::file_time_type::min() : t;
}

FileWatcher::WatchId FileWatcher::watch(const fs::path& path, Callback on_change) {
    WatchId id = next_id_++;
    entries_.push_back({id, path, stamp_of(path), std::move(on_change)});
    return id;
}

void FileWatcher::unwatch(WatchId id) {
    std::erase_if(entries_, [id](const Entry& e) { return e.id == id; });
}

void FileWatcher::poll() {
    // Callbacks may add or remove watches, so collect first.
    std::vector<std::pair<Callback, fs::path>> changed;
    for (Entry& e : entries_) {
        auto now = stamp_of(e.path);
        if (now != e.stamp) {
            e.stamp = now;
            changed.emplace_back(e.on_change, e.path);
        }
    }
    for (auto& [callback, path] : changed) callback(path);
}

} // namespace bowl
