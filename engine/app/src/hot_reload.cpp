#include "ph/app/hot_reload.hpp"

#include "ph/core/file.hpp"
#include "ph/core/time.hpp"
#include "ph/ecs/world_io.hpp"

#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_stdinc.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <format>
#include <thread>

namespace ph {

namespace fs = std::filesystem;

// AutoBuilder

struct AutoBuilder::Build {
    std::thread thread;
    std::atomic<bool> done{false};
    int exit_code = -1;
    std::string output;
    f64 started = 0;
};

AutoBuilder::AutoBuilder(std::vector<fs::path> source_dirs, std::vector<std::string> command)
    : dirs_(std::move(source_dirs)), command_(std::move(command)) {
    scan(); // the starting point; only later changes trigger builds
}

AutoBuilder::~AutoBuilder() {
    if (build_ && build_->thread.joinable()) build_->thread.join();
}

bool AutoBuilder::is_source(const fs::path& path) {
    static constexpr std::string_view extensions[] = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp"};
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::find(std::begin(extensions), std::end(extensions), ext) != std::end(extensions);
}

bool AutoBuilder::scan() {
    std::vector<std::pair<fs::path, fs::file_time_type>> now;
    for (const fs::path& dir : dirs_) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            const fs::path& p = it->path();
            if (it->is_directory(ec)) {
                // Hidden folders (.git) and build folders (they hold CMakeCache.txt) are never sources.
                if (p.filename().string().starts_with('.') || fs::exists(p / "CMakeCache.txt", ec))
                    it.disable_recursion_pending();
                continue;
            }
            if (is_source(p)) now.emplace_back(p, it->last_write_time(ec));
        }
    }
    std::sort(now.begin(), now.end());
    bool changed = now != stamps_;
    stamps_ = std::move(now);
    return changed;
}

void AutoBuilder::start_build() {
    info("auto-build: source changed, building");
    build_ = std::make_unique<Build>();
    build_->started = now_seconds();
    Build* b = build_.get();
    b->thread = std::thread([b, command = command_] {
        std::vector<const char*> args;
        for (const std::string& a : command) args.push_back(a.c_str());
        args.push_back(nullptr);
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args.data());
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
        SDL_Process* process = SDL_CreateProcessWithProperties(props);
        SDL_DestroyProperties(props);
        if (!process) {
            b->output = std::format("can't start the build: {}", SDL_GetError());
        } else {
            size_t size = 0;
            void* out = SDL_ReadProcess(process, &size, &b->exit_code); // waits for the build to finish
            if (out) b->output.assign(static_cast<const char*>(out), size);
            SDL_free(out);
            SDL_DestroyProcess(process);
        }
        b->done.store(true, std::memory_order_release);
    });
}

bool AutoBuilder::building() const { return build_ != nullptr; }

void AutoBuilder::poll() {
    if (build_ && build_->done.load(std::memory_order_acquire)) {
        build_->thread.join();
        f64 seconds = now_seconds() - build_->started;
        if (build_->exit_code == 0) {
            info("auto-build: built in {:.1f} s", seconds);
        } else {
            // Compiler errors are what matter; fall back to the end of the output when no line says "error".
            std::vector<std::string_view> lines, errors;
            std::string_view rest = build_->output;
            while (!rest.empty()) {
                usize nl = rest.find('\n');
                std::string_view line = rest.substr(0, nl);
                if (!line.empty()) lines.push_back(line);
                if (line.find("error") != std::string_view::npos) errors.push_back(line);
                rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
            }
            const auto& shown = errors.empty() ? lines : errors;
            usize first = errors.empty() && shown.size() > 15 ? shown.size() - 15 : 0;
            for (usize i = first; i < shown.size() && i < first + 15; ++i) error("  {}", shown[i]);
            warn("auto-build failed after {:.1f} s; the game keeps running the previous code", seconds);
        }
        build_.reset();
    }

    f64 now = now_seconds();
    if (now - last_scan_ >= 0.25) {
        last_scan_ = now;
        if (scan()) changed_at_ = now;
    }
    // Wait until saving has settled (editors often write several files), and for any build in progress.
    if (changed_at_ >= 0 && now - changed_at_ > 0.3 && !build_) {
        changed_at_ = -1;
        start_build();
    }
}

// HotReloadHost

