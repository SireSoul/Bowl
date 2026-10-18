#pragma once

#include "content.hpp"
#include "paint_studio.hpp"
#include "document.hpp"
#include "ui.hpp"

#include "bowl/app/dev_link.hpp"
#include "bowl/app/engine.hpp"
#include "bowl/core/json_text.hpp"
#include "bowl/gfx/tilemap.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bowl::editor {

// Bowl Editor: lays out levels (.world files) with the game's own renderer, so what you see is what the game shows.
//
//     bowl-editor [level.world] [--assets <folder>] [--game <game code library>] [--select <name>] [--resave]
//                 [--attach [profiler|systems|entities]]
//
// --game loads a game's hot-reload library (lib<game>_code.so / .dylib) so its BOWL_COMPONENT types can be added and
// edited. Without it, the game's components are kept as raw JSON. --resave rewrites the file and quits. --attach opens
// the Game tab (on that pane) attached to the first game running on this computer.
class Editor final : public Game {
public:
    AppConfig configure() override;
    void init(Engine& engine) override;
    void update(Engine& engine, f32 dt) override;
    void render(Engine& engine) override;
    void dev_ui(Engine& engine) override;
    void shutdown(Engine& engine) override;

private:
    // The editor's rooms: levels, the game's content as forms, its data files as JSON, painting art, and a running game.
    enum class Workspace { Level, Content, Data, Paint, Game };
    enum class Tool { Select, Place, Pan, Brush, Region };
    enum class Drag { None, Move, Box, Pan };

    // A picture, a Tiled map or a prefab in the assets folder, shown on the art shelf.
    struct Art {
        enum class Kind : u8 { Picture, Map, Prefab } kind = Kind::Picture;
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
    // Hidden or locked, itself or a group it's in (EditorFlags).
    bool hidden(const ecs::World& world, ecs::Entity e) const;
    bool locked(const ecs::World& world, ecs::Entity e) const;
    void set_hidden(Engine& engine, bool on); // the selection
    void set_locked(Engine& engine, bool on);
    // The map things snap to: the first visible map in the level, and where it is. Null when there's none.
    const gfx::Tilemap* snap_map(Vec2& origin) const;

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

    // data.cpp: the Data workspace
    struct DataFile {
        std::string path; // virtual path, for display
        std::filesystem::path file;
    };
    struct DataEdit { // made while drawing, applied after (each edit re-reads the document)
        enum Kind { Set, Insert, Erase } kind;
        json::TextDocument::Path path;
        json::Value value;
        std::string key;
    };
    void scan_data();
    void data_workspace(Engine& engine);
    void data_tree(const json::Value& value, json::TextDocument::Path& path, std::string_view label);
    bool data_tree_fixed_ = false; // values only: no keys or items added or removed (a game's component)
    void data_open(usize index);
    bool data_save();
    void data_shortcuts(Engine& engine);
    void data_undo(int direction);

    std::vector<DataFile> data_files_;
    std::optional<usize> data_index_;
    json::TextDocument data_doc_;
    std::string data_saved_;                // the text as it is on disk
    std::vector<std::string> data_history_; // undo steps (whole texts: data files are small)
    usize data_cursor_ = 0;
    std::vector<DataEdit> data_edits_;
    char data_filter_[128] = {};

    // paint_studio.cpp: the Paint workspace; the Level's Brush tool paints through it
    PaintStudio paint_;
    void brush_mouse(Engine& engine); // Level workspace: the Brush tool
    bool painting_ = false;

    // Regions: colliders drawn and edited on the level (regions.cpp)
    bool region_mouse(Engine& engine); // true when it used the mouse
    void draw_colliders(Engine& engine, f32 px);
    bool show_colliders_ = true;
    ecs::Entity brush_target_;          // the sprite the Brush is painting on
    int region_handle_ = -1; // the resize handle being dragged
    Vec2 region_anchor_;

    // content.cpp: the Content workspace, forms for the game's content from its schemas
    ContentWorkspace content_;

    // game.cpp: the Game workspace, attached to a running game through its dev link
    void game_workspace(Engine& engine);
    void game_update(Engine& engine); // every frame, whichever workspace shows
    void game_profiler();
    void game_systems();
    void game_schedule(const char* label, const std::vector<ecs::SystemInfo>& systems);
    void game_entities();
    void game_access_note(Engine& engine, ecs::Entity e); // Level tab: which of the game's systems use its components
    devlink::DevLinkClient link_;
    std::vector<devlink::GameEntry> link_games_;
    f64 link_scanned_ = -1e9;
    std::string link_game_;               // the one attached to (or last attached to)
    bool attach_first_ = false;           // --attach: to the first running game, as soon as there is one
    int game_pane_ = 0;                   // 0 profiler, 1 systems, 2 entities
    char link_filter_[64] = "";
    u32 link_from_ = 0;
    std::optional<std::pair<u32, u32>> link_watching_;
    std::map<std::string, json::TextDocument> link_docs_; // the watched entity's components, as edited
    std::optional<devlink::Schedules> known_schedules_;   // the last a game sent, kept in the editor's settings
    std::string known_schedules_when_;

    Workspace workspace_ = Workspace::Level;
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
    bool snap_to_map_ = true; // with a map in the level, snap to the middle of its cells instead of the square grid
    Engine* engine_ = nullptr;
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

// Engine component names read better without namespaces ("bowl::gfx::Sprite" is "Art", "game::SpawnPoint" is
// "Spawn point").
std::string pretty_name(std::string_view type_name);

} // namespace bowl::editor
