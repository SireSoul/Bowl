#include "ph/app/hot_reload.hpp"

#include "ph/core/time.hpp"
#include "ph/ecs/world_io.hpp"

#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_stdinc.h>

#if !defined(_WIN32)
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

using CreateGame = Game* (*)();

struct HotReloadHost::Library {
    void* handle = nullptr;
    CreateGame create = nullptr;
    fs::path copy; // the file actually loaded (a fresh copy each time, so the loader never reuses a cached image)
    const void* base = nullptr;

    ~Library() {
#if !defined(_WIN32)
        if (handle) dlclose(handle);
#endif
        std::error_code ec;
        if (!copy.empty()) fs::remove(copy, ec);
    }
};

HotReloadHost::HotReloadHost(Options options) : options_(std::move(options)) {}

HotReloadHost::~HotReloadHost() {
    builder_.reset();
    game_.reset(); // its code lives in the library: destroy it first
    current_.reset();
}

std::unique_ptr<HotReloadHost::Library> HotReloadHost::open_copy() {
#if defined(_WIN32)
    error("hot reload: not supported on Windows yet");
    return nullptr;
#else
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
    lib->handle = dlopen(lib->copy.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib->handle) {
        error("hot reload: {}", dlerror());
        return nullptr;
    }
    lib->create = reinterpret_cast<CreateGame>(dlsym(lib->handle, "ph_create_game"));
    if (!lib->create) {
        error("hot reload: {} has no ph_create_game (is it built with PH_MAIN?)", library.string());
        return nullptr;
    }
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(lib->create), &info)) lib->base = info.dli_fbase;
    return lib;
#endif
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
#if defined(_WIN32)
    (void)info;
    (void)base;
    return false;
#else
    const void* fns[] = {reinterpret_cast<const void*>(info.move_construct), reinterpret_cast<const void*>(info.destroy),
                         reinterpret_cast<const void*>(info.construct_default), reinterpret_cast<const void*>(info.save),
                         reinterpret_cast<const void*>(info.load), reinterpret_cast<const void*>(info.visit)};
    for (const void* fn : fns) {
        Dl_info di{};
        if (fn && dladdr(fn, &di) && di.dli_fbase == base) return true;
    }
    return false;
#endif
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

void HotReloadHost::bring_up(Engine& engine, const json::Value& world, serial::Context& ctx) {
    game_.reset(current_->create());
    asset_mark_ = engine.assets().mark();
    game_->init(engine);
    if (const json::Value* bindings = engine.settings().find("input")) engine.input().load_bindings(*bindings);
    ecs::load_entities(engine.world(), world, ctx);
}

bool HotReloadHost::reload(Engine& engine) {
    Stopwatch watch;
    serial::Context ctx{&engine.assets()};
    json::Value saved = ecs::save_world(engine.world(), ctx);
    usize entity_count = saved["entities"].size();
    teardown(engine);
    // Component registrations made by the old code go dormant first, so the new library's registrations (which run
    // the moment it loads) refresh them.
    auto previous = ecs::mark_components_dormant(&code_in_library, const_cast<void*>(current_->base));
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
    info("hot reload #{}: new code running, {} entities restored in {:.0f} ms", reloads_, entity_count,
         watch.milliseconds());
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
