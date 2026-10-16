// Fireflies: the game docs/tutorial.md builds, step by step. Walk around a dark field and catch fireflies.
//
//   WASD / arrows / left stick   move
//   Esc / gamepad East           back to the title screen
//   --play                       skip the title screen
//
// CI builds this file, so the tutorial's code always compiles against the current engine.

#include "bowl/app/main.hpp"
#include "bowl/gfx/particles.hpp"
#include "bowl/gfx/text.hpp"
#include "bowl/physics/collision.hpp"
#include "bowl/ui/ui.hpp"

#include <cmath>
#include <format>
#include <memory>
#include <vector>

using namespace bowl;

// Components

struct Position {
    Vec2 value;
    Vec2 previous; // value at the previous fixed tick, so drawing can interpolate
};
BOWL_COMPONENT(Position, value, previous)

struct Firefly {
    f32 heading = 0; // radians
    f32 turn = 0;    // radians per second
    f32 speed = 60;  // units per second
};
BOWL_COMPONENT(Firefly, heading, turn, speed)

struct Player {
    f32 radius = 16;
};
BOWL_COMPONENT(Player, radius)

// Resources

struct Arena {
    Rect bounds{-420, -240, 840, 480};
};

// A tiny deterministic random generator, stored as a resource so every scene shares it.
struct Random {
    u32 state = 12345;
    f32 next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<f32>(state >> 8) / 16777216.0f;
    }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * next(); }
};

constexpr int firefly_count = 14;
constexpr const char* best_key = "fireflies.best";

void spawn_firefly(ecs::World& world) {
    Random& rng = world.resource<Random>();
    const Rect& a = world.resource<Arena>().bounds;
    Vec2 at{rng.range(a.x, a.right()), rng.range(a.y, a.bottom())};
    world.create(Position{at, at}, Firefly{rng.range(0, tau), rng.range(-1.5f, 1.5f), rng.range(40, 110)});
}

std::unique_ptr<audio::Sound> make_chime() {
    std::vector<f32> mono(static_cast<usize>(audio::Sound::sample_rate * 0.25f));
    for (usize i = 0; i < mono.size(); ++i) {
        f32 t = static_cast<f32>(i) / audio::Sound::sample_rate;
        f32 envelope = std::exp(-t * 14.0f);
        mono[i] = (std::sin(tau * 1320.0f * t) + 0.5f * std::sin(tau * 1980.0f * t)) * envelope * 0.3f;
    }
    return audio::Sound::from_mono(mono);
}

class TitleScene;

class PlayScene final : public Scene {
public:
    void enter(Engine& engine) override {
        ecs::World& world = engine.world();
        font_ = engine.assets().load<gfx::Font>("engine/fonts/Roboto-Medium.ttf");
        chime_ = engine.assets().add("generated/chime", make_chime());
        best_ = engine.settings().get(best_key, 0);

        gfx::ParticleSettings sparks;
        sparks.rate = 40;
        sparks.lifetime_min = 0.3f;
        sparks.lifetime_max = 0.8f;
        sparks.speed_min = 10;
        sparks.speed_max = 70;
        sparks.spread = pi;
        sparks.drag = 2;
        sparks.size_start = 7;
        sparks.size_end = 1;
        sparks.color_start = {2.4f, 2.0f, 0.8f, 1};
        sparks.color_end = {1.0f, 0.5f, 0.2f, 0};
        player_ = world.create(Position{}, Player{}, gfx::ParticleEmitter{.settings = sparks, .emitting = false});
        for (int i = 0; i < firefly_count; ++i) spawn_firefly(world);

        engine.camera().position = {0, 0};
        gfx::PostSettings& post = engine.renderer().post();
        post.lighting = true;
        post.ambient = {0.12f, 0.14f, 0.26f, 1};
        post.bloom = true;
        post.vignette = 0.4f;
    }

    void exit(Engine& engine) override {
        // Fireflies and the player only exist while playing.
        std::vector<ecs::Entity> all;
        engine.world().each<const Position>([&](ecs::Entity e, const Position&) { all.push_back(e); });
        for (ecs::Entity e : all) engine.world().destroy(e);
        engine.renderer().post() = {};
    }

