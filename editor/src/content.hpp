#pragma once

#include "bowl/app/engine.hpp"
#include "bowl/core/json_text.hpp"
#include "bowl/gfx/tilemap.hpp"

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct SDL_Process;

// The Content workspace: a game's content (items, people, shops, events, sounds) edited as forms instead of JSON.
// What the forms hold comes from schema files in the assets folder (any file ending in .schema.json) that say which
// data files hold which collections of records, and what fields each record has. See docs/guides/editor.md, "The
// Content workspace", for the format. Sound events (.sound files) are a collection of their own without any schema.
namespace bowl::editor {

namespace schema {

// One field of a record: its key in the object, what kind of value it holds, and how to show it.
struct Field {
    std::string key, type, label, help;
    json::Value fallback; // "default": shown (dimmed) when the key is missing, and used for new records
    bool has_default = false;
    std::optional<f64> min, max;
    f64 step = 0;
    std::vector<std::string> options; // choice, choices
    bool free = false;                // choice, ref: any text is fine too
    bool multiline = false;           // text
    bool optional = false;            // ref: may be empty
    bool relative = false;            // audio: the path is from the record file's folder (a leading / from the top)
    std::string to;                   // ref: the collection whose ids it names
    std::string area;                 // spawn, cell: the key of the field beside it naming the map
    std::string suggest;              // text: offer the values other fields with the same suggest hold
    std::string folder;               // map: where the maps are ("maps")
    std::vector<std::string> summary; // list items: the fields shown in each item's title
    std::shared_ptr<Field> of;        // list: what each item is

    std::string title() const; // label, or the key made readable ("per_day" is "Per day")
};

struct Variant {
    std::string name, label;
    std::vector<Field> fields;
};

// A named type: an object with fields, maybe also one of several variants (a "tag" key says which, and the field of
// type "variant" says where its menu goes), or an alias for a field with settings ("base").
struct Type {
    std::string name;
    std::vector<Field> fields;
    std::string tag;
    std::vector<Variant> variants;
    std::vector<std::string> summary;
    std::optional<Field> base;

    const Variant* variant(std::string_view name) const;
};

// A list of records in one file ("list" names the key holding them; none: the whole file is one record), or one record
// per file in a folder (sound events).
struct Collection {
    std::string id, title, help;
    std::string file, list;      // a file's records
    std::string folder, extension; // files as records: every file with this extension under the folder
    std::string type;            // the records' type
    std::string label, picture;  // fields shown in the list of records
    std::string id_key = "id";
    std::string icon; // person, item, shop, event, map, sound, settings, area: the picture beside it in the list
    bool files() const { return !extension.empty(); }
};

// A kind of marker on a map's "markers" layer, and its properties (Tiled custom properties).
struct MarkerType {
    std::string name, label, help;
    bool area = false; // covers cells (an exit); otherwise a point in the middle of a cell (a spawn point)
    std::vector<Field> fields;
};

// Things a game places on a map's object layer one per cell (trees, rocks), and where their pictures are said: a list
// in a data file, each with a name (the object's type), a picture and the point of it that stands on the cell.
struct MapObjects {
    std::string layer;                       // the object layer ("features")
    std::string file, list = "kinds";        // the data file and the key of its list
    std::string name = "name", picture = "picture", origin = "origin";
};

struct Schema {
    std::vector<Collection> collections;
    std::optional<MapObjects> objects;
    std::map<std::string, Type, std::less<>> types;
    std::vector<MarkerType> markers;
    std::vector<std::string> problems;
    // The game, for the Play button: an executable (from the assets folder) and its arguments.
    std::string play;
    std::vector<std::string> play_args;
    std::filesystem::path play_from; // the schema's folder, which `play` is relative to

    const MarkerType* marker(std::string_view name) const;

