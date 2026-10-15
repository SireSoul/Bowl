#pragma once

#include "document.hpp"
#include "ui.hpp"

#include "ph/app/engine.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ph::editor {

// Bowl Editor: lays out levels (.world files) with the game's own renderer, so what you see is what the game shows.
//
//     bowl-editor [level.world] [--assets <folder>] [--game <game code library>] [--select <name>] [--resave]
//
// --game loads a game's hot-reload library (lib<game>_code.so / .dylib) so its PH_COMPONENT types can be added and
// edited. Without it, the game's components are kept as raw JSON. --resave rewrites the file and quits.
class Editor final : public Game {
public:
    AppConfig configure() override;
    void init(Engine& engine) override;
    void update(Engine& engine, f32 dt) override;
    void render(Engine& engine) override;
    void dev_ui(Engine& engine) override;
    void shutdown(Engine& engine) override;

private:
    enum class Tool { Select, Place, Pan };
    enum class Drag { None, Move, Box, Pan };

    // A picture in the assets folder, shown on the art shelf.
    struct Art {
        std::string path;   // virtual path, as Sprite.texture stores it
        std::string folder; // its folder, for grouping ("" at the top)
        std::string name;   // file name without extension
        Asset<gfx::Texture> texture; // loaded when first shown
    };

    // editor.cpp: the scene, what the mouse and keys do there, and the actions behind every button
    void scene_mouse(Engine& engine);
    void shortcuts(Engine& engine);
    ecs::Entity pick(Engine& engine, Vec2 world);
    std::vector<ecs::Entity> pick_box(Engine& engine, Rect area);
    Vec2 snapped(Vec2 p, bool force_free = false) const;
    Vec2 mouse_world(Engine& engine) const;
    bool selected(ecs::Entity e) const;
    void select_only(ecs::Entity e);
    void toggle_selected(ecs::Entity e);
    std::vector<ecs::Entity> top_level_selection(Engine& engine) const; // without entities whose parent is selected
    std::optional<Rect> bounds_of(Engine& engine, ecs::Entity e);

    ecs::Entity create(Engine& engine, Vec2 at, const std::string& art = {});
    void duplicate(Engine& engine);
    void remove_selection(Engine& engine);
    void copy_selection(Engine& engine);
    void paste(Engine& engine);
    void group_selection(Engine& engine);
    void ungroup_selection(Engine& engine);
    void reparent(Engine& engine, ecs::Entity child, ecs::Entity parent); // keeps where the child is in the world
    void nudge(Engine& engine, Vec2 by);
    void rotate_selection(Engine& engine, f32 degrees);
    void align(Engine& engine, int edge); // 0 left, 1 center x, 2 right, 3 top, 4 center y, 5 bottom
    void undo(Engine& engine);
    void redo(Engine& engine);

    void look_at(Engine& engine, Vec2 world); // centers a point in the area the panels leave free
    void frame(Engine& engine, bool selection_only);

    // Asks to save unsaved changes first, then runs `then`.
    void after_saving_changes(Engine& engine, std::function<void(Engine&)> then);
    void new_level(Engine& engine);
    void open_dialog(Engine& engine);
    void open(Engine& engine, const std::filesystem::path& file);
    void save(Engine& engine, bool choose_file);
    void scan_art();
    void say(std::string message); // a short message in the status line
    void update_title(Engine& engine);

    // panels.cpp: everything drawn with ImGui
    void top_bar(Engine& engine);
    void left_panel(Engine& engine);
    void art_shelf(Engine& engine);
    void outline(Engine& engine);
    void outline_node(Engine& engine, ecs::Entity e);
    void right_panel(Engine& engine);
    void inspect_one(Engine& engine, ecs::Entity e);
    void inspect_many(Engine& engine);
    void inspect_level(Engine& engine);
    void transform_card(Engine& engine, ecs::Entity e);
    void art_card(Engine& engine, ecs::Entity e);
    void art_picker(Engine& engine, ecs::Entity e);
    void raw_cards(Engine& engine, ecs::Entity e);
    void add_component_button(Engine& engine, ecs::Entity e);
    void status_bar(Engine& engine);
    void modals(Engine& engine);
    void help_window();
    void welcome_window(Engine& engine);
    void art_drop(Engine& engine); // art dragged from the shelf and let go over the scene
    bool thumbnail(Art& art, float size, bool chosen);

    std::unique_ptr<Document> doc_;
    Selection selection_;
    Tool tool_ = Tool::Select;
    std::string place_art_; // what the Place tool stamps ("" for an empty entity)
    ecs::Entity hovered_;

    // Dragging in the scene
    Drag drag_ = Drag::None;
    Vec2 drag_start_world_;
    struct Moving {
        ecs::Entity e;
        Vec2 local, global; // positions when the drag began
    };
    std::vector<Moving> moving_;
    bool drag_copied_ = false; // Alt-drag duplicates first

    // View
    bool show_grid_ = true, snap_ = true;
    f32 grid_ = 16;
    Rect free_area_;
    bool frame_pending_ = true;

    // Panels
    std::vector<Art> art_;
    std::filesystem::path assets_dir_;
    char art_filter_[128] = {};
    char add_filter_[128] = {};
    int left_tab_ = 0; // 0 art, 1 outline
    bool show_help_ = false, show_welcome_ = false;
    std::string message_;
    f64 message_time_ = -100;
    std::function<void(Engine&)> after_save_prompt_;
    bool save_prompt_open_ = false;
    std::optional<std::string> recovery_; // autosaved text waiting to be offered back
    std::string title_;
    f64 last_track_ = 0;

    // Startup options
    std::string select_name_;
    bool resave_ = false;
    void* game_library_ = nullptr;
    AssetManager* assets_ = nullptr; // for loading thumbnails while drawing

    gfx::SpriteBatch scene_, gizmos_;

    // File dialogs answer on another thread; the main thread picks the result up.
    struct DialogResult {
        std::mutex mutex;
        std::optional<std::filesystem::path> open, save;
    };
    std::shared_ptr<DialogResult> dialogs_ = std::make_shared<DialogResult>();
};

// Engine component names read better without namespaces ("ph::gfx::Sprite" is "Art", "game::SpawnPoint" is
// "Spawn point").
std::string pretty_name(std::string_view type_name);

} // namespace ph::editor
