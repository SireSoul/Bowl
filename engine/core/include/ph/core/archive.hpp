#pragma once

#include "ph/core/types.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ph {

// An asset archive (.phpak): many files in one, stored as they are, with an index at the front. Shipping builds put
// their asset folders in archives (PH_PACK_ASSETS) and AssetManager::mount_archive reads from them; ph-pack makes them.
//
// Layout, little-endian: "PHPK", u32 version (1), u32 file count, then per file: u16 name length, the name (UTF-8,
// '/' separated), u64 offset from the start of the archive, u64 size; then the files' bytes.
class Archive : NonCopyable {
public:
    // Reads the index. Nothing if the file isn't an archive.
    static std::unique_ptr<Archive> open(const std::filesystem::path& path);

    bool contains(std::string_view name) const;
    // A file's bytes. Safe from any thread.
    std::optional<std::vector<u8>> read(std::string_view name) const;
    std::vector<std::string> names() const; // sorted
    const std::filesystem::path& path() const { return path_; }

private:
    struct Entry {
        u64 offset = 0, size = 0;
    };
    std::filesystem::path path_;
    std::unordered_map<std::string, Entry> entries_;
};

// Writes an archive of every file under `folder`, named by their paths relative to it. False if a file can't be read
// or the archive can't be written.
bool write_archive(const std::filesystem::path& archive, const std::filesystem::path& folder);

} // namespace ph
