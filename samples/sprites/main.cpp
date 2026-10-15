// Sprites: a stress test and a tour of the engine.
//
//   left click / A button   spawn a burst of sprites at the cursor
//   C / Y button            clear
//   WASD / left stick       move the camera, mouse wheel zooms
//   F1                      dev overlay (profiler, systems, entities, assets, log, pause/step)
//
// Run with --frames N to quit after N frames (used for automated smoke runs).

#include "ph/app/main.hpp"
#include "ph/gfx/sprite_sheet.hpp"
#include "ph/gfx/text.hpp"

#if PH_DEV_TOOLS
#include <imgui.h>
#endif

#include <cmath>
#include <format>
#include <vector>

using namespace ph;
using namespace ph::ecs;

// Components

// PH_COMPONENT makes a component savable, editable in the F1 inspector, and kept across game-code hot reloads.
struct Transform {
    Vec2 position;
    Vec2 previous; // position at the previous fixed tick, for interpolated rendering
    f32 rotation = 0;
};
PH_COMPONENT(Transform, position, previous, rotation)
struct Velocity {
    Vec2 value;
};
PH_COMPONENT(Velocity, value)
struct Spin {
    f32 speed = 0;
};
PH_COMPONENT(Spin, speed)
struct Look {
    Color color;
    f32 size = 16;
};
PH_COMPONENT(Look, color, size)
struct Lifetime {
    f32 seconds = 0;
};
PH_COMPONENT(Lifetime, seconds)
struct Banner {
    int slot = 0; // position in the row of animated sprites
};
PH_COMPONENT(Banner, slot)

// Resources

struct Arena {
    Rect bounds{-900, -500, 1800, 1000};
};

struct Rng {
    u64 state = 0x9E3779B97F4A7C15ull;
    u32 next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<u32>(state >> 32);
    }
    f32 uniform(f32 lo = 0, f32 hi = 1) { return lo + (hi - lo) * static_cast<f32>(next() % 1000000) / 1000000.0f; }
};

class SpritesGame final : public Game {
public:
    AppConfig configure() override {
        AppConfig config;
        config.window.title = "Bowl: sprites";
        return config;
    }

