#include "ph/core/archive.hpp"

#include "ph/core/file.hpp"
#include "ph/core/log.hpp"

#include <algorithm>
#include <fstream>

namespace ph {

namespace fs = std::filesystem;

namespace {

constexpr char magic[4] = {'P', 'H', 'P', 'K'};
constexpr u32 version = 1;

template <class T> void put(std::vector<u8>& out, T v) {
    for (usize i = 0; i < sizeof(T); ++i) out.push_back(static_cast<u8>(static_cast<u64>(v) >> (8 * i)));
}
template <class T> bool get(std::ifstream& in, T& v) {
    u8 bytes[sizeof(T)];
    if (!in.read(reinterpret_cast<char*>(bytes), sizeof(T))) return false;
    u64 value = 0;
    for (usize i = 0; i < sizeof(T); ++i) value |= static_cast<u64>(bytes[i]) << (8 * i);
    v = static_cast<T>(value);
    return true;
}

} // namespace

std::unique_ptr<Archive> Archive::open(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    char head[4];
    u32 file_version = 0, count = 0;
    if (!in.read(head, 4) || !std::equal(head, head + 4, magic) || !get(in, file_version) || !get(in, count))
        return nullptr;
    if (file_version != version) {
        error("archive {}: version {}, this build reads {}", path.string(), file_version, version);
        return nullptr;
    }
    std::error_code ec;
    u64 file_size = fs::file_size(path, ec);
    auto archive = std::unique_ptr<Archive>(new Archive());
    archive->path_ = path;
    for (u32 i = 0; i < count; ++i) {
        u16 length = 0;
        Entry e;
        if (!get(in, length)) return nullptr;
        std::string name(length, '\0');
        if (!in.read(name.data(), length) || !get(in, e.offset) || !get(in, e.size)) return nullptr;
        if (e.offset + e.size > file_size || e.offset + e.size < e.offset) {
            error("archive {}: '{}' runs past the end", path.string(), name);
            return nullptr;
        }
        archive->entries_.emplace(std::move(name), e);
    }
    return archive;
}

bool Archive::contains(std::string_view name) const { return entries_.contains(std::string(name)); }

std::optional<std::vector<u8>> Archive::read(std::string_view name) const {
    auto it = entries_.find(std::string(name));
    if (it == entries_.end()) return std::nullopt;
    std::ifstream in(path_, std::ios::binary); // a stream per read, so threads don't share one
    std::vector<u8> bytes(static_cast<usize>(it->second.size));
    in.seekg(static_cast<std::streamoff>(it->second.offset));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return std::nullopt;
    return bytes;
}

std::vector<std::string> Archive::names() const {
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const auto& [name, e] : entries_) out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

bool write_archive(const fs::path& archive, const fs::path& folder) {
    std::error_code ec;
    std::vector<std::pair<std::string, fs::path>> files;
    for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string name = fs::relative(it->path(), folder, ec).generic_string();
        if (name.size() > 0xFFFF) return false;
        files.emplace_back(std::move(name), it->path());
    }
    if (ec) {
        error("archive: can't list {}: {}", folder.string(), ec.message());
        return false;
    }
    std::sort(files.begin(), files.end()); // the same folder makes the same archive

    std::vector<std::vector<u8>> contents;
    for (const auto& [name, file] : files) {
        auto bytes = read_file_bytes(file);
        if (!bytes) {
            error("archive: can't read {}", file.string());
            return false;
        }
        contents.push_back(std::move(*bytes));
    }
    std::vector<u8> out(magic, magic + 4);
    put(out, version);
    put(out, static_cast<u32>(files.size()));
    u64 offset = 12;
    for (const auto& [name, file] : files) offset += 2 + name.size() + 16;
    for (usize i = 0; i < files.size(); ++i) {
        put(out, static_cast<u16>(files[i].first.size()));
        out.insert(out.end(), files[i].first.begin(), files[i].first.end());
        put(out, offset);
        put(out, static_cast<u64>(contents[i].size()));
        offset += contents[i].size();
    }
    for (const auto& c : contents) out.insert(out.end(), c.begin(), c.end());
    if (archive.has_parent_path()) fs::create_directories(archive.parent_path(), ec);
    return write_file_bytes(archive, out.data(), out.size());
}

} // namespace ph
