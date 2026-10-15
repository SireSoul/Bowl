#pragma once

#include "ph/core/types.hpp"
#include "ph/gfx/sprites.hpp"

#include <memory>
#include <vector>

namespace ph {

class Engine;

// One screen of a game: title, gameplay, inventory, pause menu... Scenes stack: the top one gets input and updates;
// scenes below keep drawing if the ones above are transparent (a pause menu over the paused game).
class Scene {
public:
    virtual ~Scene() = default;
    virtual void enter(Engine&) {}  // pushed or became the replacement
    virtual void exit(Engine&) {}   // popped or replaced
    virtual void pause(Engine&) {}  // another scene was pushed on top
    virtual void resume(Engine&) {} // the scene on top was popped
    virtual void fixed_update(Engine&, f32 /*dt*/) {}
    virtual void update(Engine&, f32 /*dt*/) {}
    virtual void render(Engine&) {}
    virtual void dev_ui(Engine&) {}
    // Draw the scene below this one too (overlays, menus over gameplay).
    virtual bool transparent() const { return false; }
    // Keep updating the scene below while this one is on top (e.g. a HUD-like overlay over live gameplay).
    virtual bool passes_updates() const { return false; }
};

// The engine owns one (engine.scenes()) and runs it after the Game's own callbacks, so a game can be all scenes:
//
//     void init(Engine& e) override { e.scenes().push(std::make_unique<TitleScene>()); }
//     ...
//     engine.scenes().replace(std::make_unique<GameplayScene>(), 0.4f); // fade out, swap, fade in
//
// Changes requested during a frame happen at the end of it, so a scene can safely replace itself.
class SceneStack : NonCopyable {
public:
    void push(std::unique_ptr<Scene> scene, f32 fade = 0);
    void pop(f32 fade = 0);
    void replace(std::unique_ptr<Scene> scene, f32 fade = 0);
    void clear();

    Scene* top() const { return scenes_.empty() ? nullptr : scenes_.back().get(); }
    usize size() const { return scenes_.size(); }
    bool empty() const { return scenes_.empty(); }
    bool transitioning() const { return fade_state_ != Fade::None; }
    Color fade_color = Color::black();

    // Driven by the App.
    void fixed_update(Engine& engine, f32 dt);
    void update(Engine& engine, f32 dt);
    void render(Engine& engine);
    void dev_ui(Engine& engine);
    void apply_pending(Engine& engine);
    void shutdown(Engine& engine);

private:
    enum class Op : u8 { Push, Pop, Replace, Clear };
    enum class Fade : u8 { None, Out, In };
    struct Pending {
        Op op;
        std::unique_ptr<Scene> scene;
        f32 fade;
    };
    void request(Op op, std::unique_ptr<Scene> scene, f32 fade);
    void run(Engine& engine, Pending& p);
    usize first_visible() const;

    std::vector<std::unique_ptr<Scene>> scenes_;
    std::vector<Pending> pending_;
    Fade fade_state_ = Fade::None;
    f32 fade_time_ = 0, fade_duration_ = 0;
    gfx::SpriteBatch fade_batch_;
};

} // namespace ph