    void fixed_update(Engine& engine, f32 dt) override {
        ecs::World& world = engine.world();
        Position& me = world.get<Position>(player_);
        const f32 radius = world.get<Player>(player_).radius;

        // Move, sliding along the arena walls.
        Vec2 move = engine.input().vector("left", "right", "up", "down") * (280.0f * dt);
        moving_ = move.length() > 0;
        const Rect& a = world.resource<Arena>().bounds;
        auto walls = [&](const Rect&, FunctionRef<void(const Rect&)> emit) {
            emit({a.x - 64, a.y - 64, a.w + 128, 64}); // top
            emit({a.x - 64, a.bottom(), a.w + 128, 64}); // bottom
            emit({a.x - 64, a.y, 64, a.h});           // left
            emit({a.right(), a.y, 64, a.h});          // right
        };
        Rect box = Rect::from_center(me.value, Vec2{radius * 2});
        physics::move_and_slide(box, move, walls);
        me.value = box.center();

        // Catch every firefly within reach. Destroying while iterating has to wait, so it goes through deferred().
        int caught = 0;
        physics::Circle reach{me.value, radius + 6};
        auto& sparks = world.get<gfx::ParticleEmitter>(player_);
        world.each<const Position, const Firefly>([&](ecs::Entity e, const Position& p, const Firefly&) {
            if (!physics::contains(reach, p.value)) return;
            world.deferred().destroy(e);
            sparks.system.burst(sparks.settings, p.value, 30);
            ++caught;
        });
        for (int i = 0; i < caught; ++i) {
            spawn_firefly(world);
            engine.audio().play(chime_, {.gain = 0.5f, .pitch = world.resource<Random>().range(0.9f, 1.2f)});
        }
        score_ += caught;
        if (score_ > best_) best_ = score_;
    }

    void update(Engine& engine, f32 dt) override; // below TitleScene, which it switches to

    void render(Engine& engine) override {
        ecs::World& world = engine.world();
        gfx::Renderer& renderer = engine.renderer();
        const gfx::Texture& glow = renderer.light_texture();
        const Rect& arena = world.resource<Arena>().bounds;

        // The world: ground, fireflies (brighter than white, so they bloom) and the player.
        scene_.begin(engine.camera());
        scene_.draw_rect(arena, Color::hex(0x2f5a3a), -1e6f); // depth far below any y, so it's always underneath
        lights_.begin(engine.camera(), gfx::SpriteSort::None, gfx::BlendMode::Additive);
        f32 alpha = engine.alpha();
        world.each<const Position, const Firefly>([&](const Position& p, const Firefly&) {
            Vec2 at = lerp(p.previous, p.value, alpha);
            scene_.draw(glow, {.position = at, .size = {12, 12}, .depth = at.y, .color = {2.2f, 2.6f, 0.9f, 1}});
            lights_.draw(glow, {.position = at, .size = {170, 170}, .color = {0.45f, 0.55f, 0.2f, 1}});
        });
        Vec2 me = interpolated(engine, player_);
        f32 r = world.get<Player>(player_).radius;
        scene_.draw_rect(Rect::from_center(me, Vec2{r * 2}), Color::hex(0xffcf5c), me.y);
        lights_.draw(glow, {.position = me, .size = {420, 420}, .color = {0.75f, 0.68f, 0.55f, 1}});
        renderer.submit(scene_);
        renderer.submit_light(lights_);

        sparks_.begin(engine.camera(), gfx::SpriteSort::None, gfx::BlendMode::Additive);
        gfx::draw_particle_emitters(world, sparks_, glow);
        renderer.submit(sparks_);

        // The HUD, in screen coordinates and untouched by lighting.
        if (!font_) return;
        hud_.begin(gfx::Camera2D::screen(engine.window().size()), gfx::SpriteSort::None);
        gfx::TextStyle style{.size = 22, .raster_scale = engine.window().pixel_density()};
        gfx::draw_text(hud_, *font_, std::format("Caught {}    Best {}", score_, best_), {24, 20}, style);
        style.size = 15;
        style.color = Color::white().with_alpha(0.6f);
        gfx::draw_text(hud_, *font_, "WASD to move, Esc for the title screen", {24, 52}, style);
        renderer.submit_ui(hud_);
    }

private:
    Vec2 interpolated(Engine& engine, ecs::Entity e) const {
        const Position& p = engine.world().get<Position>(e);
        return lerp(p.previous, p.value, engine.alpha());
    }

