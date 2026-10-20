#pragma once

#include "code_editor.hpp"

#include "bowl/app/engine.hpp"
#include "bowl/core/json_text.hpp"
#include "bowl/gfx/material.hpp"
#include "bowl/paint/paint.hpp"
#include "bowl/shadergraph/shadergraph.hpp"

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

// The Materials workspace: how things look. Materials (.material files) give a picture its shader, glow (an emissive
// map), normal map and shadow, edited as a form beside a live preview drawn by the game's renderer, with a light to
// drag around and bloom. Normal and emissive maps are made from the picture here, or painted in the Paint workspace.
// Shaders are written as GLSL in a code editor that compiles as you type, or built as a graph of nodes
// (.shadergraph files, which write the GLSL beside them); either way their parameters show up in every material that
// uses them, with the defaults, ranges and help their comments give. Saving writes the files the game reads, so a
// running game changes with them.
namespace bowl::editor {

class MaterialsWorkspace {
public:
    struct Hooks {
        std::function<std::vector<std::string>()> pictures; // every picture
        std::function<void(const std::string&)> paint;      // a picture in the Paint workspace
        std::function<void(const std::string&)> open_text;  // a file as JSON text
        std::function<void()> pictures_changed;             // a picture was written
    };
    Hooks hooks;

    void init(Engine& engine, std::filesystem::path assets_dir, std::function<void(std::string)> say);
    void scan(); // the materials, shaders and graphs in the assets
    // Opens a material (.material), a shader (its .frag.glsl or asset path) or a graph (.shadergraph). What was open is
    // saved first.
    bool open(const std::string& path);
    // A new material showing `picture` (with the emissive and normal maps beside it, if there are any) and, if given,
    // using `shader`. Opens it and returns its path ("" if it couldn't be written).
    std::string new_material(const std::string& picture, const std::string& shader = {});
    bool unsaved() const;
    bool save();
    const std::string& path() const { return path_; }
    const std::vector<std::string>& materials() const { return materials_; }
    const std::vector<std::string>& shaders() const { return shaders_; } // asset paths, "shaders/water.frag"
    const std::vector<std::string>& graphs() const { return graphs_; }

    void update(Engine& engine);          // before the frame: edits reach the preview, compiles finish
    void draw(Engine& engine, Rect area); // the panels
    void render(Engine& engine);          // the preview, with the game's renderer

private:
    enum class Kind { None, Material, Code, Graph };
    enum class View { Lit, Color, Glow, Normals };

    // A compile running on another thread, and what the last one that worked made.
    struct Compiler {
        std::future<std::pair<bool, std::string>> job; // ok, log
        std::shared_ptr<ShaderBundle> bundle;          // filled by the job
        std::string pending;                           // text waiting to be compiled
        f64 due = 0;                                   // when (it waits until typing pauses)
        std::string source_name;
        std::filesystem::path source_path;
        Asset<gfx::Shader> shader;                          // the last that compiled
        std::string log;
        bool failed = false;
        f64 took = 0;
    };

    // materials.cpp: lists, the material form, maps, the preview
    void list_panel(Rect area);
    void material_panel(Engine& engine, Rect area);
    void params_form(const gfx::Shader* shader, const json::Value* values, const std::function<void(const std::string&, json::Value)>& set,
                     const std::function<void(const std::string&)>& reset);
    void textures_form(const gfx::Shader* shader, const json::Value* values, const std::function<void(const std::string&, std::string)>& set);
    void glow_card(Engine& engine);
    void normal_card(Engine& engine);
    void maps_dialogs(Engine& engine);
    void stage_panel(Engine& engine, Rect area);
    void new_dialogs(Engine& engine);
    bool picture_combo(const char* id, std::string& path, bool allow_none);
    void thumbnail(const std::string& path, f32 box);
    gfx::Camera2D camera(Engine& engine) const;
    const gfx::Texture* stage_texture();
    std::optional<paint::Image> read_picture(const std::string& path) const;
    bool write_picture(const std::string& path, const paint::Image& image);
    std::string sibling(const std::string& picture, std::string_view suffix) const; // "a/b.png" -> "a/b_suffix.png"
    void shortcuts(Engine& engine);

    // Edits to the material's file, made while drawing and applied after (each one re-reads the document).
    void edit(std::function<void(json::TextDocument&)> change) { edits_.push_back(std::move(change)); }
    void set_key(json::TextDocument::Path container, const std::string& key, json::Value value);
    void erase_key(json::TextDocument::Path path);
    void apply_edits();
    void track_history();
    void undo(int direction);

