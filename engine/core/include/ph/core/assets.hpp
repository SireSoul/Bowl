#pragma once

#include "ph/core/archive.hpp"
#include "ph/core/file.hpp"
#include "ph/core/type_id.hpp"
#include "ph/core/types.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
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
    using value_type = T;

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

template <class T> struct is_asset_handle : std::false_type {};
template <class T> struct is_asset_handle<Asset<T>> : std::true_type {};

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

    // Background loads (AssetManager::load_async): what the type's decoder made from the file on the loading thread,
    // or null. The loader then only does what has to happen on the main thread (a GPU upload, say).
    template <class P> P* prepared() const { return static_cast<P*>(prepared_.get()); }

private:
    friend class AssetManager;
    AssetManager& assets_;
    std::string path_;
    std::vector<std::filesystem::path> dependencies_;
    std::optional<std::vector<u8>> prefetched_; // the asset's own file, already read on the loading thread
    std::shared_ptr<void> prepared_;
};

template <class T> using AssetLoader = std::function<std::unique_ptr<T>(AssetLoadContext&)>;
// Runs on the loading thread for load_async: turns the file's bytes into whatever saves the loader work (decoded
// pixels, decoded samples). Must be thread-safe. Null: nothing to prepare.
using AssetDecoder = std::function<std::shared_ptr<void>(std::string_view path, std::span<const u8> bytes)>;

// Loads and caches assets by virtual path from an ordered list of mounted folders and archives. Main thread only
// (load_async does its file reading and decoding on a thread of its own).
// If a reload fails (say, a shader with a syntax error), the previous value stays in place.
class AssetManager : NonCopyable {
public:
    AssetManager();
    ~AssetManager();

    // Files under `virtual_prefix` (e.g. "engine/" or "") resolve into `directory`. Earlier mounts win.
    void mount(std::string virtual_prefix, std::filesystem::path directory);
    // The same, from an archive (.phpak, see ph/core/archive.hpp). False if it isn't one.
    bool mount_archive(std::string virtual_prefix, const std::filesystem::path& archive);
    // The real file a virtual path reads from, for code that needs one (a font library, a compiler). Nothing when the
    // file is missing or inside an archive: read() works for both.
    std::optional<std::filesystem::path> resolve(std::string_view virtual_path) const;
    bool exists(std::string_view virtual_path) const;
    // A file's bytes by virtual path, from whichever mount has it. Not cached and not watched: load assets with load().
    std::optional<std::vector<u8>> read(std::string_view virtual_path) const;

    template <class T> void register_loader(AssetLoader<T> loader) {
        auto& slot = loaders_[type_id<T>()];
        std::shared_ptr<LoaderSlot> previous = slot;
        slot = std::make_shared<LoaderSlot>(LoaderSlot{std::make_shared<AssetLoader<T>>(std::move(loader)), ++serial_, previous});
    }

    // Returns the cached asset or loads it. On failure the handle is empty (and the error is logged); if the file
    // appears or is fixed later, hot reload fills it in. An asset still loading in the background is finished now.
    template <class T> Asset<T> load(std::string_view path);

    // Starts loading in the background and returns the handle at once: the file is read, and decoded if the type has
    // a decoder, on a loading thread; finish_loads() (the App calls it every frame) completes it on this thread. The
    // handle stays empty until then, so check it before use. Loading screens: start everything, wait for loading()
    // to reach zero.
    template <class T> Asset<T> load_async(std::string_view path);
    template <class T> void register_decoder(AssetDecoder decoder) {
        decoders_[type_id<T>()] = std::make_shared<AssetDecoder>(std::move(decoder));
    }
    // Background loads not finished yet.
    usize loading() const;
    // Completes the background loads that are ready.
    void finish_loads();

    // Registers an asset made in code (procedural textures, generated sounds). Never reloaded.
    template <class T> Asset<T> add(std::string_view path, std::unique_ptr<T> value);

    template <class T> Asset<T> find(std::string_view path) const;

    // Re-runs the loader of every asset whose files changed. Call from the main loop; cheap when nothing changed.
    void set_hot_reload(bool enabled) { hot_reload_ = enabled; }
    bool hot_reload() const { return hot_reload_; }
    void poll();
    // Reloads one asset now (dev tools button). Returns false if it failed or isn't loaded.
    bool reload(std::string_view path);
    void on_reload(std::function<void(std::string_view path, bool ok)> callback) {
        on_reload_.push_back({std::move(callback), ++serial_});
    }

    // rollback(mark) drops every loader, asset and callback registered after mark() was taken, and restores loaders
    // they replaced. Game-code hot reload uses it to free the game library's objects before unloading the library.
    u64 mark() const { return serial_; }
    void rollback(u64 mark);

    struct Info {
        std::string path;
        std::string_view type;
        u32 version;
        bool loaded;
    };
    std::vector<Info> list() const;

private:
    friend class AssetLoadContext;
    struct LoaderSlot {
        std::shared_ptr<void> loader; // AssetLoader<T>
        u64 serial;
        std::shared_ptr<LoaderSlot> previous; // what this registration replaced
    };
    struct Callback {
        std::function<void(std::string_view, bool)> fn;
        u64 serial;
    };
    struct Entry {
        std::string path;
        TypeId type;
        std::string type_name;
        u64 serial = 0;
        std::shared_ptr<void> slot;
        std::function<bool(Entry&)> reload; // empty for code-made assets
        std::function<u32()> version;
        std::function<bool()> loaded;
        std::vector<FileWatcher::WatchId> watches;
        bool dirty = false;
        // Background loads: waiting for the loading thread, and how to finish with what it read.
        bool pending = false;
        std::function<bool(Entry&, std::optional<std::vector<u8>>, std::shared_ptr<void>)> finish;
    };
    struct Key {
        TypeId type;
        std::string path;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        usize operator()(const Key& k) const { return std::hash<std::string>{}(k.path) * 31 + static_cast<usize>(k.type); }
    };
    struct Mount {
        std::string prefix;
        std::filesystem::path directory;
        std::shared_ptr<const Archive> archive; // set: read from this instead of the directory
    };
    // Where a virtual path's bytes come from: a real file, or a file in an archive.
    struct Source {
        std::filesystem::path file;
        std::shared_ptr<const Archive> archive;
        std::string name;
    };
    std::optional<Source> locate(std::string_view virtual_path) const;
    static std::optional<std::vector<u8>> read_source(const Source& source);