namespace {

// The few operating-system calls hot reload needs.
#if defined(_WIN32)
void* load_library(const fs::path& path) { return LoadLibraryW(path.c_str()); }
void* find_symbol(void* library, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
}
void unload_library(void* library) { FreeLibrary(static_cast<HMODULE>(library)); }
std::string last_error() { return std::format("Windows error {}", GetLastError()); }
// The module (executable or library) whose code contains an address.
const void* module_of(const void* address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module))
        return nullptr;
    return module;
}

// Profilers and debuggers keep a loaded library's .pdb open, and the linker then can't write the next build's
// (LNK1201). So each copy gets its own: the .pdb is copied beside it, and the copy's CodeView record is changed to
// name that file. Returns the .pdb copy, or an empty path when there's nothing to do.
fs::path give_own_pdb(const fs::path& library, const fs::path& copy) {
    std::error_code ec;
    fs::path pdb = fs::path(library).replace_extension(".pdb");
    if (!fs::exists(pdb, ec)) return {};
    fs::path own = fs::path(copy).replace_extension(".pdb");
    fs::copy_file(pdb, own, fs::copy_options::overwrite_existing, ec);
    if (ec) return {};

    auto bytes = read_file_bytes(copy);
    if (!bytes) return own;
    std::vector<u8>& b = *bytes;
    auto u16_at = [&](usize at) -> u32 { return at + 2 <= b.size() ? u32(b[at]) | u32(b[at + 1]) << 8 : 0; };
    auto u32_at = [&](usize at) -> u32 { return at + 4 <= b.size() ? u16_at(at) | u16_at(at + 2) << 16 : 0; };
    usize pe = u32_at(0x3C);
    if (u32_at(pe) != 0x00004550) return own; // "PE\0\0"
    u32 sections = u16_at(pe + 6), optional_size = u16_at(pe + 20);
    usize optional = pe + 24;
    usize directories = optional + (u16_at(optional) == 0x20B ? 112 : 96); // PE32+ or PE32
    u32 debug_rva = u32_at(directories + 6 * 8), debug_size = u32_at(directories + 6 * 8 + 4);
    auto file_offset = [&](u32 rva) -> usize {
        for (u32 i = 0; i < sections; ++i) {
            usize s = optional + optional_size + i * 40;
            u32 va = u32_at(s + 12), size = (std::max)(u32_at(s + 8), u32_at(s + 16)), raw = u32_at(s + 20);
            if (rva >= va && rva < va + size) return raw + (rva - va);
        }
        return 0;
    };
    usize debug = file_offset(debug_rva);
    if (!debug) return own;
    std::string name = own.filename().string();
    for (usize e = debug; e + 28 <= debug + debug_size; e += 28) {
        if (u32_at(e + 12) != 2) continue; // IMAGE_DEBUG_TYPE_CODEVIEW
        usize record = u32_at(e + 24), size = u32_at(e + 16);
        if (u32_at(record) != 0x53445352 || size < 25 || record + size > b.size()) continue; // "RSDS"
        // The GUID and age stay, so debuggers still match the file; only its name changes. A bare name is looked
        // for next to the library.
        usize path = record + 24, room = size - 24;
        if (name.size() + 1 > room) continue;
        std::fill(b.begin() + static_cast<std::ptrdiff_t>(path), b.begin() + static_cast<std::ptrdiff_t>(path + room), u8(0));
        std::copy(name.begin(), name.end(), b.begin() + static_cast<std::ptrdiff_t>(path));
        write_file_bytes(copy, b.data(), b.size());
        break;
    }
    return own;
}
#else
void* load_library(const fs::path& path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }
void* find_symbol(void* library, const char* name) { return dlsym(library, name); }
void unload_library(void* library) { dlclose(library); }
std::string last_error() {
    const char* e = dlerror();
    return e ? e : "unknown error";
}
const void* module_of(const void* address) {
    Dl_info info{};
    return dladdr(address, &info) ? info.dli_fbase : nullptr;
}
#endif

} // namespace

using CreateGame = Game* (*)();

struct HotReloadHost::Library {
    void* handle = nullptr;
    CreateGame create = nullptr;
    fs::path copy; // the file actually loaded (a fresh copy each time, so the loader never reuses a cached image)
    fs::path pdb;  // Windows: the copy's own debug symbols
    const void* base = nullptr;