    // materials_shaders.cpp: the code editor, the graph, compiling
    void code_panel(Engine& engine, Rect area);
    void try_panel(Engine& engine, Rect area); // beside code and graphs: the preview, test values, problems
    void graph_canvas(Engine& engine, Rect area);
    void graph_node_panel(Engine& engine);
    void add_node_menu(Vec2 at);
    void graph_changed(bool record = true);
    void compile_soon(const std::string& text, f64 delay);
    void finish_compiles(Engine& engine);
    void mark_problems();
    bool save_code();
    bool save_graph();
    void rebuild_try_material();

    std::filesystem::path assets_dir_;
    AssetManager* assets_ = nullptr;
    Engine* engine_ = nullptr;
    std::function<void(std::string)> say_;
    std::vector<std::string> materials_, shaders_, graphs_;
    char filter_[64] = {};
    ImFont* mono_ = nullptr;

    Kind kind_ = Kind::None;
    std::string path_;         // the open file's asset path
    std::string pending_open_; // opened once this frame's panels are drawn

    // A material
    json::TextDocument doc_;
    std::string saved_;                // its text on disk
    std::vector<std::string> history_; // undo steps (whole texts)
    usize cursor_ = 0;
    std::vector<std::function<void(json::TextDocument&)>> edits_;
    std::unique_ptr<gfx::SpriteMaterial> preview_; // what the preview draws, rebuilt from the text before each frame
    std::vector<std::string> warnings_;
    bool rebuild_ = true;

    // Code
    CodeEditor code_;
    std::string code_saved_;

    // A graph
    shadergraph::Graph graph_;
    std::string graph_saved_;                // its JSON on disk
    std::vector<std::string> graph_history_; // undo steps (whole JSON)
    usize graph_cursor_ = 0;
    std::set<u32> selected_;
    Vec2 graph_pan_{0, 0};
    f32 graph_zoom_ = 1;
    bool graph_fit_ = true;
    std::string generated_;
    std::vector<std::string> graph_problems_;
    bool show_generated_ = false;
    // Dragging on the canvas: nodes, a new link (from an output, or into an input), a box, the view.
    enum class GraphDrag { None, Nodes, Link, Box, Pan } graph_drag_ = GraphDrag::None;
    u32 drag_node_ = 0, drag_pin_ = 0;
    bool drag_from_output_ = true;
    Vec2 drag_start_;
    bool graph_moved_ = false;
    Vec2 menu_at_;                    // where the add-node menu puts the node, in canvas units
    std::optional<std::pair<u32, u32>> menu_link_; // a link dragged into nothing: the new node joins it
    bool menu_link_from_output_ = true;
    char node_filter_[64] = {};

    // Code and graphs: compiling, and the preview's test values
    Compiler compiler_;
    std::unique_ptr<gfx::SpriteMaterial> try_material_;
    std::map<std::string, std::vector<f32>, std::less<>> try_params_;
    std::map<std::string, std::string, std::less<>> try_textures_;
    bool try_rebuild_ = true;

    // The preview
    Rect stage_;
    f32 zoom_ = 4;
    Vec2 pan_;
    bool fit_ = true;
    View view_ = View::Lit;
    bool lighting_ = true, bloom_ = true;
    Color ambient_{0.32f, 0.34f, 0.42f, 1};
    Color light_color_{1.0f, 0.86f, 0.62f, 1};
    f32 light_radius_ = 96, light_height_ = 0.35f;
    Vec2 light_{-24, -20}; // from the picture's middle, in its pixels
    bool light_follows_ = false;
    gfx::Material light_look_, glow_look_;
    f32 light_look_height_ = -1;
    std::string try_picture_; // what code and graphs are shown on
    std::unique_ptr<gfx::Texture> test_card_;
    gfx::SpriteLayers layers_;
    gfx::SpriteBatch lights_;

    // Making maps: the settings, and what they'd make, shown in the preview until it's saved
    bool glow_dialog_ = false, normal_dialog_ = false;
    paint::NormalOptions normal_options_;
    f32 glow_threshold_ = 1.01f;  // brightness that glows (above 1: none)
    std::set<paint::Pixel> glow_colors_;
    std::vector<paint::Pixel> picture_colors_;
    std::unique_ptr<gfx::Texture> map_preview_;
    bool map_preview_is_normal_ = false;
    bool map_stale_ = true;

    // Dialogs for new files
    bool new_material_dialog_ = false, new_shader_dialog_ = false;
    char new_name_[96] = {};
    std::string new_picture_;
    bool new_graph_ = false;
};

} // namespace bowl::editor