    template <class T> Entry& make_entry(Key key, bool from_file);
    template <class T>
    bool run_loader(Entry& entry, std::optional<std::vector<u8>> prefetched = {}, std::shared_ptr<void> prepared = {});
    struct Background; // the loading thread and its queues
    void start_background(const Key& key, Source source, std::shared_ptr<AssetDecoder> decoder);
    void watch_dependencies(Entry& entry, const std::vector<std::filesystem::path>& files);
    void notify(std::string_view path, bool ok);

    std::vector<Mount> mounts_;
    std::unordered_map<TypeId, std::shared_ptr<LoaderSlot>> loaders_;
    std::unordered_map<Key, std::unique_ptr<Entry>, KeyHash> entries_;
    std::vector<Callback> on_reload_;
    FileWatcher watcher_;
    bool hot_reload_ = false;
    u64 serial_ = 0;
    std::unordered_map<TypeId, std::shared_ptr<AssetDecoder>> decoders_;
    std::unique_ptr<Background> background_;
};

void log_asset_error(std::string_view path, std::string_view type, std::string_view message);

template <class T>
bool AssetManager::run_loader(Entry& entry, std::optional<std::vector<u8>> prefetched, std::shared_ptr<void> prepared) {
    auto it = loaders_.find(type_id<T>());
    if (it == loaders_.end() || !it->second) {
        log_asset_error(entry.path, type_name<T>(), "no loader registered for this type");
        return false;
    }
    auto& loader = *static_cast<AssetLoader<T>*>(it->second->loader.get());
    AssetLoadContext ctx(*this, entry.path);
    ctx.prefetched_ = std::move(prefetched);
    ctx.prepared_ = std::move(prepared);
    std::unique_ptr<T> value = loader(ctx);
    watch_dependencies(entry, ctx.dependencies());
    if (!value) return false;
    auto& slot = *static_cast<AssetSlot<T>*>(entry.slot.get());
    slot.value = std::move(value);
    ++slot.version;
    return true;
}

template <class T> AssetManager::Entry& AssetManager::make_entry(Key key, bool from_file) {
    auto slot = std::make_shared<AssetSlot<T>>();
    slot->path = key.path;
    auto entry = std::make_unique<Entry>();
    entry->path = key.path;
    entry->type = key.type;
    entry->type_name = std::string(type_name<T>());
    entry->serial = ++serial_;
    entry->slot = slot;
    if (from_file) entry->reload = [this](Entry& e) { return run_loader<T>(e); };
    entry->version = [s = slot.get()] { return s->version; };
    entry->loaded = [s = slot.get()] { return s->value != nullptr; };
    return *entries_.emplace(std::move(key), std::move(entry)).first->second;
}

template <class T> Asset<T> AssetManager::load(std::string_view path) {
    Key key{type_id<T>(), std::string(path)};
    if (auto it = entries_.find(key); it != entries_.end()) {
        if (it->second->pending) { // needed now: don't wait for the loading thread
            it->second->pending = false;
            run_loader<T>(*it->second);
        }
        return Asset<T>(std::static_pointer_cast<AssetSlot<T>>(it->second->slot));
    }
    Entry& entry = make_entry<T>(std::move(key), true);
    run_loader<T>(entry);
    return Asset<T>(std::static_pointer_cast<AssetSlot<T>>(entry.slot));
}

template <class T> Asset<T> AssetManager::load_async(std::string_view path) {
    Key key{type_id<T>(), std::string(path)};
    if (auto it = entries_.find(key); it != entries_.end())
        return Asset<T>(std::static_pointer_cast<AssetSlot<T>>(it->second->slot));
    Entry& entry = make_entry<T>(key, true);
    auto source = locate(path);
    if (!source) { // nothing to read: fail (and start watching) the usual way
        run_loader<T>(entry);
        return Asset<T>(std::static_pointer_cast<AssetSlot<T>>(entry.slot));
    }
    entry.pending = true;
    entry.finish = [this](Entry& e, std::optional<std::vector<u8>> bytes, std::shared_ptr<void> prepared) {
        return run_loader<T>(e, std::move(bytes), std::move(prepared));
    };
    auto decoder = decoders_.find(type_id<T>());
    start_background(key, std::move(*source), decoder == decoders_.end() ? nullptr : decoder->second);
    return Asset<T>(std::static_pointer_cast<AssetSlot<T>>(entry.slot));
}

template <class T> Asset<T> AssetManager::add(std::string_view path, std::unique_ptr<T> value) {
    Key key{type_id<T>(), std::string(path)};
    auto it = entries_.find(key);
    Entry& entry = it != entries_.end() ? *it->second : make_entry<T>(std::move(key), false);
    auto slot = std::static_pointer_cast<AssetSlot<T>>(entry.slot);
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