    ~Library() {
        if (handle) unload_library(handle);
        std::error_code ec;
        if (!copy.empty()) fs::remove(copy, ec);
        if (!pdb.empty()) fs::remove(pdb, ec);
    }
};

HotReloadHost::HotReloadHost(Options options) : options_(std::move(options)) {}

HotReloadHost::~HotReloadHost() {
    builder_.reset();
    game_.reset(); // its code lives in the library: destroy it first
    current_.reset();
}

std::unique_ptr<HotReloadHost::Library> HotReloadHost::open_copy() {
    const fs::path& library = options_.library;
    auto lib = std::make_unique<Library>();
    std::error_code ec;
    lib->copy = fs::temp_directory_path() / std::format("ph-hot-{}-{}{}", library.stem().string(), copies_++,
                                                          library.extension().string());
    fs::copy_file(library, lib->copy, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error("hot reload: can't copy {}: {}", library.string(), ec.message());
        lib->copy.clear();
        return nullptr;
    }
#if defined(_WIN32)
    lib->pdb = give_own_pdb(library, lib->copy);
#endif
    lib->handle = load_library(lib->copy);
    if (!lib->handle) {
        error("hot reload: can't load {}: {}", library.string(), last_error());
        return nullptr;
    }
    lib->create = reinterpret_cast<CreateGame>(find_symbol(lib->handle, "ph_create_game"));
    if (!lib->create) {
        error("hot reload: {} has no ph_create_game (is it built with PH_MAIN?)", library.string());
        return nullptr;
    }
    lib->base = module_of(reinterpret_cast<const void*>(lib->create));
    return lib;
}

AppConfig HotReloadHost::configure() {
    std::error_code ec;
    stamp_ = fs::last_write_time(options_.library, ec);
    current_ = open_copy();
    if (!current_) return {};
    game_.reset(current_->create());
    return game_->configure();
}

void HotReloadHost::init(Engine& engine) {
    asset_mark_ = engine.assets().mark();
    if (game_) game_->init(engine);
    info("hot reload: watching {} (rebuild it to reload the game code)", options_.library.string());
    if (!options_.build_command.empty() && !engine.has_arg("--no-auto-build")) {
        builder_ = std::make_unique<AutoBuilder>(options_.source_dirs, options_.build_command);
        info("auto-build: {} source files watched; saving one rebuilds the game code", builder_->watched_files());
    }
}

void HotReloadHost::start(Engine& engine) {
    if (game_) game_->start(engine);
}

void HotReloadHost::check(Engine& engine) {
    if (builder_) builder_->poll();
    f64 now = now_seconds();
    if (now - last_check_ < 0.25) return;
    last_check_ = now;
    std::error_code ec;
    auto stamp = fs::last_write_time(options_.library, ec);
    if (ec) return; // mid-rebuild: the file may be missing for a moment
    if (stamp != stamp_) {
        stamp_ = stamp;
        changed_at_ = now; // wait for the linker to finish writing
        return;
    }
    if (changed_at_ >= 0 && now - changed_at_ > 0.3) {
        changed_at_ = -1;
        reload(engine);
    }
}

namespace {
bool code_in_library(const ecs::ComponentInfo& info, void* base) {
    const void* fns[] = {reinterpret_cast<const void*>(info.move_construct), reinterpret_cast<const void*>(info.destroy),
                         reinterpret_cast<const void*>(info.construct_default), reinterpret_cast<const void*>(info.save),
                         reinterpret_cast<const void*>(info.load), reinterpret_cast<const void*>(info.visit)};
    for (const void* fn : fns)
        if (fn && module_of(fn) == base) return true;
    return false;
}
} // namespace

void HotReloadHost::teardown(Engine& engine) {
    // Everything the game made goes while its code is still loaded (its destructors live there).
    engine.scenes().shutdown(engine);
    engine.fixed_schedule().clear();
    engine.frame_schedule().clear();
    engine.input().unbind_all(); // init() binds again; otherwise every reload would add a second copy
    engine.world().reset();
    engine.world().set_resource<Engine*>(&engine);
    engine.assets().rollback(asset_mark_);
    game_.reset();
}