    const Type* type(std::string_view name) const;
    const Collection* collection(std::string_view id) const;
    // The field with its type's alias applied: {"type": "flag"} becomes the "flag" alias's base, with this field's
    // key, label and help.
    Field resolve(const Field& f) const;
    // Adds what a schema document says. Problems go in `problems`, naming `source`.
    void add(const json::Value& doc, std::string_view source);
};

// A new value for a field: its default, or an empty value of its kind.
json::Value default_value(const Schema& s, const Field& f);
// A new record of a type: every field at its default (the first variant, for one with variants).
json::Value default_record(const Schema& s, const Type& t);

} // namespace schema

class ContentWorkspace {
public:
    // `say` puts a short message in the status line.
    void init(Engine& engine, std::filesystem::path assets_dir, std::function<void(std::string)> say);
    void scan(); // finds schemas, data files, pictures, sounds and maps again
    void draw(Engine& engine, Rect area);
    void update(Engine& engine); // every frame, whichever workspace shows (file dialogs answer here)
    bool unsaved() const;
    // Find anything (Ctrl+K, from any workspace): draws it when it's open.
    void search(Engine& engine) { palette(engine); }
    void open_search() {
        palette_open_ = true;
        palette_query_.clear();
        palette_pick_ = 0;
    }
    bool searching() const { return palette_open_; }
    // What the editor around it does for search results and buttons that lead elsewhere.
    struct Hooks {
        std::function<std::vector<std::string>()> pictures;       // every picture, for search
        std::function<void(const std::string&)> open_picture;     // in the Paint workspace
        std::function<void(const std::string&)> open_text;        // a file as JSON text (the Data workspace)
        std::function<void(int)> go;                              // 0 Content, 1 Paint, 2 Level, 3 files as text, 4 Game
    };
    Hooks hooks;
    // Shows a collection, and one of its records by id (or the first).
    void open(std::string_view collection, std::string_view record = {}) { jump_to_ = std::string(collection) + "\n" + std::string(record); }
    bool save_all();

private:
    using Path = json::TextDocument::Path;

    struct Doc {
        std::filesystem::path file;
        json::TextDocument text;
        std::string saved;
        std::vector<std::string> history;
        usize cursor = 0;
        bool exists = false;
        bool modified() const { return text.text() != saved; }
    };
    struct Edit {
        enum Kind { Set, Insert, Erase } kind;
        std::string doc;
        Path path;
        json::Value value;
        std::string key;
    };
    // Where a field's value is: the object holding it, and its key there (or, for an item of a list, its index).
    struct Slot {
        std::string doc;
        Path path; // the value's own path
        const json::Value* value = nullptr; // null when the key is missing
        const json::Value* parent = nullptr; // the object it's in (for fields beside it: a cell's area)
        std::string record_dir;             // folder of the record's file, for paths relative to it
        std::function<void(json::Value)> setter; // writes the value somewhere other than `path` (a map property)
    };
    struct Problem {
        std::string collection;
        usize record = 0;
        std::string text;
        std::string doc; // where the value is, so its field shows the problem too
        Path path;
    };
    // Somewhere one record names another (a shop's stock naming an item).
    struct Ref {
        std::string doc;
        Path path; // the value naming it
        std::string collection; // the record it's in
        usize record = 0;
        std::string where; // "General Store · Stock 2 · Item"
    };
    // Calls `fn` for every field's value in a record, with its path.
    using Visit = std::function<void(const schema::Field& f, const json::Value* v, const Path& path, const json::Value* parent)>;
    void walk(const schema::Field& raw, const json::Value* v, const Path& path, const json::Value* parent, const Visit& fn);
    struct MapInfo {
        Asset<gfx::Tilemap> map;
        std::vector<std::pair<std::string, Vec2>> spawns; // name, cell
        std::vector<std::pair<std::string, Rect>> markers; // every named object on object layers, in cells
    };

    Doc* doc(const std::string& virtual_path); // loaded on first use; null if there's no such file and none can be made
    void apply_edits();
    void track_history();
    void undo(int direction);
    void shortcuts(Engine& engine);

    // The current collection's records.
    usize record_count(const schema::Collection& c);
    const json::Value* record(const schema::Collection& c, usize index, std::string* doc_path, Path* path);
    std::string record_label(const schema::Collection& c, usize index);
    std::string record_id(const schema::Collection& c, usize index);
    std::vector<std::pair<std::string, std::string>> ids_of(std::string_view collection); // id, label
    void new_record(const schema::Collection& c);
    void duplicate_record(const schema::Collection& c, usize index);
    void delete_record(const schema::Collection& c, usize index);
    void move_record(const schema::Collection& c, usize index, int by); // up (-1) or down (1) the list

