// Sprites: a stress test and a tour of the engine.
//
//   left click / A button   spawn a burst of sprites at the cursor
//   C / Y button            clear
//   WASD / left stick       move the camera, mouse wheel zooms
//   F1                      dev overlay (profiler, systems, entities, assets, log, pause/step)
//
// Run with --frames N to quit after N frames (used for automated smoke runs).

#include "ph/app/main.hpp"
#include "ph/gfx/text.hpp"

#if PH_DEV_TOOLS
#include <imgui.h>
#endif

#include <cmath>
#include <format>
#include <vector>

using namespace ph;
using namespace ph::ecs;

// --- components: plain structs ---------------------------------------------------------------------------------------

struct Transform {
    Vec2 position;
    Vec2 previous; // position at the previous fixed tick, for interpolated rendering
    f32 rotation = 0;
};
struct Velocity {
    Vec2 value;
};
struct Spin {
    f32 speed = 0;
};
struct Look {
    Color color;
    f32 size = 16;
};
struct Lifetime {
    f32 seconds = 0;
};

// --- resources -------------------------------------------------------------------------------------------------------

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
        config.window.title = "PLACEHOLDERENGINENAME — sprites";
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

        spawn(engine, {0, 0}, 2000);
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
        if (!initialized_camera_) {
            camera.position = {0, 0};
            camera.zoom = 0.6f;
            initialized_camera_ = true;
        }
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
        gfx::draw_text(ui_, *font_, "PLACEHOLDERENGINENAME", {24, 20}, title);
        gfx::TextStyle body = title;
        body.size = 16;
        body.color = Color::hex(0xb8c0ff);
        std::string stats = std::format("{} sprites  ·  {} draw calls  ·  tick {}", engine.world().query<const Look>().count(),
                                        engine.renderer().stats().draw_calls, engine.tick());
        gfx::draw_text(ui_, *font_, stats, {24, 58}, body);
        gfx::TextStyle help = body;
        help.align = gfx::TextAlign::Center;
        help.max_width = 520;
        help.color = Color::hex(0xffffff, 0.7f);
        gfx::draw_text(ui_, *font_,
                       "Hold the left mouse button (or A) to spawn sprites. WASD or the left stick moves the camera, "
                       "the wheel zooms, C clears, F1 opens the dev overlay.",
                       {screen.x * 0.5f, screen.y - 46}, help);
        engine.renderer().submit(ui_);
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
    Asset<audio::Sound> blip_;
    gfx::SpriteBatch batch_, ui_;
    int spawn_rate_ = 40;
    bool initialized_camera_ = false;
    static constexpr u32 palette_[] = {0xff5c8a, 0xffb84d, 0xffe066, 0x5cd6a9, 0x4dc3ff, 0x9d7bff};
};

PH_MAIN(SpritesGame)