    void init(Engine& engine) override {
        engine.world().set_resource<Arena>();
        engine.world().set_resource<Rng>();

        Input& input = engine.input();
        input.bind("spawn", {InputBinding::mouse(MouseButton::Left), InputBinding::button(GamepadButton::South)});
        input.bind("clear", {InputBinding::key(Key::C), InputBinding::button(GamepadButton::North)});
        input.bind("left", {InputBinding::key(Key::A), InputBinding::key(Key::Left), InputBinding::axis(GamepadAxis::LeftX, -1)});
        input.bind("right", {InputBinding::key(Key::D), InputBinding::key(Key::Right), InputBinding::axis(GamepadAxis::LeftX, +1)});
        input.bind("up", {InputBinding::key(Key::W), InputBinding::key(Key::Up), InputBinding::axis(GamepadAxis::LeftY, -1)});
        input.bind("down", {InputBinding::key(Key::S), InputBinding::key(Key::Down), InputBinding::axis(GamepadAxis::LeftY, +1)});

        font_ = engine.assets().load<gfx::Font>("engine/fonts/Roboto-Medium.ttf");
        hologram_.fragment = engine.assets().load<gfx::Shader>("shaders/hologram.frag");
        sdf_look_ = gfx::sdf_text_material(engine.assets(), {.outline_color = Color::hex(0x1a1f3d), .outline = 0.25f});
        ball_ = engine.assets().add("generated/ball", make_ball_texture(engine.gpu()));
        blip_ = engine.assets().add("generated/blip", make_blip());

        // Fixed-rate simulation. Each of these runs in parallel across entities; the scheduler also runs systems with
        // disjoint data side by side.
        auto& fixed = engine.fixed_schedule();
        fixed.par_each<Transform>("remember previous", [](Transform& t) { t.previous = t.position; });
        fixed.barrier();
        fixed.par_each<Transform, Velocity>("integrate", [](SystemContext& ctx, Entity, Transform& t, Velocity& v) {
            const Rect& b = ctx.world.resource<Arena>().bounds;
            t.position += v.value * ctx.dt;
            if (t.position.x < b.x || t.position.x > b.right()) v.value.x = -v.value.x;
            if (t.position.y < b.y || t.position.y > b.bottom()) v.value.y = -v.value.y;
            t.position = {clamp(t.position.x, b.x, b.right()), clamp(t.position.y, b.y, b.bottom())};
            v.value.y += 400.0f * ctx.dt; // gravity
        }).reads_resource<Arena>();
        fixed.par_each<Transform, const Spin>("spin", [](SystemContext& ctx, Entity, Transform& t, const Spin& s) {
            t.rotation += s.speed * ctx.dt;
        });
        // Commands from parallel chunks are merged in a fixed order, so despawning stays deterministic.
        fixed.par_each<Lifetime>("expire", [](SystemContext& ctx, Entity e, Lifetime& life) {
            life.seconds -= ctx.dt;
            if (life.seconds <= 0) ctx.commands.destroy(e);
        });

        // Animated sprites: a procedural 8-frame sheet, stepped by a parallel system in the frame schedule.
        auto sheet = std::make_unique<gfx::SpriteSheet>(
            gfx::SpriteSheet::grid(engine.assets().add("generated/spinner", make_spinner_texture(engine.gpu())), 64,
                                   64, 8, 0.07f));
        sheet->animations.push_back({"bounce", 0, 7, gfx::SpriteAnimation::Direction::PingPong});
        spinner_ = engine.assets().add("generated/spinner.sheet", std::move(sheet));
        // Banners get their animator here rather than in start(): animators aren't saved, so after a game-code hot
        // reload this puts them back.
        engine.frame_schedule().each<const Banner>(
            "animate banners", [this](SystemContext& ctx, Entity e, const Banner& b) {
                gfx::SpriteAnimator anim;
                anim.sheet = spinner_;
                anim.play(b.slot % 2 ? "bounce" : "default");
                anim.speed = 0.6f + 0.2f * static_cast<f32>(b.slot);
                ctx.commands.add<gfx::SpriteAnimator>(e, anim);
            },
            ecs::without<gfx::SpriteAnimator>());
        engine.frame_schedule().par_each<gfx::SpriteAnimator>(
            "animate", [](SystemContext& ctx, Entity, gfx::SpriteAnimator& a) { a.update(ctx.dt); });
    }

    void start(Engine& engine) override {
        engine.camera().position = {0, 0};
        engine.camera().zoom = 0.6f;
        spawn(engine, {0, 0}, 2000);
        for (int i = 0; i < 5; ++i) engine.world().create(Banner{i});
    }

    void fixed_update(Engine& engine, f32) override {
        Input& input = engine.input();
        if (input.down("spawn")) spawn(engine, engine.camera().screen_to_world(input.mouse_position()), spawn_rate_);
        if (input.pressed("clear")) {
            std::vector<Entity> all;
            engine.world().each<const Transform>([&](Entity e, const Transform&) { all.push_back(e); });
            for (Entity e : all) engine.world().destroy(e);
        }
    }

    void update(Engine& engine, f32 dt) override {
        Input& input = engine.input();
        gfx::Camera2D& camera = engine.camera();
        camera.position += input.vector("left", "right", "up", "down") * (600.0f * dt / camera.zoom);
        if (input.wheel() != 0) camera.zoom = clamp(camera.zoom * std::pow(1.1f, input.wheel()), 0.1f, 8.0f);
    }