    void collections_panel(Engine& engine, Rect area);
    void form_panel(Engine& engine, Rect area);
    void problems_list(const std::vector<Problem>& problems);

    // Forms
    void fields(Engine& engine, const std::vector<schema::Field>& list, const schema::Type* owner, const Slot& object);
    void field(Engine& engine, const schema::Field& f, const Slot& slot);
    void value_widget(Engine& engine, const schema::Field& f, const Slot& slot);
    void list_widget(Engine& engine, const schema::Field& f, const Slot& slot);
    void object_widget(Engine& engine, const schema::Type& t, const Slot& slot);
    void variant_menu(const schema::Type& t, const Slot& object);
    void raw_widget(const Slot& slot); // a value that isn't what the schema says, as JSON text
    void set(const Slot& slot, json::Value v);
    std::string item_title(const schema::Field& item, const json::Value& v, usize index);

    // Pickers
    bool picture_picker(const char* id, std::string& path);
    void thumbnail(const std::string& path, f32 box, bool animate = false);
    bool path_combo(const char* id, std::string& value, const std::vector<std::string>& options, bool allow_empty);
    void sound_buttons(Engine& engine, const std::string& path, bool audio);
    void map_spot_window(Engine& engine);
    // A map drawn in the window, filling what's left but `bottom` pixels; wheel zooms, right-drag moves.
    struct MapView {
        f32 zoom = 0;
        Vec2 pan;
        ImVec2 origin{}, size{};
        bool hovered = false;
        Vec2 mouse_cell; // under the mouse, in cells
        Vec2 center() const { return {origin.x + size.x * 0.5f, origin.y + size.y * 0.5f}; }
        ImVec2 to_screen(Vec2 world) const {
            Vec2 s = center() + (world - pan) * zoom;
            return {s.x, s.y};
        }
        Vec2 to_world(ImVec2 s) const { return pan + (Vec2{s.x, s.y} - center()) / zoom; }
    };
    void map_canvas(const char* id, const gfx::Tilemap& map, MapView& view, f32 bottom);
    void cell_outline(const gfx::Tilemap& map, const MapView& view, Rect cells, ImU32 color, f32 thick);
    // The Maps collection: markers placed and changed on the map itself.
    void maps_view(Engine& engine, Rect area, const std::string& file);
    std::vector<Problem> marker_problems(const std::string& file, usize record);
    void play_game(const std::string& area);

    // content_world.cpp: more for maps, and the world
    struct ObjectKind {
        std::string name, picture;
        Vec2 origin{0.5f, 1.0f};
    };
    std::vector<ObjectKind> object_kinds(); // from the schema's "objects"
    // The map's tiles drawn through `to_screen` (map pixels to the screen).
    void draw_tiles(const gfx::Tilemap& m, const std::function<ImVec2(Vec2)>& to_screen, f32 alpha = 1);
    // Objects on the schema's object layer, drawn with their pictures.
    void draw_objects(const gfx::Tilemap& m, const json::Value& map, const std::function<ImVec2(Vec2)>& to_screen, f32 zoom);
    // An exit's way back: on the map it leads to, a spawn point just inside the matching edge and an exit home; on
    // this map, a spawn point just inside this exit for coming back. Returns what it did, to say.
    std::string make_way_back(const std::string& file, usize object);
    // Every value naming a map (exits, areas, schedules...), in content and maps.
    std::vector<Ref> map_refs(const std::string& name);
    bool rename_map(const std::string& file, const std::string& to);
    bool delete_map(const std::string& file);
    void world_view(Engine& engine, Rect area);
    void object_painting(const std::string& file, const json::Value& map, const MapView& view, const gfx::Tilemap& m);
    MapInfo* map_info(const std::string& name, const std::string& folder);
    std::string map_path(const std::string& name, const std::string& folder) const;
    void import_audio(Engine& engine, std::function<void(const std::string& asset_path)> then);
    void suggestions(std::string_view key, std::set<std::string>& out);
    void play(Engine& engine, const std::string& path, bool audio);

