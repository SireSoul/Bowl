#pragma once

#include "ph/app/engine.hpp"

#include <filesystem>
#include <memory>

namespace ph {

// Game-code hot reload (dev builds, macOS and Linux). The game is built as a loadable library and run by this host:
// when the library file changes (you rebuilt it), the host
//   1. loads the new library next to the old one (if that fails, the old code keeps running),
//   2. saves the world (every savable component) and releases everything made by the old code: systems, scenes,
//      resources, hooks, and the assets and loaders registered since startup,
//   3. unloads the old library, creates the new Game, runs its init(), and loads the world back.
// Entity ids change across a reload; references between saved entities are remapped. Use PH_COMPONENT for components
// so they're registered as soon as the library loads. State kept only in the Game object is rebuilt by init().
class HotReloadHost : public Game {
public:
    explicit HotReloadHost(std::filesystem::path library);
    ~HotReloadHost() override;

    AppConfig configure() override;
    void init(Engine& engine) override;
    void start(Engine& engine) override;
    void fixed_update(Engine& engine, f32 dt) override;
    void update(Engine& engine, f32 dt) override;
    void render(Engine& engine) override;
    void dev_ui(Engine& engine) override;
    void event(Engine& engine, const SDL_Event& e) override;
    void shutdown(Engine& engine) override;

    u32 reloads() const { return reloads_; }

private:
    struct Library;
    std::unique_ptr<Library> open_copy();
    bool reload(Engine& engine);
    void check(Engine& engine);
    void teardown(Engine& engine);
    void bring_up(Engine& engine, const json::Value& world, serial::Context& ctx);

    std::filesystem::path library_;
    std::unique_ptr<Library> current_;
    std::unique_ptr<Game> game_;
    std::filesystem::file_time_type stamp_{};
    f64 last_check_ = 0, changed_at_ = -1;
    u64 asset_mark_ = 0;
    u32 reloads_ = 0, copies_ = 0;
};

} // namespace ph