    void render(Engine& engine) override {
        batch_.begin(engine.camera());
        const gfx::Texture& ball = *ball_;
        f32 alpha = engine.alpha();
        engine.world().each<const Transform, const Look>([&](const Transform& t, const Look& look) {
            gfx::SpriteInstance s;
            s.position = lerp(t.previous, t.position, alpha); // smooth at any frame rate
            s.size = {look.size, look.size};
            s.rotation = t.rotation;
            s.color = look.color;
            s.depth = s.position.y;
            batch_.draw(ball, s);
        });
        const Rect& b = engine.world().resource<Arena>().bounds;
        batch_.draw_rect({b.x - 4, b.bottom(), b.w + 8, 4}, Color::hex(0x6b7cff), 1e9f);
        engine.renderer().submit(batch_);

        // Screen-space HUD on its own camera, drawn after the world.
        if (!font_) return;
        Vec2 screen = engine.window().size();
        ui_.begin(gfx::Camera2D::screen(screen), gfx::SpriteSort::None);
        gfx::TextStyle title{.size = 28, .color = Color::white(), .raster_scale = engine.window().pixel_density()};
        title.shadow = {2, 2};
        gfx::draw_text(ui_, *font_, "Bowl", {24, 20}, title);
        gfx::TextStyle body = title;
        body.size = 16;
        body.color = Color::hex(0xb8c0ff);
        body.markup = true;
        body.time = static_cast<f32>(engine.time());
        std::string stats = std::format("[color=#ffffff]{}[/color] sprites  ·  [color=#ffffff]{}[/color] draw calls  ·  [wave]tick {}[/wave]", engine.world().query<const Look>().count(),
                                        engine.renderer().stats().draw_calls, engine.tick());
        gfx::draw_text(ui_, *font_, stats, {24, 58}, body);
        // The spinners use a custom material (assets/shaders/hologram.frag.glsl) with a time uniform.
        struct HologramParams {
            f32 time, pad[3];
        };
        hologram_.set_uniforms(HologramParams{static_cast<f32>(engine.time()), {}});
        holo_.begin(gfx::Camera2D::screen(screen), gfx::SpriteSort::None, gfx::BlendMode::Alpha,
                    hologram_.fragment ? &hologram_ : nullptr);
        engine.world().each<const Banner, const gfx::SpriteAnimator>([&](const Banner& banner, const gfx::SpriteAnimator& a) {
            Vec2 at{screen.x - 340 + 64.0f * static_cast<f32>(banner.slot), 48};
            Vec2 scale = banner.slot == 4 ? Vec2{-0.8f, 0.8f} : Vec2{0.8f, 0.8f}; // the last one is mirrored
            gfx::draw_frame(holo_, *a.sheet, a.frame, at, scale);
        });
        engine.renderer().submit_ui(holo_);
        gfx::TextStyle help = body;
        help.align = gfx::TextAlign::Center;
        help.max_width = 520;
        help.color = Color::hex(0xffffff, 0.7f);
        gfx::draw_text(ui_, *font_,
                       "Hold the left mouse button (or A) to spawn sprites. WASD or the left stick moves the camera, "
                       "the wheel zooms, C clears, F1 opens the dev overlay.",
                       {screen.x * 0.5f, screen.y - 46}, help);
        engine.renderer().submit_ui(ui_);

        // Distance-field text stays sharp while its size changes every frame.
        sdf_.begin(gfx::Camera2D::screen(screen), gfx::SpriteSort::None, gfx::BlendMode::Alpha,
                   sdf_look_.fragment ? &sdf_look_ : nullptr);
        gfx::TextStyle pulse{.size = 40 + 8 * std::sin(static_cast<f32>(engine.time()) * 2), .color = Color::hex(0xffe066)};
        pulse.sdf = true;
        gfx::draw_text(sdf_, *font_, "Bowl", {24, 92}, pulse);
        engine.renderer().submit_ui(sdf_);
    }

