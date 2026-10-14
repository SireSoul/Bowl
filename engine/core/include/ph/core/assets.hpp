#pragma once

#include "ph/core/file.hpp"
#include "ph/core/type_id.hpp"
#include "ph/core/types.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ph {

class AssetManager;

template <class T> struct AssetSlot {
    std::unique_ptr<T> value;
    u32 version = 0;
    std::string path;
};

// Shared handle to a loaded asset. Hot reload swaps the value in place, so every handle sees the new data; compare
// version() with a cached copy to rebuild anything derived from it (pipelines, atlases, ...).
template <class T> class Asset {
public:
    Asset() = default;
    explicit Asset(std::shared_ptr<AssetSlot<T>> slot) : slot_(std::move(slot)) {}

    explicit operator bool() const { return slot_ && slot_->value; }
    T* get() const { return slot_ ? slot_->value.get() : nullptr; }
    T* operator->() const { return get(); }
    T& operator*() const { return *get(); }
    u32 version() const { return slot_ ? slot_->version : 0; }
    std::string_view path() const { return slot_ ? std::string_view{slot_->path} : std::string_view{}; }
    bool operator==(const Asset& o) const { return slot_ == o.slot_; }

private:
    std::shared_ptr<AssetSlot<T>> slot_;
};

// Passed to loaders. Read files through it so hot reload knows what the asset depends on.
class AssetLoadContext {
public:
    AssetLoadContext(AssetManager& assets, std::string_view path) : assets_(assets), path_(path) {}

    // The virtual path being loaded, e.g. "textures/hero.png".
    std::string_view path() const { return path_; }
    AssetManager& assets() const { return assets_; }

    // Reads a file by virtual path from the first mount that has it, and records it as a dependency.
    std::optional<std::vector<u8>> read(std::string_view virtual_path);
    std::optional<std::string> read_text(std::string_view virtual_path);
    bool exists(std::string_view virtual_path) const;
    // Records a real file as a dependency (e.g. a shader #include resolved by a compiler).
    void depends_on(const std::filesystem::path& file) { dependencies_.push_back(file); }

    const std::vector<std::filesystem::path>& dependencies() const { return dependencies_; }

private:
    AssetManager& assets_;
    std::string path_;
    std::vector<std::filesystem::path> dependencies_;
};

template <class T> using AssetLoader = std::function<std::unique_ptr<T>(AssetLoadContext&)>;

// Loads and caches assets by virtual path, from an ordered list of mounted folders. Not thread-safe: use it from the
// main thread. Failed reloads keep the previous value, so a typo in a shader never takes the game down.
class AssetManager : NonCopyable {
public:
    AssetManager();
    ~AssetManager();

    // Files under `virtual_prefix` (e.g. "engine/" or "") resolve into `directory`. Earlier mounts win.
    void mount(std::string virtual_prefix, std::filesystem::path directory);
    std::optional<std::filesystem::path> resolve(std::string_view virtual_path) const;

    template <class T> void register_loader(AssetLoader<T> loader) {
        loaders_[type_id<T>()] = std::make_shared<AssetLoader<T>>(std::move(loader));
    }

    // Returns the cached asset or loads it. On failure the handle is empty (and the error is logged); if the file
    // appears or is fixed later, hot reload fills it in.
    template <class T> Asset<T> load(std::string_view path);

    // Registers an asset made in code (procedural textures, generated sounds). Never reloaded.
    template <class T> Asset<T> add(std::string_view path, std::unique_ptr<T> value);

    template <class T> Asset<T> find(std::string_view path) const;

    // Re-runs the loader of every asset whose files changed. Call from the main loop; cheap when nothing changed.
    void set_hot_reload(bool enabled) { hot_reload_ = enabled; }
    bool hot_reload() const { return hot_reload_; }
    void poll();
    // Reloads one asset now (dev tools button). Returns false if it failed or isn't loaded.
    bool reload(std::string_view path);
    void on_reload(std::function<void(std::string_view path, bool ok)> callback) { on_reload_.push_back(std::move(callback)); }