    ecs::Entity player_;
    Asset<gfx::Font> font_;
    Asset<audio::Sound> chime_;
    gfx::SpriteBatch scene_, lights_, sparks_, hud_;
    int score_ = 0, best_ = 0;
    bool moving_ = false;
};

class TitleScene final : public Scene {
public:
    void enter(Engine& engine) override {
        ui_.style.font = engine.assets().load<gfx::Font>("engine/fonts/Roboto-Medium.ttf");
        best_ = engine.settings().get(best_key, 0);
    }

    void render(Engine& engine) override {
        Vec2 screen = engine.window().size();
        batch_.begin(gfx::Camera2D::screen(screen), gfx::SpriteSort::None);
        ui_.begin(batch_, engine.input(), screen, engine.delta(), engine.window().pixel_density());
        if (ui_.begin_panel("title", {screen.x * 0.5f - 170, screen.y * 0.5f - 130, 340, 0})) {
            ui_.label("Fireflies", ui_.style.accent, 34);
            ui_.label(best_ > 0 ? std::format("Best: {}", best_) : "Catch as many as you can.", ui_.style.text_dim);
            ui_.space(6);
            // While the fade runs, this screen is still drawn and clickable; one Play is enough.
            if (ui_.button("Play") && !engine.scenes().transitioning())
                engine.scenes().replace(std::make_unique<PlayScene>(), 0.3f);
            if (ui_.button("Quit")) engine.quit();
            ui_.end_panel();
        }
        ui_.end();
        engine.renderer().submit_ui(batch_);
    }

private:
    ui::Context ui_;
    gfx::SpriteBatch batch_;
    int best_ = 0;
};

void PlayScene::update(Engine& engine, f32 dt) {
    if (engine.input().pressed("back") && !engine.scenes().transitioning()) {
        engine.settings().set(best_key, best_);
        engine.scenes().replace(std::make_unique<TitleScene>(), 0.3f);
    }
    auto& sparks = engine.world().get<gfx::ParticleEmitter>(player_);
    sparks.origin = interpolated(engine, player_);
    sparks.emitting = moving_;
    gfx::update_particle_emitters(engine.world(), dt);
}

class FirefliesGame final : public Game {
public:
    AppConfig configure() override {
        AppConfig config;
        config.window.title = "Fireflies";
        return config;
    }

    void init(Engine& engine) override {
        Input& input = engine.input();
        input.bind("left", {InputBinding::key(Key::A), InputBinding::key(Key::Left), InputBinding::axis(GamepadAxis::LeftX, -1)});
        input.bind("right", {InputBinding::key(Key::D), InputBinding::key(Key::Right), InputBinding::axis(GamepadAxis::LeftX, +1)});
        input.bind("up", {InputBinding::key(Key::W), InputBinding::key(Key::Up), InputBinding::axis(GamepadAxis::LeftY, -1)});
        input.bind("down", {InputBinding::key(Key::S), InputBinding::key(Key::Down), InputBinding::axis(GamepadAxis::LeftY, +1)});
        input.bind("back", {InputBinding::key(Key::Escape), InputBinding::button(GamepadButton::East)});

        engine.world().set_resource<Arena>();
        engine.world().set_resource<Random>();

        // Systems run every fixed tick, before the scenes' fixed_update. These two touch only fireflies' own data,
        // so the scheduler spreads them over all cores.
        auto& fixed = engine.fixed_schedule();
        fixed.par_each<Position>("remember position", [](Position& p) { p.previous = p.value; });
        fixed.par_each<Position, Firefly>("wander", [](ecs::SystemContext& ctx, ecs::Entity, Position& p, Firefly& f) {
            const Rect& a = ctx.world.resource<Arena>().bounds;
            f.heading += f.turn * ctx.dt;
            p.value += Vec2::from_angle(f.heading) * (f.speed * ctx.dt);
            if (!a.contains(p.value)) { // turn around at the edge
                f.heading += pi;
                p.value = {clamp(p.value.x, a.x, a.right() - 1), clamp(p.value.y, a.y, a.bottom() - 1)};
            }
        }).reads_resource<Arena>();
    }

    void start(Engine& engine) override {
        if (engine.has_arg("--play")) engine.scenes().push(std::make_unique<PlayScene>());
        else engine.scenes().push(std::make_unique<TitleScene>());
    }
};

BOWL_MAIN(FirefliesGame)