void HotReloadHost::bring_up(Engine& engine, const std::vector<SavedComponent>& saved, serial::Context& ctx) {
    game_.reset(current_->create());
    asset_mark_ = engine.assets().mark();
    game_->init(engine);
    if (const json::Value* bindings = engine.settings().find("input")) engine.input().load_bindings(*bindings);
    // Components that had to leave while the code changed come back through the new code's loaders.
    for (const SavedComponent& s : saved) {
        auto id = ecs::find_component(s.name);
        if (!id || !ecs::component_info(*id).savable()) {
            ctx.warn(std::format("'{}' wasn't registered by the new code in time; its data was dropped", s.name));
            continue;
        }
        const ecs::ComponentInfo& info = ecs::component_info(*id);
        engine.world().add_component(s.entity, *id, [&](void* p) {
            if (!info.load(s.data, p, ctx)) ctx.warn(std::format("'{}' loaded with errors", s.name));
        });
    }
}

bool HotReloadHost::reload(Engine& engine) {
    Stopwatch watch;
    serial::Context ctx{&engine.assets()};
    ecs::World& world = engine.world();
    void* base = const_cast<void*>(current_->base);

    // The world stays, so entities keep their ids and every reference between them holds. Of the components whose
    // code is in the old library: plain data (trivially copyable) stays where it is, savable ones are saved and come
    // back through the new code, and anything else has to go.
    std::vector<SavedComponent> saved;
    usize kept = 0, dropped = 0;
    std::vector<std::pair<ecs::Entity, ecs::ComponentId>> removals;
    for (ecs::ComponentId c = 0; c < ecs::component_type_count(); ++c) {
        const ecs::ComponentInfo& info = ecs::component_info(c);
        if (info.dormant || !code_in_library(info, base)) continue;
        bool plain = info.size == 0 || info.trivially_relocatable;
        for (const auto& a : world.archetypes()) {
            if (a->empty() || !a->has(c)) continue;
            for (ecs::Entity e : a->entities()) {
                if (plain) {
                    ++kept;
                    continue;
                }
                if (info.savable()) saved.push_back({e, std::string(info.name), info.save(world.component_ptr(e, c), ctx)});
                else ++dropped;
                removals.emplace_back(e, c);
            }
        }
        if (!plain && !info.savable() && dropped)
            ctx.warn(std::format("'{}' is neither plain data nor savable, so it can't survive a reload", info.name));
    }
    for (auto [e, c] : removals) world.remove_component(e, c); // while the old destructors are still loaded

    engine.scenes().shutdown(engine);
    engine.fixed_schedule().clear();
    engine.frame_schedule().clear();
    engine.input().unbind_all(); // init() binds again; otherwise every reload would add a second copy
    world.forget_code();
    world.set_resource<Engine*>(&engine);
    engine.assets().rollback(asset_mark_);
    game_.reset();

    // Component registrations made by the old code go dormant first, so the new library's registrations (which run
    // the moment it loads) refresh them.
    auto previous = ecs::mark_components_dormant(&code_in_library, base);
    auto next = open_copy();
    if (!next) {
        ecs::restore_components(previous);
        bring_up(engine, saved, ctx);
        warn("hot reload: the new build didn't load; still running the previous code");
        return false;
    }
    current_ = std::move(next); // unloads the old library
    bring_up(engine, saved, ctx);
    ++reloads_;
    for (const auto& w : ctx.warnings) warn("hot reload: {}", w);
    info("hot reload #{}: new code running; {} entities kept, {} components kept in place, {} reloaded through their "
         "savers, in {:.0f} ms",
         reloads_, world.entity_count(), kept, saved.size(), watch.milliseconds());
    return true;
}

void HotReloadHost::fixed_update(Engine& engine, f32 dt) {
    if (game_) game_->fixed_update(engine, dt);
}

void HotReloadHost::update(Engine& engine, f32 dt) {
    check(engine);
    if (game_) game_->update(engine, dt);
}

void HotReloadHost::render(Engine& engine) {
    if (game_) game_->render(engine);
}

void HotReloadHost::dev_ui(Engine& engine) {
    if (game_) game_->dev_ui(engine);
}

void HotReloadHost::event(Engine& engine, const SDL_Event& e) {
    if (game_) game_->event(engine, e);
}

void HotReloadHost::shutdown(Engine& engine) {
    if (game_) game_->shutdown(engine);
    teardown(engine);
}

} // namespace ph
