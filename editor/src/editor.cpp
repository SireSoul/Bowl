#include "editor.hpp"

#include "bowl/app/inspector.hpp"
#include "bowl/core/time.hpp"
#include "bowl/ecs/world_io.hpp"
#include "bowl/gfx/particles.hpp"
#include "bowl/gfx/sprite.hpp"
#include "bowl/gfx/tilemap.hpp"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_dialog.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <map>

namespace bowl::editor {

namespace fs = std::filesystem;

namespace {

const SDL_DialogFileFilter world_filter{"Bowl level", "world"};

std::string extension(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

bool is_image(const fs::path& p) {
    std::string ext = extension(p);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp";
}
bool is_map(const fs::path& p) { return extension(p) == ".tmj" || extension(p) == ".tmx"; }
bool is_prefab(const fs::path& p) { return extension(p) == ".prefab"; }

// The middle of a map cell, in map pixels.
Vec2 cell_center(const gfx::Tilemap& map, i32 x, i32 y) {
    if (map.orientation == gfx::Tilemap::Orientation::Isometric)
        return map.cell_to_world(static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f);
    return map.cell_to_world(static_cast<f32>(x), static_cast<f32>(y)) +
           Vec2{static_cast<f32>(map.tile_width), static_cast<f32>(map.tile_height)} * 0.5f;
}

// The corners of a map cell's outline, in map pixels: a diamond on isometric maps, a rectangle on the others.
std::array<Vec2, 4> cell_corners(const gfx::Tilemap& map, i32 x, i32 y) {
    f32 fx = static_cast<f32>(x), fy = static_cast<f32>(y);
    if (map.orientation == gfx::Tilemap::Orientation::Isometric)
        return {map.cell_to_world(fx, fy), map.cell_to_world(fx + 1, fy), map.cell_to_world(fx + 1, fy + 1),
                map.cell_to_world(fx, fy + 1)};
    Vec2 a = map.cell_to_world(fx, fy), size{static_cast<f32>(map.tile_width), static_cast<f32>(map.tile_height)};
    return {a, a + Vec2{size.x, 0}, a + size, a + Vec2{0, size.y}};
}

void line(gfx::SpriteBatch& batch, Vec2 a, Vec2 b, f32 thickness, Color color) {
    gfx::SpriteInstance s;
    s.position = (a + b) * 0.5f;
    s.size = {(b - a).length(), thickness};
    s.rotation = std::atan2(b.y - a.y, b.x - a.x);
    s.color = color;
    batch.draw(nullptr, s);
}

// The entity and everything below it, parents first.
void subtree(ecs::World& world, ecs::Entity e, std::vector<ecs::Entity>& out) {
    out.push_back(e);
    if (const ecs::Children* c = world.try_get<const ecs::Children>(e)) {
        std::vector<ecs::Entity> list = c->list;
        for (ecs::Entity child : list)
            if (world.alive(child)) subtree(world, child, out);
    }
}

// After loading copies, links to entities outside the copy are null: drop those Parent components.
void drop_dangling_parents(ecs::World& world, const std::vector<ecs::Entity>& entities) {
    for (ecs::Entity e : entities)
        if (const ecs::Parent* p = world.try_get<const ecs::Parent>(e); p && !world.alive(p->entity))
            world.remove<ecs::Parent>(e);
}

} // namespace

std::string pretty_name(std::string_view type) {
    static const std::map<std::string_view, std::string_view> known = {
        {"bowl::ecs::Transform2D", "Transform"},        {"bowl::ecs::GlobalTransform2D", "World transform"},
        {"bowl::gfx::Sprite", "Art"},                   {"bowl::gfx::ParticleEmitter", "Particles"},
        {"bowl::ecs::Name", "Name"},                    {"bowl::ecs::Parent", "Parent"},
        {"bowl::ecs::Children", "Children"},
    };
    if (auto it = known.find(type); it != known.end()) return std::string(it->second);
    auto colon = type.rfind("::");
    std::string_view last = colon == std::string_view::npos ? type : type.substr(colon + 2);
    // "SpawnPoint" reads as "Spawn point".
    std::string out;
    for (usize i = 0; i < last.size(); ++i) {
        char c = last[i];
        bool upper = std::isupper(static_cast<unsigned char>(c));
        if (upper && i > 0 && std::islower(static_cast<unsigned char>(last[i - 1]))) {
            out += ' ';
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            out += c;
        }
    }
    return out;
}

AppConfig Editor::configure() {
    AppConfig config;
    config.window.title = "Bowl Editor";
    config.window.width = 1600;
    config.window.height = 940;
    config.application = "Bowl Editor";
    config.dev_ui_always = true;
    config.dev_link = false; // the editor attaches to games; nothing attaches to it
    return config;
}

void Editor::init(Engine& engine) {
    doc_ = std::make_unique<Document>(engine);
    assets_ = &engine.assets();
    engine_ = &engine;
    ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard; // the keys belong to the editor's shortcuts
    ui::apply_theme();
    // Engine components, so they can be added from the inspector before anything uses them.
    ecs::register_components<ecs::Name, ecs::Transform2D, ecs::GlobalTransform2D, ecs::Parent, ecs::Children,
                             gfx::Sprite, gfx::ParticleEmitter, gfx::Map>();

    fs::path file;
    std::string game;
    const auto& args = engine.args();
    for (usize i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        bool has_value = i + 1 < args.size();
        if (a == "--assets" && has_value) assets_dir_ = args[++i];
        else if (a == "--game" && has_value) game = args[++i];
        else if (a == "--select" && has_value) select_name_ = args[++i];
        else if (a == "--resave") resave_ = true;
        else if (a == "--attach") {
            attach_first_ = true;
            workspace_ = Workspace::Game;
            if (has_value && !args[i + 1].starts_with("--")) {
                const std::string& pane = args[++i];
                game_pane_ = pane == "systems" ? 1 : pane == "entities" ? 2 : 0;
            }
        }
        else if (a == "--paint" && has_value && !args[i + 1].starts_with("--")) ++i; // read below
        else if (a == "--content") { // its collection and record are read below
            for (int k = 0; k < 2 && i + 1 < args.size() && !args[i + 1].starts_with("--"); ++k) ++i;
        }
        else if ((a == "--frames" || a == "--screenshot" || a == "--inspect" || a == "--driver" || a == "--lang") && has_value)
            ++i; // the engine's own flags
        else if (!a.starts_with("--")) file = a;
    }

    // Assets: --assets, or the "assets" folder the level lives in, or ./assets.
    if (assets_dir_.empty() && !file.empty()) {
        for (fs::path p = fs::absolute(file).parent_path(); !p.empty() && p != p.parent_path(); p = p.parent_path())
            if (p.filename() == "assets") {
                assets_dir_ = p;
                break;
            }
    }
    if (assets_dir_.empty()) assets_dir_ = fs::exists("assets") ? fs::path("assets") : fs::current_path();
    assets_dir_ = fs::absolute(assets_dir_);
    engine.assets().mount("", assets_dir_);

    if (!game.empty()) {
        // Loading the library runs its BOWL_COMPONENT registrations. It stays loaded: component code lives in it.
        // On Windows this is <game>_editor_code.dll, the copy built against the editor.
#if defined(_WIN32)
        game_library_ = LoadLibraryW(fs::absolute(game).c_str());
        if (game_library_) info("editor: loaded the game's components from {}", game);
        else error("editor: can't load {} (Windows error {}); use <game>_editor_code.dll", game, GetLastError());
#else
        game_library_ = dlopen(fs::absolute(game).c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (game_library_) info("editor: loaded the game's components from {}", game);
        else error("editor: can't load {}: {}", game, dlerror());
#endif
    }

    Settings& prefs = engine.settings();
    show_grid_ = prefs.get("editor.grid", true);
    snap_ = prefs.get("editor.snap", true);
    grid_ = prefs.get("editor.grid_size", 16.0f);
    show_welcome_ = !prefs.get("editor.welcome_seen", false) && !resave_;

    if (file.empty() || !doc_->open(file)) doc_->clear();
    if (resave_) { // write the file back in the current format and quit, without showing anything
        if (!doc_->file().empty()) doc_->save();
        engine.quit();
        return;
    }
    recovery_ = doc_->pending_autosave();
    engine.renderer().set_clear_color(Color::hex(0x1d1f24));
    scan_art();
    scan_data();
    content_.init(engine, assets_dir_, [this](std::string m) { say(std::move(m)); });
    paint_.init(engine, assets_dir_, [this](std::string m) { say(std::move(m)); });
    content_.hooks.pictures = [this] { return paint_.pictures(); };
    content_.hooks.open_picture = [this](const std::string& p) {
        if (paint_.open(p)) workspace_ = Workspace::Paint;
    };
    content_.hooks.open_text = [this](const std::string& p) {
        scan_data();
        data_open_path(p);
        workspace_ = Workspace::Data;
    };
    content_.hooks.go = [this](int w) {
        static constexpr Workspace order[] = {Workspace::Content, Workspace::Paint, Workspace::Level, Workspace::Data, Workspace::Game};
        workspace_ = order[std::clamp(w, 0, 4)];
    };
    // The Level tab is for games that lay out levels: shown when there are .world files (or one was opened).
    {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(assets_dir_, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
            if (!ec && it->path().extension() == ".world") {
                has_levels_ = true;
                break;
            }
        if (!file.empty()) has_levels_ = true, workspace_ = Workspace::Level;
    }
    // --content [collection [record id]]: start in the Content workspace there.
    for (usize i = 0; i < args.size(); ++i)
        if (args[i] == "--content") {
            workspace_ = Workspace::Content;
            std::string coll = i + 1 < args.size() && !args[i + 1].starts_with("--") ? args[i + 1] : "";
            std::string rec = !coll.empty() && i + 2 < args.size() && !args[i + 2].starts_with("--") ? args[i + 2] : "";
            if (!coll.empty()) content_.open(coll, rec);
        }
    // --data / --paint: start in that workspace with its first file open.
    if (engine.has_arg("--data")) {
        workspace_ = Workspace::Data;
        if (!data_files_.empty()) data_open(0);
    }
    // --paint [picture]: that picture, or the first.
    for (usize i = 0; i < args.size(); ++i)
        if (args[i] == "--paint") {
            workspace_ = Workspace::Paint;
            bool named = i + 1 < args.size() && !args[i + 1].starts_with("--");
            if (named) paint_.open(args[i + 1]);
            else if (!paint_.pictures().empty()) paint_.open(paint_.pictures()[0]);
        }
}

void Editor::data_open_path(const std::string& path) {
    for (usize i = 0; i < data_files_.size(); ++i)
        if (data_files_[i].path == path) data_open(i);
}

bool Editor::unsaved_anywhere() const {
    return (doc_ && doc_->modified() && !doc_->file().empty()) || content_.unsaved() || paint_.unsaved() ||
           (data_index_ && data_doc_.text() != data_saved_);
}

void Editor::save_everything(Engine& engine) {
    if (doc_ && doc_->modified() && !doc_->file().empty()) save(engine, false);
    if (content_.unsaved()) content_.save_all();
    if (paint_.unsaved()) paint_.save();
    if (data_index_ && data_doc_.text() != data_saved_) data_save();
}

void Editor::shutdown(Engine& engine) {
    if (!doc_ || resave_) return;
    doc_->write_autosave(); // unsaved work is offered back next time
    if (content_.unsaved() && content_.save_all()) info("editor: saved the content changes on the way out");
    if (paint_.unsaved() && paint_.save()) info("editor: saved the picture being painted on the way out");
    Settings& prefs = engine.settings();
    prefs.set("editor.grid", show_grid_);
    prefs.set("editor.snap", snap_);
    prefs.set("editor.grid_size", grid_);
}

// Update

void Editor::update(Engine& engine, f32) {
    {
        std::optional<fs::path> open_path, save_path;
        {
            std::lock_guard lock(dialogs_->mutex);
            open_path.swap(dialogs_->open);
            save_path.swap(dialogs_->save);
        }
        if (open_path) open(engine, *open_path);
        if (save_path) {
            fs::path p = *save_path;
            if (p.extension() != ".world") p += ".world";
            doc_->track(selection_);
            if (doc_->save_as(p)) say("Saved " + p.filename().string());
            scan_art();
        }
    }

    ecs::World& world = engine.world();
    std::erase_if(selection_, [&](ecs::Entity e) { return !world.alive(e); });
    ecs::propagate_transforms(world);
    if (frame_pending_ && free_area_.w > 0) {
        frame_pending_ = false;
        frame(engine, false);
        if (!select_name_.empty()) {
            for (ecs::Entity e : doc_->entities())
                if (const auto* n = world.try_get<const ecs::Name>(e); n && n->value == select_name_) select_only(e);
            select_name_.clear();
        }
    }
    // Ctrl+1 to 5 switch workspaces from anywhere.
    {
        Input& in = engine.input();
        bool cmd = in.down(Key::LeftCtrl) || in.down(Key::RightCtrl) || in.down(Key::LeftSuper) || in.down(Key::RightSuper);
        if (cmd && in.pressed(Key::Num1)) workspace_ = Workspace::Content;
        if (cmd && in.pressed(Key::Num2)) workspace_ = Workspace::Paint;
        if (cmd && in.pressed(Key::Num3)) workspace_ = Workspace::Level;
        if (cmd && in.pressed(Key::Num4)) workspace_ = Workspace::Game;
        if (cmd && in.pressed(Key::Num5)) workspace_ = Workspace::Data;
        if (cmd && in.pressed(Key::K) && !content_.searching()) content_.open_search(); // find anything, from anywhere
    }
    if (workspace_ == Workspace::Level) {
        scene_mouse(engine);
        shortcuts(engine);
    }
    ecs::propagate_transforms(world);

    // Record an undo step once an edit has settled: nothing being dragged or typed into.
    f64 now = now_seconds();
    bool busy = drag_ != Drag::None || ImGui::IsAnyItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (!busy && now - last_track_ > 0.25) {
        last_track_ = now;
        doc_->track(selection_);
    }
    update_title(engine);
}

Vec2 Editor::mouse_world(Engine& engine) const {
    return engine.camera().screen_to_world(engine.input().mouse_position());
}

Vec2 Editor::snapped(Vec2 p, bool force_free) const {
    if (!snap_ || force_free) return p;
    Vec2 origin;
    if (const gfx::Tilemap* map = snap_map(origin)) {
        auto [x, y] = map->cell_at(p - origin);
        return origin + cell_center(*map, x, y);
    }
    return {std::round(p.x / grid_) * grid_, std::round(p.y / grid_) * grid_};
}

const gfx::Tilemap* Editor::snap_map(Vec2& origin) const {
    if (!snap_to_map_ || !engine_) return nullptr;
    const ecs::World& world = engine_->world();
    const gfx::Tilemap* found = nullptr;
    for (ecs::Entity e : doc_->entities()) {
        const gfx::Map* m = world.try_get<const gfx::Map>(e);
        const ecs::GlobalTransform2D* g = world.try_get<const ecs::GlobalTransform2D>(e);
        if (!m || !g || !m->map || hidden(world, e)) continue;
        found = &*m->map;
        origin = g->position;
        break;
    }
    return found;
}

bool Editor::hidden(const ecs::World& world, ecs::Entity e) const {
    for (int depth = 0; e && depth < 64; ++depth) {
        if (const EditorFlags* f = world.try_get<const EditorFlags>(e); f && f->hidden) return true;
        const ecs::Parent* p = world.try_get<const ecs::Parent>(e);
        e = p ? p->entity : ecs::null_entity;
    }
    return false;
}

bool Editor::locked(const ecs::World& world, ecs::Entity e) const {
    for (int depth = 0; e && depth < 64; ++depth) {
        if (const EditorFlags* f = world.try_get<const EditorFlags>(e); f && f->locked) return true;
        const ecs::Parent* p = world.try_get<const ecs::Parent>(e);
        e = p ? p->entity : ecs::null_entity;
    }
    return false;
}

void Editor::set_hidden(Engine& engine, bool on) {
    ecs::World& world = engine.world();
    for (ecs::Entity e : selection_) {
        EditorFlags* f = world.try_get<EditorFlags>(e);
        if (!f) f = &world.add<EditorFlags>(e);
        f->hidden = on;
    }
    if (on) selection_.clear(); // what can't be seen isn't left selected
}

void Editor::set_locked(Engine& engine, bool on) {
    ecs::World& world = engine.world();
    for (ecs::Entity e : selection_) {
        EditorFlags* f = world.try_get<EditorFlags>(e);
        if (!f) f = &world.add<EditorFlags>(e);
        f->locked = on;
    }
}

bool Editor::selected(ecs::Entity e) const { return std::find(selection_.begin(), selection_.end(), e) != selection_.end(); }

void Editor::select_only(ecs::Entity e) { selection_ = e ? Selection{e} : Selection{}; }

void Editor::toggle_selected(ecs::Entity e) {
    auto it = std::find(selection_.begin(), selection_.end(), e);
    if (it == selection_.end()) selection_.push_back(e);
    else selection_.erase(it);
}

std::vector<ecs::Entity> Editor::top_level_selection(Engine& engine) const {
    ecs::World& world = engine.world();
    std::vector<ecs::Entity> out;
    for (ecs::Entity e : selection_) {
        bool inside_selected = false;
        for (const ecs::Parent* p = world.try_get<const ecs::Parent>(e); p && world.alive(p->entity);
             p = world.try_get<const ecs::Parent>(p->entity))
            if (selected(p->entity)) inside_selected = true;
        if (!inside_selected) out.push_back(e);
    }
    return out;
}

std::optional<Rect> Editor::bounds_of(Engine& engine, ecs::Entity e) {
    ecs::World& world = engine.world();
    if (auto b = gfx::sprite_bounds(world, e)) return b;
    const gfx::Map* m = world.try_get<const gfx::Map>(e);
    const ecs::GlobalTransform2D* at = world.try_get<const ecs::GlobalTransform2D>(e);
    if (m && at && m->map && !m->map->infinite && m->map->width > 0) { // around its corners
        const gfx::Tilemap& map = *m->map;
        Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
        auto take = [&](Vec2 c) { lo = {std::min(lo.x, c.x), std::min(lo.y, c.y)}, hi = {std::max(hi.x, c.x), std::max(hi.y, c.y)}; };
        for (auto [cx, cy] : {std::pair{0u, 0u}, {map.width, 0u}, {map.width, map.height}, {0u, map.height}})
            take(map.cell_to_world(static_cast<f32>(cx), static_cast<f32>(cy)));
        if (map.orientation != gfx::Tilemap::Orientation::Isometric) take(map.pixel_size());
        return Rect{at->position.x + lo.x, at->position.y + lo.y, hi.x - lo.x, hi.y - lo.y};
    }
    if (const auto* g = world.try_get<const ecs::GlobalTransform2D>(e)) return Rect{g->position.x, g->position.y, 0, 0};
    return std::nullopt;
}

ecs::Entity Editor::pick(Engine& engine, Vec2 p) {
    ecs::World& world = engine.world();
    ecs::Entity best;
    f32 best_depth = -1e30f;
    world.each<const gfx::Sprite, const ecs::GlobalTransform2D>(
        [&](ecs::Entity e, const gfx::Sprite& s, const ecs::GlobalTransform2D& g) {
            auto bounds = gfx::sprite_bounds(world, e);
            if (!s.visible || !bounds || !bounds->contains(p) || hidden(world, e) || locked(world, e)) return;
            f32 depth = (s.y_sort ? g.position.y : 0.0f) + s.depth;
            if (!best || depth >= best_depth) {
                best = e;
                best_depth = depth;
            }
        });
    if (best) return best;
    // Entities that draw nothing are picked by their marker.
    f32 reach = 9.0f / engine.camera().zoom;
    world.each<const ecs::GlobalTransform2D>([&](ecs::Entity e, const ecs::GlobalTransform2D& g) {
        if (!world.has<gfx::Sprite>(e) && !world.has<gfx::Map>(e) && (g.position - p).length() <= reach &&
            !hidden(world, e) && !locked(world, e))
            best = e;
    });
    if (best) return best;
    // Last, a map under the mouse (most start locked, so they stay out of the way).
    for (ecs::Entity e : doc_->entities())
        if (world.has<gfx::Map>(e) && !hidden(world, e) && !locked(world, e))
            if (auto b = bounds_of(engine, e); b && b->contains(p)) best = e;
    return best;
}

std::vector<ecs::Entity> Editor::pick_box(Engine& engine, Rect area) {
    std::vector<ecs::Entity> out;
    for (ecs::Entity e : doc_->entities()) {
        if (hidden(engine.world(), e) || locked(engine.world(), e)) continue;
        auto b = bounds_of(engine, e);
        if (!b) continue;
        bool point = b->w == 0 && b->h == 0;
        if (point ? area.contains(b->position()) : area.overlaps(*b)) out.push_back(e);
    }
    return out;
}

void Editor::scene_mouse(Engine& engine) {
    Input& in = engine.input();
    const ImGuiIO& io = ImGui::GetIO();
    gfx::Camera2D& cam = engine.camera();
    bool over_ui = io.WantCaptureMouse;
    bool space = in.down(Key::Space) && !io.WantCaptureKeyboard;
    bool alt = in.down(Key::LeftAlt) || in.down(Key::RightAlt);
    bool shift = in.down(Key::LeftShift) || in.down(Key::RightShift);
    Vec2 m = mouse_world(engine);

    if (drag_ == Drag::Pan) {
        if (!in.down(MouseButton::Left) && !in.down(MouseButton::Middle) && !in.down(MouseButton::Right)) drag_ = Drag::None;
        else cam.position -= in.mouse_delta() / cam.zoom;
        return;
    }
    if (!over_ui && in.wheel() != 0) { // zoom toward the cursor
        Vec2 before = m;
        cam.zoom = clamp(cam.zoom * std::pow(1.15f, in.wheel()), 0.05f, 32.0f);
        cam.position += before - mouse_world(engine);
        m = mouse_world(engine);
    }
    if (!over_ui && (in.pressed(MouseButton::Middle) || in.pressed(MouseButton::Right) ||
                     ((tool_ == Tool::Pan || space) && in.pressed(MouseButton::Left)))) {
        drag_ = Drag::Pan;
        return;
    }
    hovered_ = over_ui ? ecs::null_entity : pick(engine, m);

    if (tool_ == Tool::Place) {
        if (!over_ui && in.pressed(MouseButton::Left)) create(engine, snapped(m, alt), place_art_);
        return;
    }
    if (tool_ == Tool::Brush) {
        brush_mouse(engine);
        return;
    }
    // Region shapes: drawing new ones, and the handles on a selected one (which come before selecting).
    if (region_mouse(engine) || tool_ != Tool::Select) return;

    ecs::World& world = engine.world();
    if (!over_ui && in.pressed(MouseButton::Left)) {
        ecs::Entity e = hovered_;
        if (e) {
            if (shift) toggle_selected(e);
            else if (!selected(e)) select_only(e);
            if (!shift) {
                if (alt) { // Alt-drag leaves the originals and moves copies
                    drag_copied_ = true;
                    duplicate(engine);
                }
                drag_ = Drag::Move;
                drag_start_world_ = m;
                moving_.clear();
                for (ecs::Entity t : top_level_selection(engine)) {
                    const auto* local = world.try_get<const ecs::Transform2D>(t);
                    const auto* global = world.try_get<const ecs::GlobalTransform2D>(t);
                    if (local && global) moving_.push_back({t, local->position, global->position});
                }
            }
        } else {
            if (!shift) selection_.clear();
            drag_ = Drag::Box;
            drag_start_world_ = m;
        }
    }

    if (drag_ == Drag::Move) {
        if (!in.down(MouseButton::Left)) {
            drag_ = Drag::None;
            drag_copied_ = false;
            return;
        }
        if (moving_.empty()) return;
        // The first thing moved snaps to the grid; everything else keeps its distance from it.
        Vec2 delta = m - drag_start_world_;
        delta = snapped(moving_[0].global + delta, alt && !drag_copied_) - moving_[0].global;
        for (const Moving& mv : moving_) {
            if (!world.alive(mv.e)) continue;
            Vec2 d = delta;
            if (const ecs::Parent* p = world.try_get<const ecs::Parent>(mv.e)) // a child moves in its parent's space
                if (const auto* g = world.try_get<const ecs::GlobalTransform2D>(p->entity)) {
                    d = d.rotated(-g->rotation);
                    d = {g->scale.x != 0 ? d.x / g->scale.x : d.x, g->scale.y != 0 ? d.y / g->scale.y : d.y};
                }
            world.get<ecs::Transform2D>(mv.e).position = mv.local + d;
        }
    } else if (drag_ == Drag::Box && !in.down(MouseButton::Left)) {
        drag_ = Drag::None;
        Vec2 a = drag_start_world_;
        Rect area{std::min(a.x, m.x), std::min(a.y, m.y), std::abs(m.x - a.x), std::abs(m.y - a.y)};
        if (area.w * cam.zoom > 3 || area.h * cam.zoom > 3)
            for (ecs::Entity e : pick_box(engine, area))
                if (!selected(e)) selection_.push_back(e);
    }
}

void Editor::shortcuts(Engine& engine) {
    if (ImGui::GetIO().WantCaptureKeyboard) return; // typing in a field
    Input& in = engine.input();
    bool cmd = in.down(Key::LeftCtrl) || in.down(Key::RightCtrl) || in.down(Key::LeftSuper) || in.down(Key::RightSuper);
    bool shift = in.down(Key::LeftShift) || in.down(Key::RightShift);
    auto key = [&](Key k) { return in.pressed(k); };

    if (key(Key::F1)) show_help_ = !show_help_;
    if (cmd) {
        if (key(Key::Z)) shift ? redo(engine) : undo(engine);
        else if (key(Key::Y)) redo(engine);
        else if (key(Key::S)) save(engine, shift);
        else if (key(Key::O)) open_dialog(engine);
        else if (key(Key::N)) new_level(engine);
        else if (key(Key::D)) duplicate(engine);
        else if (key(Key::C)) copy_selection(engine);
        else if (key(Key::X)) {
            copy_selection(engine);
            remove_selection(engine);
        } else if (key(Key::V)) paste(engine);
        else if (key(Key::A)) selection_ = doc_->entities();
        else if (key(Key::G)) shift ? ungroup_selection(engine) : group_selection(engine);
        return;
    }
    if (key(Key::V)) tool_ = Tool::Select;
    if (key(Key::P)) tool_ = Tool::Place;
    if (key(Key::H)) tool_ = Tool::Pan;
    if (key(Key::B)) tool_ = Tool::Brush;
    if (key(Key::R)) tool_ = Tool::Region;
    if (key(Key::C)) show_colliders_ = !show_colliders_;
    if (key(Key::G)) {
        if (shift) snap_ = !snap_;
        else show_grid_ = !show_grid_;
    }
    if (key(Key::F5)) {
        scan_art();
        say("Looked for new art");
    }
    if (key(Key::Escape)) {
        if (tool_ != Tool::Select) tool_ = Tool::Select;
        else selection_.clear();
    }
    if (key(Key::Delete) || key(Key::Backspace)) remove_selection(engine);
    if (key(Key::F)) frame(engine, !selection_.empty());
    if (key(Key::Home)) frame(engine, false);
    f32 step = shift ? grid_ : 1.0f;
    if (key(Key::Left)) nudge(engine, {-step, 0});
    if (key(Key::Right)) nudge(engine, {step, 0});
    if (key(Key::Up)) nudge(engine, {0, -step});
    if (key(Key::Down)) nudge(engine, {0, step});
    if (key(Key::Q)) rotate_selection(engine, shift ? -1.0f : -15.0f);
    if (key(Key::E)) rotate_selection(engine, shift ? 1.0f : 15.0f);
}

// Render

void Editor::render(Engine& engine) {
    if (workspace_ == Workspace::Paint) {
        paint_.render(engine);
        return;
    }
    if (workspace_ == Workspace::Data || workspace_ == Workspace::Game || workspace_ == Workspace::Content) return;
    ecs::World& world = engine.world();
    const gfx::Camera2D& cam = engine.camera();
    f32 px = 1.0f / cam.zoom;
    scene_.begin(cam);
    bool alt_down = engine.input().down(Key::LeftAlt) || engine.input().down(Key::RightAlt);
    Vec2 map_origin;
    const gfx::Tilemap* cells = snap_ ? snap_map(map_origin) : nullptr;
    if (show_grid_ && !cells) {
        // Lines stay at least 10 pixels apart: zoomed out, every fourth line is kept, and so on. Every eighth is
        // brighter, and the axes through the origin are colored.
        f32 step = grid_;
        while (step * cam.zoom < 10) step *= 4;
        Rect view = cam.visible_bounds();
        auto shade = [&](f32 v, Color axis) {
            if (std::abs(v) < step * 0.5f) return axis;
            return std::fmod(std::abs(std::round(v / step)), 8.0f) == 0 ? Color::hex(0xffffff, 0.11f) : Color::hex(0xffffff, 0.05f);
        };
        for (f32 x = std::floor(view.x / step) * step; x <= view.right(); x += step)
            scene_.draw_rect({x - px * 0.5f, view.y, px, view.h}, shade(x, Color::hex(0x5ae07a, 0.45f)), -1e9f);
        for (f32 y = std::floor(view.y / step) * step; y <= view.bottom(); y += step)
            scene_.draw_rect({view.x, y - px * 0.5f, view.w, px}, shade(y, Color::hex(0xe05a5a, 0.45f)), -1e9f);
    }
    auto skip = [&](ecs::Entity e) { return hidden(world, e); };
    gfx::draw_maps(world, scene_, static_cast<f32>(engine.time()), skip);
    gfx::draw_sprites(world, scene_, skip);

    // What the Place tool (or art being dragged from the shelf) would put down, see-through.
    const ImGuiPayload* drag = ImGui::GetDragDropPayload();
    bool dragging_art = drag && drag->IsDataType(inspector::asset_payload);
    bool alt = engine.input().down(Key::LeftAlt) || engine.input().down(Key::RightAlt);
    if ((tool_ == Tool::Place || dragging_art) && !ImGui::GetIO().WantCaptureMouse) {
        std::string art = dragging_art ? std::string(static_cast<const char*>(drag->Data), static_cast<usize>(drag->DataSize))
                                       : place_art_;
        Vec2 at = snapped(mouse_world(engine), alt);
        if (!art.empty() && is_image(art)) {
            if (auto tex = engine.assets().load<gfx::Texture>(art)) {
                gfx::SpriteInstance s;
                s.position = at;
                s.size = tex->size();
                s.origin = {0.5f, 1.0f};
                s.depth = 1e9f;
                s.color = Color::white().with_alpha(0.55f);
                scene_.draw(*tex, s);
            }
        }
    }
    engine.renderer().submit(scene_);

    gizmos_.begin(cam, gfx::SpriteSort::None);
    Color accent = Color::hex(0xf2c45c), muted = Color::hex(0xa0a6b4);
    // With a map to snap to, the cell under the mouse instead of the square grid.
    if (cells && show_grid_ && !alt_down && !ImGui::GetIO().WantCaptureMouse) {
        auto [cx, cy] = cells->cell_at(mouse_world(engine) - map_origin);
        auto c = cell_corners(*cells, cx, cy);
        for (int i = 0; i < 4; ++i)
            line(gizmos_, map_origin + c[static_cast<usize>(i)], map_origin + c[static_cast<usize>((i + 1) % 4)], 1.5f * px,
                 accent.with_alpha(0.7f));
    }
    world.each<const ecs::GlobalTransform2D>([&](ecs::Entity e, const ecs::GlobalTransform2D& g) {
        bool chosen = selected(e), hover = e == hovered_ && !chosen;
        if (hidden(world, e)) return;
        if (world.has<gfx::Map>(e)) {
            if (chosen || hover)
                if (auto b = bounds_of(engine, e))
                    gizmos_.draw_outline(b->expanded(px), chosen ? accent : muted.with_alpha(0.8f), (chosen ? 2 : 1) * px);
            return;
        }
        if (world.has<gfx::Sprite>(e)) {
            if (!chosen && !hover) return;
            if (auto b = gfx::sprite_bounds(world, e))
                gizmos_.draw_outline(b->expanded(px), chosen ? accent : muted.with_alpha(0.8f), (chosen ? 2 : 1) * px);
        } else { // a marker for entities that draw nothing
            gizmos_.draw_outline(Rect::from_center(g.position, Vec2{14 * px}), chosen ? accent : hover ? Color::white() : muted,
                                 2 * px);
            gizmos_.draw_rect(Rect::from_center(g.position, Vec2{3 * px}), chosen ? accent : muted);
        }
        if (chosen) gizmos_.draw_rect(Rect::from_center(g.position, Vec2{5 * px}), accent); // the pivot
    });
    if (tool_ == Tool::Place && !is_image(place_art_) && !ImGui::GetIO().WantCaptureMouse)
        gizmos_.draw_outline(Rect::from_center(snapped(mouse_world(engine), alt), Vec2{14 * px}), accent.with_alpha(0.6f), 2 * px);
    draw_colliders(engine, px);
    if (drag_ == Drag::Box) {
        Vec2 a = drag_start_world_, b = mouse_world(engine);
        Rect r{std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y)};
        gizmos_.draw_rect(r, accent.with_alpha(0.08f));
        gizmos_.draw_outline(r, accent.with_alpha(0.8f), px);
    }
    engine.renderer().submit(gizmos_);
}

// Actions

ecs::Entity Editor::create(Engine& engine, Vec2 at, const std::string& art) {
    ecs::World& world = engine.world();
    std::string name = art.empty() ? "Thing" : fs::path(art).stem().string();
    if (is_prefab(art)) { // copies of the prefab's entities, its root where the mouse is
        Asset<ecs::Prefab> prefab = engine.assets().load<ecs::Prefab>(art);
        if (!prefab) {
            say(std::format("Couldn't read {}", art));
            return ecs::null_entity;
        }
        serial::Context ctx{&engine.assets()};
        std::vector<ecs::Entity> made = ecs::instantiate(world, *prefab, ctx);
        if (made.empty()) return ecs::null_entity;
        if (!world.has<ecs::Transform2D>(made[0])) world.add<ecs::Transform2D>(made[0]);
        world.get<ecs::Transform2D>(made[0]).position = at;
        if (!world.has<ecs::Name>(made[0])) world.add<ecs::Name>(made[0], ecs::Name{name});
        for (ecs::Entity e : made) doc_->add(e);
        select_only(made[0]);
        ecs::propagate_transforms(world);
        return made[0];
    }
    ecs::Entity e = world.create(ecs::Name{name}, ecs::Transform2D{at});
    if (is_map(art)) { // a backdrop to place things on: locked, so clicks go to what's on it
        world.add<gfx::Map>(e, gfx::Map{.map = engine.assets().load<gfx::Tilemap>(art)});
        world.add<EditorFlags>(e, EditorFlags{.locked = true});
        doc_->add(e);
        select_only(e);
        tool_ = Tool::Select;
        ecs::propagate_transforms(world);
        say(std::format("Placed {}, locked so you can place things on it (unlock it in the Outline)", name));
        return e;
    }
    if (!art.empty()) {
        gfx::Sprite s;
        s.texture = engine.assets().load<gfx::Texture>(art);
        s.origin = {0.5f, 1.0f}; // pictures stand on the middle of their bottom edge
        world.add<gfx::Sprite>(e, std::move(s));
    }
    doc_->add(e);
    select_only(e);
    ecs::propagate_transforms(world);
    return e;
}

void Editor::duplicate(Engine& engine) {
    ecs::World& world = engine.world();
    Selection copies;
    for (ecs::Entity root : top_level_selection(engine)) {
        std::vector<ecs::Entity> tree;
        subtree(world, root, tree);
        serial::Context ctx{&engine.assets()};
        auto made = ecs::load_entities(world, ecs::save_entities(world, tree, ctx), ctx);
        if (made.empty()) continue;
        drop_dangling_parents(world, made);
        for (usize i = 0; i < made.size() && i < tree.size(); ++i) {
            if (const RawComponents* raw = world.try_get<const RawComponents>(tree[i])) world.add<RawComponents>(made[i], *raw);
            doc_->add(made[i]);
        }
        if (const ecs::Parent* p = world.try_get<const ecs::Parent>(root)) ecs::set_parent(world, made[0], p->entity);
        // Out in the open, a copy lands a grid step away; mid Alt-drag it stays put under the mouse.
        if (!drag_copied_)
            if (auto* t = world.try_get<ecs::Transform2D>(made[0])) t->position += Vec2{grid_, grid_};
        copies.push_back(made[0]);
    }
    if (!copies.empty()) selection_ = copies;
    ecs::propagate_transforms(world);
}

void Editor::remove_selection(Engine& engine) {
    ecs::World& world = engine.world();
    for (ecs::Entity e : top_level_selection(engine))
        if (world.alive(e)) {
            ecs::set_parent(world, e, ecs::null_entity);
            ecs::destroy_recursive(world, e);
        }
    selection_.clear();
}

void Editor::copy_selection(Engine& engine) {
    ecs::World& world = engine.world();
    std::vector<ecs::Entity> all;
    for (ecs::Entity root : top_level_selection(engine)) subtree(world, root, all);
    if (all.empty()) return;
    serial::Context ctx{&engine.assets()};
    std::string text = json::stringify(ecs::save_entities(world, all, ctx));
    SDL_SetClipboardText(text.c_str());
    say(std::format("Copied {} {}", all.size(), all.size() == 1 ? "thing" : "things"));
}

void Editor::paste(Engine& engine) {
    char* text = SDL_GetClipboardText();
    if (!text) return;
    auto parsed = json::parse(text);
    SDL_free(text);
    if (!parsed || parsed.value["format"].as_string() != ecs::world_format) return;
    ecs::World& world = engine.world();
    serial::Context ctx{&engine.assets()};
    auto made = ecs::load_entities(world, parsed.value, ctx);
    drop_dangling_parents(world, made);
    Selection roots;
    for (ecs::Entity e : made) {
        doc_->add(e);
        if (!world.has<ecs::Parent>(e)) roots.push_back(e);
    }
    ecs::propagate_transforms(world);
    // Centered on the mouse, keeping the copied things' layout.
    Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
    for (ecs::Entity e : roots)
        if (auto* t = world.try_get<const ecs::Transform2D>(e)) {
            lo = {std::min(lo.x, t->position.x), std::min(lo.y, t->position.y)};
            hi = {std::max(hi.x, t->position.x), std::max(hi.y, t->position.y)};
        }
    if (lo.x <= hi.x) {
        Vec2 shift = snapped(mouse_world(engine)) - snapped((lo + hi) * 0.5f);
        for (ecs::Entity e : roots)
            if (auto* t = world.try_get<ecs::Transform2D>(e)) t->position += shift;
    }
    selection_ = roots;
    say(std::format("Pasted {} {}", made.size(), made.size() == 1 ? "thing" : "things"));
}

void Editor::reparent(Engine& engine, ecs::Entity child, ecs::Entity parent) {
    ecs::World& world = engine.world();
    ecs::propagate_transforms(world);
    const auto* g = world.try_get<const ecs::GlobalTransform2D>(child);
    std::optional<ecs::GlobalTransform2D> before;
    if (g) before = *g;
    if (!ecs::set_parent(world, child, parent)) {
        say("Can't put something inside itself");
        return;
    }
    auto* t = world.try_get<ecs::Transform2D>(child);
    if (!t || !before) return;
    // The same place in the world, in the new parent's space.
    if (const auto* pg = parent ? world.try_get<const ecs::GlobalTransform2D>(parent) : nullptr) {
        Vec2 local = (before->position - pg->position).rotated(-pg->rotation);
        t->position = {pg->scale.x != 0 ? local.x / pg->scale.x : local.x, pg->scale.y != 0 ? local.y / pg->scale.y : local.y};
        t->rotation = before->rotation - pg->rotation;
        t->scale = {pg->scale.x != 0 ? before->scale.x / pg->scale.x : before->scale.x,
                    pg->scale.y != 0 ? before->scale.y / pg->scale.y : before->scale.y};
    } else {
        t->position = before->position;
        t->rotation = before->rotation;
        t->scale = before->scale;
    }
    ecs::propagate_transforms(world);
}

void Editor::group_selection(Engine& engine) {
    std::vector<ecs::Entity> tops = top_level_selection(engine);
    if (tops.empty()) return;
    ecs::World& world = engine.world();
    Vec2 sum;
    int n = 0;
    for (ecs::Entity e : tops)
        if (const auto* g = world.try_get<const ecs::GlobalTransform2D>(e)) {
            sum += g->position;
            ++n;
        }
    Vec2 center = snapped(n ? sum / static_cast<f32>(n) : Vec2{});
    ecs::Entity group = world.create(ecs::Name{"Group"}, ecs::Transform2D{center});
    doc_->add(group);
    ecs::propagate_transforms(world);
    for (ecs::Entity e : tops) reparent(engine, e, group);
    select_only(group);
    say(std::format("Grouped {} {}", tops.size(), tops.size() == 1 ? "thing" : "things"));
}

void Editor::ungroup_selection(Engine& engine) {
    ecs::World& world = engine.world();
    Selection freed;
    for (ecs::Entity group : top_level_selection(engine)) {
        const ecs::Children* c = world.try_get<const ecs::Children>(group);
        if (!c) continue;
        std::vector<ecs::Entity> children = c->list;
        ecs::Entity outer = world.has<ecs::Parent>(group) ? world.get<ecs::Parent>(group).entity : ecs::null_entity;
        for (ecs::Entity child : children)
            if (world.alive(child)) {
                reparent(engine, child, outer);
                freed.push_back(child);
            }
        ecs::set_parent(world, group, ecs::null_entity);
        world.destroy(group);
    }
    if (!freed.empty()) selection_ = freed;
}

void Editor::nudge(Engine& engine, Vec2 by) {
    for (ecs::Entity e : top_level_selection(engine))
        if (auto* t = engine.world().try_get<ecs::Transform2D>(e)) t->position += by;
}

void Editor::rotate_selection(Engine& engine, f32 degrees) {
    for (ecs::Entity e : top_level_selection(engine))
        if (auto* t = engine.world().try_get<ecs::Transform2D>(e)) t->rotation += radians(degrees);
}

void Editor::align(Engine& engine, int edge) {
    std::vector<ecs::Entity> tops = top_level_selection(engine);
    if (tops.size() < 2) return;
    Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
    for (ecs::Entity e : tops)
        if (auto b = bounds_of(engine, e)) {
            lo = {std::min(lo.x, b->x), std::min(lo.y, b->y)};
            hi = {std::max(hi.x, b->right()), std::max(hi.y, b->bottom())};
        }
    for (ecs::Entity e : tops) {
        auto b = bounds_of(engine, e);
        auto* t = engine.world().try_get<ecs::Transform2D>(e);
        if (!b || !t) continue;
        switch (edge) {
        case 0: t->position.x += lo.x - b->x; break;
        case 1: t->position.x += (lo.x + hi.x) * 0.5f - b->center().x; break;
        case 2: t->position.x += hi.x - b->right(); break;
        case 3: t->position.y += lo.y - b->y; break;
        case 4: t->position.y += (lo.y + hi.y) * 0.5f - b->center().y; break;
        case 5: t->position.y += hi.y - b->bottom(); break;
        }
    }
}

void Editor::undo(Engine&) {
    doc_->track(selection_);
    if (!doc_->can_undo()) return;
    selection_ = doc_->undo();
}

void Editor::redo(Engine&) {
    doc_->track(selection_);
    if (!doc_->can_redo()) return;
    selection_ = doc_->redo();
}

void Editor::look_at(Engine& engine, Vec2 world) {
    gfx::Camera2D& cam = engine.camera();
    Vec2 window_center = cam.viewport * 0.5f;
    Vec2 view_center = free_area_.w > 0 ? free_area_.center() : window_center;
    cam.position = world - (view_center - window_center) / cam.zoom;
}

void Editor::frame(Engine& engine, bool selection_only) {
    Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
    bool any = false;
    for (ecs::Entity e : selection_only ? selection_ : doc_->entities())
        if (auto b = bounds_of(engine, e)) {
            lo = {std::min(lo.x, b->x), std::min(lo.y, b->y)};
            hi = {std::max(hi.x, b->right()), std::max(hi.y, b->bottom())};
            any = true;
        }
    gfx::Camera2D& cam = engine.camera();
    if (!any) {
        cam.zoom = 1;
        look_at(engine, {0, 0});
        return;
    }
    // Fit everything in the free area with a margin; no closer than 100% for a whole level, 300% for a selection.
    Vec2 size = hi - lo;
    f32 limit = selection_only ? 3.0f : 1.0f;
    cam.zoom = limit;
    if (free_area_.w > 0 && (size.x > 0 || size.y > 0))
        cam.zoom = std::min(limit, 0.8f * std::min(size.x > 0 ? free_area_.w / size.x : 1e9f, size.y > 0 ? free_area_.h / size.y : 1e9f));
    look_at(engine, (lo + hi) * 0.5f);
}

// Files

void Editor::after_saving_changes(Engine& engine, std::function<void(Engine&)> then) {
    doc_->track(selection_);
    if (!doc_->modified()) {
        then(engine);
        return;
    }
    after_save_prompt_ = std::move(then);
    save_prompt_open_ = true;
}

void Editor::new_level(Engine& engine) {
    after_saving_changes(engine, [this](Engine&) {
        doc_->clear();
        selection_.clear();
        frame_pending_ = true;
        say("New level");
    });
}

void Editor::open_dialog(Engine& engine) {
    after_saving_changes(engine, [this](Engine& e) {
        std::string where = (doc_->file().empty() ? assets_dir_ : doc_->file().parent_path()).string();
        SDL_ShowOpenFileDialog(
            [](void* user, const char* const* files, int) {
                if (!files || !files[0]) return; // cancelled
                auto* r = static_cast<DialogResult*>(user);
                std::lock_guard lock(r->mutex);
                r->open = fs::path(files[0]);
            },
            dialogs_.get(), e.window().handle(), &world_filter, 1, where.c_str(), false);
    });
}

void Editor::open(Engine& engine, const fs::path& file) {
    (void)engine;
    if (!doc_->open(file)) {
        say("Couldn't open " + file.filename().string() + " (see the log)");
        return;
    }
    selection_.clear();
    frame_pending_ = true;
    recovery_ = doc_->pending_autosave();
    say("Opened " + file.filename().string());
}

void Editor::save(Engine& engine, bool choose_file) {
    doc_->track(selection_);
    if (!choose_file && !doc_->file().empty()) {
        if (doc_->save()) say("Saved " + doc_->title());
        return;
    }
    std::string where = (doc_->file().empty() ? assets_dir_ / "level.world" : doc_->file()).string();
    SDL_ShowSaveFileDialog(
        [](void* user, const char* const* files, int) {
            if (!files || !files[0]) return;
            auto* r = static_cast<DialogResult*>(user);
            std::lock_guard lock(r->mutex);
            r->save = fs::path(files[0]);
        },
        dialogs_.get(), engine.window().handle(), &world_filter, 1, where.c_str());
}

void Editor::scan_art() {
    std::map<std::string, Asset<gfx::Texture>> loaded; // keep textures already shown
    for (Art& a : art_) loaded[a.path] = a.texture;
    art_.clear();
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(assets_dir_, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->path().filename().string().starts_with('.')) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec) || !(is_image(it->path()) || is_map(it->path()) || is_prefab(it->path()))) continue;
        fs::path rel = fs::relative(it->path(), assets_dir_, ec);
        Art art;
        art.kind = is_map(it->path()) ? Art::Kind::Map : is_prefab(it->path()) ? Art::Kind::Prefab : Art::Kind::Picture;
        art.path = rel.generic_string();
        art.folder = rel.parent_path().generic_string();
        art.name = rel.stem().string();
        if (auto found = loaded.find(art.path); found != loaded.end()) art.texture = found->second;
        art_.push_back(std::move(art));
    }
    std::sort(art_.begin(), art_.end(), [](const Art& a, const Art& b) {
        return a.folder != b.folder ? a.folder < b.folder : a.name < b.name;
    });
}

void Editor::say(std::string message) {
    info("editor: {}", message);
    message_ = std::move(message);
    message_time_ = now_seconds();
}

void Editor::update_title(Engine& engine) {
    std::string title = std::format("{}{} - Bowl Editor", doc_->title(), doc_->modified() ? " *" : "");
    if (title != title_) {
        title_ = title;
        engine.window().set_title(title_);
    }
}

} // namespace bowl::editor