    struct Info {
        std::string path;
        std::string_view type;
        u32 version;
        bool loaded;
    };
    std::vector<Info> list() const;

private:
    struct Entry {
        std::string path;
        TypeId type;
        std::string_view type_name;
        std::shared_ptr<void> slot;
        std::function<bool(Entry&)> reload; // empty for code-made assets
        std::function<u32()> version;
        std::function<bool()> loaded;
        std::vector<FileWatcher::WatchId> watches;
        bool dirty = false;
    };
    struct Key {
        TypeId type;
        std::string path;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        usize operator()(const Key& k) const { return std::hash<std::string>{}(k.path) * 31 + k.type; }
    };
    struct Mount {
        std::string prefix;
        std::filesystem::path directory;
    };

    template <class T> bool run_loader(Entry& entry);
    void watch_dependencies(Entry& entry, const std::vector<std::filesystem::path>& files);
    void notify(std::string_view path, bool ok);

    std::vector<Mount> mounts_;
    std::unordered_map<TypeId, std::shared_ptr<void>> loaders_;
    std::unordered_map<Key, std::unique_ptr<Entry>, KeyHash> entries_;
    std::vector<std::function<void(std::string_view, bool)>> on_reload_;
    FileWatcher watcher_;
    bool hot_reload_ = false;
};

void log_asset_error(std::string_view path, std::string_view type, std::string_view message);

template <class T> bool AssetManager::run_loader(Entry& entry) {
    auto it = loaders_.find(type_id<T>());
    if (it == loaders_.end()) {
        log_asset_error(entry.path, type_name<T>(), "no loader registered for this type");
        return false;
    }
    auto& loader = *static_cast<AssetLoader<T>*>(it->second.get());
    AssetLoadContext ctx(*this, entry.path);
    std::unique_ptr<T> value = loader(ctx);
    watch_dependencies(entry, ctx.dependencies());
    if (!value) return false;
    auto& slot = *static_cast<AssetSlot<T>*>(entry.slot.get());
    slot.value = std::move(value);
    ++slot.version;
    return true;
}

template <class T> Asset<T> AssetManager::load(std::string_view path) {
    Key key{type_id<T>(), std::string(path)};
    if (auto it = entries_.find(key); it != entries_.end())
        return Asset<T>(std::static_pointer_cast<AssetSlot<T>>(it->second->slot));

    auto slot = std::make_shared<AssetSlot<T>>();
    slot->path = std::string(path);
    auto entry = std::make_unique<Entry>();
    entry->path = std::string(path);
    entry->type = key.type;
    entry->type_name = type_name<T>();
    entry->slot = slot;
    entry->reload = [this](Entry& e) { return run_loader<T>(e); };
    entry->version = [s = slot.get()] { return s->version; };
    entry->loaded = [s = slot.get()] { return s->value != nullptr; };
    Entry& e = *entry;
    entries_.emplace(std::move(key), std::move(entry));
    run_loader<T>(e);
    return Asset<T>(std::move(slot));
}

template <class T> Asset<T> AssetManager::add(std::string_view path, std::unique_ptr<T> value) {
    Key key{type_id<T>(), std::string(path)};
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        auto slot = std::make_shared<AssetSlot<T>>();
        slot->path = std::string(path);
        auto entry = std::make_unique<Entry>();
        entry->path = std::string(path);
        entry->type = key.type;
        entry->type_name = type_name<T>();
        entry->slot = slot;
        entry->version = [s = slot.get()] { return s->version; };
        entry->loaded = [s = slot.get()] { return s->value != nullptr; };
        it = entries_.emplace(std::move(key), std::move(entry)).first;
    }
    auto slot = std::static_pointer_cast<AssetSlot<T>>(it->second->slot);
    slot->value = std::move(value);
    ++slot->version;
    return Asset<T>(std::move(slot));
}

template <class T> Asset<T> AssetManager::find(std::string_view path) const {
    auto it = entries_.find(Key{type_id<T>(), std::string(path)});
    if (it == entries_.end()) return {};
    return Asset<T>(std::static_pointer_cast<AssetSlot<T>>(it->second->slot));
}

} // namespace ph