    void dev_ui(Engine& engine) override {
#if PH_DEV_TOOLS
        ImGui::SetNextWindowPos({450, 12}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Sprites sample");
        ImGui::Text("%zu sprites", engine.world().query<const Look>().count());
        ImGui::SliderInt("spawn per tick", &spawn_rate_, 1, 2000);
        if (ImGui::Button("Spawn 10k")) spawn(engine, {0, -200}, 10000);
        ImGui::End();
#else
        (void)engine;
#endif
    }

private:
    void spawn(Engine& engine, Vec2 at, int count) {
        World& world = engine.world();
        Rng& rng = world.resource<Rng>();
        for (int i = 0; i < count; ++i) {
            f32 angle = rng.uniform(0, tau);
            f32 speed = rng.uniform(50, 400);
            Color color = Color::hex(palette_[rng.next() % std::size(palette_)]);
            Entity e = world.create(Transform{at, at, 0}, Velocity{Vec2::from_angle(angle) * speed},
                                    Look{color, rng.uniform(6, 22)});
            if (rng.next() % 3 == 0) world.add<Spin>(e, rng.uniform(-6, 6));
            if (rng.next() % 4 == 0) world.add<Lifetime>(e, rng.uniform(2, 8));
        }
        engine.audio().play(blip_, {.gain = 0.25f, .pan = clamp(at.x / 900.0f, -1.0f, 1.0f), .pitch = rng.uniform(0.8f, 1.4f)});
    }

    static std::unique_ptr<gfx::Texture> make_ball_texture(gfx::GpuDevice& gpu) {
        constexpr u32 n = 64;
        std::vector<u8> pixels(n * n * 4);
        for (u32 y = 0; y < n; ++y)
            for (u32 x = 0; x < n; ++x) {
                f32 dx = (static_cast<f32>(x) + 0.5f) / n * 2 - 1, dy = (static_cast<f32>(y) + 0.5f) / n * 2 - 1;
                f32 d = std::sqrt(dx * dx + dy * dy);
                f32 a = saturate((1.0f - d) * 12.0f);            // anti-aliased disc
                f32 light = saturate(1.1f - (dx + 0.4f) * (dx + 0.4f) - (dy + 0.4f) * (dy + 0.4f)); // soft highlight
                u8* p = &pixels[(y * n + x) * 4];
                p[0] = p[1] = p[2] = static_cast<u8>(150 + 105 * light);
                p[3] = static_cast<u8>(a * 255);
            }
        return gpu.create_texture({.width = n, .height = n, .rgba8 = pixels.data(), .name = "ball"});
    }

    // Eight 64x64 frames of a three-armed pinwheel turning, in one 512x64 strip.
    static std::unique_ptr<gfx::Texture> make_spinner_texture(gfx::GpuDevice& gpu) {
        constexpr u32 cell = 64, frames = 8, w = cell * frames;
        std::vector<u8> pixels(w * cell * 4, 0);
        for (u32 f = 0; f < frames; ++f)
            for (u32 y = 0; y < cell; ++y)
                for (u32 x = 0; x < cell; ++x) {
                    f32 dx = (static_cast<f32>(x) + 0.5f) / cell * 2 - 1, dy = (static_cast<f32>(y) + 0.5f) / cell * 2 - 1;
                    f32 r = std::sqrt(dx * dx + dy * dy);
                    f32 angle = std::atan2(dy, dx) - tau * static_cast<f32>(f) / (frames * 3);
                    f32 arm = 0.5f + 0.5f * std::cos(angle * 3);
                    f32 a = saturate((0.9f - r) * 20) * saturate((arm - 0.55f) * 6 + (0.25f - r) * 8);
                    u8* p = &pixels[(y * w + f * cell + x) * 4];
                    p[0] = static_cast<u8>(255 * saturate(0.6f + 0.4f * dx));
                    p[1] = static_cast<u8>(255 * saturate(0.5f + 0.5f * r));
                    p[2] = 255;
                    p[3] = static_cast<u8>(a * 255);
                }
        return gpu.create_texture({.width = w, .height = cell, .rgba8 = pixels.data(), .name = "spinner"});
    }

    static std::unique_ptr<audio::Sound> make_blip() {
        std::vector<f32> mono(static_cast<usize>(audio::Sound::sample_rate * 0.08f));
        for (usize i = 0; i < mono.size(); ++i) {
            f32 t = static_cast<f32>(i) / audio::Sound::sample_rate;
            f32 env = std::exp(-t * 40.0f);
            mono[i] = std::sin(tau * (880.0f - 3000.0f * t) * t) * env * 0.6f;
        }
        return audio::Sound::from_mono(mono);
    }

    Asset<gfx::Texture> ball_;
    Asset<gfx::Font> font_;
    Asset<gfx::SpriteSheet> spinner_;
    gfx::Material hologram_;
    gfx::SpriteBatch holo_, sdf_;
    gfx::Material sdf_look_;
    Asset<audio::Sound> blip_;
    gfx::SpriteBatch batch_, ui_;
    int spawn_rate_ = 40;
    static constexpr u32 palette_[] = {0xff5c8a, 0xffb84d, 0xffe066, 0x5cd6a9, 0x4dc3ff, 0x9d7bff};
};

PH_MAIN(SpritesGame)
