#pragma once

#include "ph/app/engine.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ph {

class AutoBuilder;

// Game-code hot reload (dev builds). The game is built as a loadable library and run by this host: when the library
// file changes (you rebuilt it), the host
//   1. keeps the world: entities, their ids, and every component whose code isn't in the game library. Of the game's
//      own components, plain data (trivially copyable) stays in place, savable ones are saved to come back through
//      the new code, and any others are dropped,
//   2. releases everything else made by the old code: systems, scenes, input bindings, resources, hooks, and the
//      assets and loaders registered since startup,
//   3. unloads the old library, loads the new one (if that fails, the old code keeps running), creates the new Game,
//      runs its init(), and puts the saved components back.
// A plain-data component whose size or alignment changed loses its old data. Use PH_COMPONENT for savable components
// so they're registered as soon as the library loads. State kept only in the Game object is rebuilt by init().
//
// With source_dirs and build_command set (ph_add_game does this), the host also rebuilds the library itself when a
// source file is saved, so editing code is enough. --no-auto-build turns that off.
class HotReloadHost : public Game {
public:
    struct Options {
        std::filesystem::path library;
        std::vector<std::filesystem::path> source_dirs; // watched recursively for C and C++ files
        std::vector<std::string> build_command;         // run when they change; empty: rebuild by hand
    };

    explicit HotReloadHost(Options options);
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
    struct SavedComponent {
        ecs::Entity entity;
        std::string name;
        json::Value data;
    };
    void bring_up(Engine& engine, const std::vector<SavedComponent>& saved, serial::Context& ctx);

    Options options_;
    std::unique_ptr<Library> current_;
    std::unique_ptr<Game> game_;
    std::unique_ptr<AutoBuilder> builder_;
    std::filesystem::file_time_type stamp_{};
    f64 last_check_ = 0, changed_at_ = -1;
    u64 asset_mark_ = 0;
    u32 reloads_ = 0, copies_ = 0;
};

// Watches source folders and runs a build command when a source file changes, on a background thread. Used by
// HotReloadHost; public so tools can reuse it.
class AutoBuilder : NonCopyable {
public:
    AutoBuilder(std::vector<std::filesystem::path> source_dirs, std::vector<std::string> command);
    ~AutoBuilder(); // waits for a running build

    // Call regularly from the main thread: notices changes (a few times a second), starts builds once files have
    // been quiet for a moment, and logs the outcome of finished builds.
    void poll();
    bool building() const;
    usize watched_files() const { return stamps_.size(); }

    // Whether a path is a C or C++ source or header, by extension.
    static bool is_source(const std::filesystem::path& path);

private:
    bool scan(); // true if anything changed since the last scan
    void start_build();

    struct Build;
    std::vector<std::filesystem::path> dirs_;
    std::vector<std::string> command_;
    std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> stamps_;
    std::unique_ptr<Build> build_;
    f64 last_scan_ = 0, changed_at_ = -1;
};

} // namespace ph