    // Problems: ids missing or repeated, references to nothing, files that aren't there.
    std::vector<Problem> problems(const schema::Collection& c);
    // `parent`: the object holding the value, for fields that look beside them (a spawn point's map).
    void check(const schema::Field& f, const json::Value* v, const std::string& where, const schema::Collection& c,
               usize record, std::vector<Problem>& out, const json::Value* parent = nullptr, const std::string& doc = {},
               const Path& path = {});

    // content_tools.cpp: finding, renaming, panels
    std::vector<Ref> find_refs(std::string_view collection, std::string_view id);
    // Changes a record's id, and every reference to it everywhere (including map markers).
    usize rename_id(const schema::Collection& c, usize record, const std::string& from, const std::string& to);
    void used_by(const schema::Collection& c, usize record);
    void palette(Engine& engine);
    // A bar between two panels that drags `value` (a width) between lo and hi; `sign` -1 when the panel is to its right.
    void splitter(const char* id, Rect bar, f32& value, f32 lo, f32 hi, f32 sign);
    // Moves item `from` of the list at `path` to `to`, the others shifting along.
    void move_item(const std::string& doc, const Path& path, const json::Value& list, usize from, usize to);
    static std::string path_key(const std::string& doc, const Path& path);
    void collection_icon(const schema::Collection& c, ImVec2 center, f32 size, ImU32 color);
    void select(usize collection, usize record);

    std::filesystem::path assets_dir_;
    AssetManager* assets_ = nullptr;
    std::function<void(std::string)> say_;
    schema::Schema schema_;
    std::map<std::string, Doc> docs_;
    std::vector<Edit> edits_;
    std::string history_doc_; // the doc undo works on: the current record's
    std::vector<std::string> pictures_, sounds_, audio_, map_names_;
    std::map<std::string, std::vector<std::string>> files_by_ext_;
    std::map<std::string, MapInfo> maps_;
    usize collection_ = 0;
    usize record_ = 0;
    char filter_[96] = {};
    Engine* engine_ = nullptr;
    f32 left_w_ = 300, side_w_ = 340; // panel widths, remembered in the editor's settings
    bool palette_open_ = false;
    std::string palette_query_;
    usize palette_pick_ = 0;
    std::map<std::string, std::string> field_problems_; // path_key -> the problem, for the record on show
    std::string id_editing_;                            // path_key of the id field being typed in, and its text
    std::string id_text_;
    std::string jump_to_; // set by a reference's "go to" button: collection id + "\n" + record id
    std::string new_file_name_;
    bool new_file_popup_ = false;
    std::optional<usize> confirm_delete_;

    // The spot picker: a map shown in a window; a click sets the cell.
    struct SpotPick {
        std::string doc;
        Path path;
        bool exists = false;
        std::string map, folder;
        Vec2 cell;
        f32 zoom = 0;
        Vec2 pan;
    };
    std::optional<SpotPick> spot_;

    // The Maps view: the marker picked (an index into the markers layer's objects), adding, dragging.
    struct MarkerEditor {
        std::string file;
        i64 selected = -1;
        MapView view;
        std::string adding; // a marker type waiting for a click; "" is a plain named marker
        bool placing = false;
        bool dragging = false;
        Vec2 grab;  // where in the marker it was grabbed, in cells
        Vec2 ghost; // where it would go, in cells
    };
    MarkerEditor markers_;
    bool world_ = false;           // the Maps view shows every map
    bool painting_objects_ = false; // the Maps view's other mode: placing trees and rocks
    std::string object_kind_;
    Vec2 last_object_cell_{-1e9f, -1e9f};
    struct WorldView {
        f32 zoom = 0;
        Vec2 pan;
    } world_view_;
    std::string rename_text_;
    bool rename_open_ = false, delete_open_ = false;
    SDL_Process* game_ = nullptr;

    // File dialogs answer on another thread.
    struct Pending {
        std::mutex mutex;
        std::optional<std::filesystem::path> picked;
        bool done = false;
    };
    std::shared_ptr<Pending> pending_ = std::make_shared<Pending>();
    std::function<void(const std::string&)> after_import_;
};

} // namespace bowl::editor
