#include "ph/app/scene.hpp"

#include "ph/app/engine.hpp"

#include <algorithm>

namespace ph {

void SceneStack::request(Op op, std::unique_ptr<Scene> scene, f32 fade) {
    pending_.push_back({op, std::move(scene), fade});
}

void SceneStack::push(std::unique_ptr<Scene> scene, f32 fade) { request(Op::Push, std::move(scene), fade); }
void SceneStack::pop(f32 fade) { request(Op::Pop, nullptr, fade); }
void SceneStack::replace(std::unique_ptr<Scene> scene, f32 fade) { request(Op::Replace, std::move(scene), fade); }
void SceneStack::clear() { request(Op::Clear, nullptr, 0); }

usize SceneStack::first_visible() const {
    if (scenes_.empty()) return 0;
    usize i = scenes_.size() - 1;
    while (i > 0 && scenes_[i]->transparent()) --i;
    return i;
}

void SceneStack::fixed_update(Engine& engine, f32 dt) {
    if (scenes_.empty() || fade_state_ == Fade::Out) return; // hold the world still while fading out
    // The top scene, plus any below it that the ones above let through.
    usize i = scenes_.size() - 1;
    while (i > 0 && scenes_[i]->passes_updates()) --i;
    for (; i < scenes_.size(); ++i) scenes_[i]->fixed_update(engine, dt);
}

void SceneStack::update(Engine& engine, f32 dt) {
    if (scenes_.empty()) return;
    usize i = scenes_.size() - 1;
    while (i > 0 && scenes_[i]->passes_updates()) --i;
    for (; i < scenes_.size(); ++i) scenes_[i]->update(engine, dt);
}

void SceneStack::render(Engine& engine) {
    for (usize i = first_visible(); i < scenes_.size(); ++i) scenes_[i]->render(engine);
    if (fade_state_ == Fade::None) return;
    f32 t = fade_duration_ > 0 ? std::clamp(fade_time_ / fade_duration_, 0.0f, 1.0f) : 1.0f;
    f32 alpha = fade_state_ == Fade::Out ? t : 1 - t;
    Vec2 screen = engine.window().size();
    fade_batch_.begin(gfx::Camera2D::screen(screen), gfx::SpriteSort::None);
    fade_batch_.draw_rect({0, 0, screen.x, screen.y}, fade_color.with_alpha(alpha));
    engine.renderer().submit_ui(fade_batch_);
}

void SceneStack::dev_ui(Engine& engine) {
    if (Scene* s = top()) s->dev_ui(engine);
}

void SceneStack::run(Engine& engine, Pending& p) {
    switch (p.op) {
    case Op::Push:
        if (Scene* below = top()) below->pause(engine);
        scenes_.push_back(std::move(p.scene));
        scenes_.back()->enter(engine);
        break;
    case Op::Pop:
        if (scenes_.empty()) break;
        scenes_.back()->exit(engine);
        scenes_.pop_back();
        if (Scene* below = top()) below->resume(engine);
        break;
    case Op::Replace:
        if (!scenes_.empty()) {
            scenes_.back()->exit(engine);
            scenes_.pop_back();
        }
        scenes_.push_back(std::move(p.scene));
        scenes_.back()->enter(engine);
        break;
    case Op::Clear:
        while (!scenes_.empty()) {
            scenes_.back()->exit(engine);
            scenes_.pop_back();
        }
        break;
    }
}

void SceneStack::apply_pending(Engine& engine) {
    f32 dt = engine.delta() > 0 ? engine.delta() : 1.0f / 60.0f;
    if (fade_state_ != Fade::None) fade_time_ += dt;

    // A fading change waits until the screen is fully covered, then happens, then fades back in.
    if (fade_state_ == Fade::Out) {
        if (fade_time_ < fade_duration_) return;
        run(engine, pending_.front());
        pending_.erase(pending_.begin());
        fade_state_ = Fade::In;
        fade_time_ = 0;
        return;
    }
    if (fade_state_ == Fade::In) {
        if (fade_time_ < fade_duration_) return;
        fade_state_ = Fade::None;
    }
    while (!pending_.empty()) {
        if (pending_.front().fade > 0) {
            fade_state_ = Fade::Out;
            fade_duration_ = pending_.front().fade;
            fade_time_ = 0;
            return;
        }
        run(engine, pending_.front());
        pending_.erase(pending_.begin());
    }
}

void SceneStack::shutdown(Engine& engine) {
    pending_.clear();
    while (!scenes_.empty()) {
        scenes_.back()->exit(engine);
        scenes_.pop_back();
    }
}

} // namespace ph
