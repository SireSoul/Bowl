#include "ph/app/hot_reload.hpp"

#include "ph/core/time.hpp"
#include "ph/ecs/world_io.hpp"

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#include <format>

namespace ph {

namespace fs = std::filesystem;

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

HotReloadHost::HotReloadHost(fs::path library) : library_(std::move(library)) {}

HotReloadHost::~HotReloadHost() {
    game_.reset(); // its code lives in the library: destroy it first
    current_.reset();
}

std::unique_ptr<HotReloadHost::Library> HotReloadHost::open_copy() {
#if defined(_WIN32)
    error("hot reload: not supported on Windows yet");
    return nullptr;
#else
    auto lib = std::make_unique<Library>();
    std::error_code ec;
    lib->copy = fs::temp_directory_path() / std::format("ph-hot-{}-{}{}", library_.stem().string(), copies_++,
                                                          library_.extension().string());
    fs::copy_file(library_, lib->copy, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error("hot reload: can't copy {}: {}", library_.string(), ec.message());
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
        error("hot reload: {} has no ph_create_game (is it built with PH_MAIN?)", library_.string());
        return nullptr;
    }
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(lib->create), &info)) lib->base = info.dli_fbase;
    return lib;
#endif
}

AppConfig HotReloadHost::configure() {
    std::error_code ec;
    stamp_ = fs::last_write_time(library_, ec);
    current_ = open_copy();
    if (!current_) return {};
    game_.reset(current_->create());
    return game_->configure();
}

void HotReloadHost::init(Engine& engine) {
    asset_mark_ = engine.assets().mark();
    if (game_) game_->init(engine);
    info("hot reload: watching {} (rebuild it to reload the game code)", library_.string());
}

void HotReloadHost::start(Engine& engine) {
    if (game_) game_->start(engine);
}

void HotReloadHost::check(Engine& engine) {
    f64 now = now_seconds();
    if (now - last_check_ < 0.25) return;
    last_check_ = now;
    std::error_code ec;
    auto stamp = fs::last_write_time(library_, ec);
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
