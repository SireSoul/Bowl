#include "ph/core/assets.hpp"

#include "ph/core/log.hpp"

#include <algorithm>

namespace ph {

namespace fs = std::filesystem;

void log_asset_error(std::string_view path, std::string_view type, std::string_view message) {
    error("asset '{}' ({}): {}", path, type, message);
}

// --- AssetLoadContext -----------------------------------------------------------------------------------------------

std::optional<std::vector<u8>> AssetLoadContext::read(std::string_view virtual_path) {
    auto file = assets_.resolve(virtual_path);
    if (!file) return std::nullopt;
    depends_on(*file);
    return read_file_bytes(*file);
}

std::optional<std::string> AssetLoadContext::read_text(std::string_view virtual_path) {
    auto bytes = read(virtual_path);
    if (!bytes) return std::nullopt;
    return std::string(bytes->begin(), bytes->end());
}

bool AssetLoadContext::exists(std::string_view virtual_path) const { return assets_.resolve(virtual_path).has_value(); }

// --- AssetManager ---------------------------------------------------------------------------------------------------

AssetManager::AssetManager() = default;
AssetManager::~AssetManager() = default;

void AssetManager::mount(std::string virtual_prefix, fs::path directory) {
    info("assets: mounted '{}' -> {}", virtual_prefix, directory.string());
    mounts_.push_back({std::move(virtual_prefix), std::move(directory)});
}

std::optional<fs::path> AssetManager::resolve(std::string_view virtual_path) const {
    for (const Mount& m : mounts_) {
        if (!virtual_path.starts_with(m.prefix)) continue;
        fs::path candidate = m.directory / fs::path(virtual_path.substr(m.prefix.size()));
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
    return std::nullopt;
}

void AssetManager::watch_dependencies(Entry& entry, const std::vector<fs::path>& files) {
    for (auto id : entry.watches) watcher_.unwatch(id);
    entry.watches.clear();
    // Also watch the paths the asset *would* load from, so a missing file that appears later gets picked up.
    std::vector<fs::path> watch = files;
    for (const Mount& m : mounts_)
        if (std::string_view(entry.path).starts_with(m.prefix))
            watch.push_back(m.directory / fs::path(std::string_view(entry.path).substr(m.prefix.size())));
    std::sort(watch.begin(), watch.end());
    watch.erase(std::unique(watch.begin(), watch.end()), watch.end());
    Entry* e = &entry;
    for (const fs::path& f : watch) entry.watches.push_back(watcher_.watch(f, [e](const fs::path&) { e->dirty = true; }));
}

void AssetManager::notify(std::string_view path, bool ok) {
    for (auto& cb : on_reload_) cb(path, ok);
}

void AssetManager::poll() {
    if (!hot_reload_) return;
    watcher_.poll();
    for (auto& [key, entry] : entries_) {
        if (!entry->dirty || !entry->reload) continue;
        entry->dirty = false;
        bool ok = entry->reload(*entry);
        if (ok) info("assets: reloaded '{}'", entry->path);
        else warn("assets: reload of '{}' failed, keeping the previous version", entry->path);
        notify(entry->path, ok);
    }
}

bool AssetManager::reload(std::string_view path) {
    bool any = false, ok = true;
    for (auto& [key, entry] : entries_) {
        if (entry->path != path || !entry->reload) continue;
        any = true;
        bool r = entry->reload(*entry);
        notify(entry->path, r);
        ok = ok && r;
    }
    return any && ok;
}

std::vector<AssetManager::Info> AssetManager::list() const {
    std::vector<Info> out;
    out.reserve(entries_.size());
    for (auto& [key, e] : entries_) out.push_back({e->path, e->type_name, e->version(), e->loaded()});
    std::sort(out.begin(), out.end(), [](const Info& a, const Info& b) { return a.path < b.path; });
    return out;
}

} // namespace ph
