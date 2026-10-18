#pragma once

#include "bowl/app/engine.hpp"
#include "bowl/gfx/sprites.hpp"
#include "bowl/paint/paint.hpp"

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The Paint workspace: a pixel-art studio. A picture in layers on a big canvas; tools down the left (move, select,
// magic wand, pencil, eraser, fill, pick, line, box, ellipse, gradient, shade); the picked tool's settings across the
// top; color, swatches, layers and history on the right. Saving writes the PNG the game reads (and a running game
// shows every stroke at once), and keeps the layers beside the assets in art-layers/ (never shipped). The pixel work
// itself is bowl::paint (editor/paint), which the tests check.
namespace bowl::editor {

class PaintStudio {
public:
    enum class Tool { Move, Select, Wand, Pencil, Eraser, Fill, Pick, Line, Box, Ellipse, Gradient, Shade };

    void init(Engine& engine, std::filesystem::path assets_dir, std::function<void(std::string)> say);
    void scan(); // the pictures in the assets
    // Opens a picture (asset path). False if it can't be read. Unsaved work in the one open is saved first.
    bool open(const std::string& path);
    bool save();
    bool unsaved() const { return canvas_ && canvas_->doc.modified(); }
    const std::string& path() const { static const std::string none; return canvas_ ? canvas_->path : none; }
    const std::vector<std::string>& pictures() const { return pictures_; }

    void draw(Engine& engine, Rect area); // the panels
    void render(Engine& engine);          // the canvas, with the game's renderer

    // The Level's Brush tool, painting with the pencil where a picture stands: a pixel of the open picture.
    void brush(i32 x, i32 y, bool first);
    void brush_end();

private:
    struct Canvas {
        std::string path;
        std::filesystem::path file;
        paint::Document doc;
        std::unique_ptr<gfx::Texture> view;
        std::vector<u8> flat; // the composite, uploaded to `view` and the game's own texture
        bool dirty = true;
    };
    // A stroke or a shape being drawn: the active layer as it was when it began, redrawn from that each frame.
    struct Stroke {
        bool on = false;
        bool secondary = false; // right button: the second color (darker, for Shade)
        paint::Image base;
        std::vector<paint::Point> points;
        paint::Point start, last;
        std::vector<u8> touched; // Shade: each pixel once a stroke
    };

    void tools_column(Rect area);
    void options_bar(Engine& engine, Rect area);
    void side_panels(Engine& engine, Rect area);
    void color_panel();
    void swatches_panel();
    void layers_panel();
    void history_panel();
    void dialogs(Engine& engine);
    void canvas_input(Engine& engine);
    void keys(Engine& engine);
    void refresh(Engine& engine); // the composite to the textures, when something changed

    // What a drag does with each tool.
    void begin(paint::Point p, bool secondary);
    void drag(paint::Point p);
    void end();
    void paint_points(paint::Image& img, const std::vector<paint::Point>& pts, paint::Pixel color);
    paint::Point snapped_end(paint::Point a, paint::Point b) const;
    void preview_shape();
    paint::Pixel color(bool secondary) const { return secondary ? secondary_ : primary_; }
    void changed() {
        if (canvas_) canvas_->dirty = true;
    }

    gfx::Camera2D camera(Engine& engine) const;
    paint::Point pixel_at(Engine& engine, ImVec2 screen) const;
    void fit();
    std::filesystem::path layers_file(const std::string& path) const;
    void new_picture(const std::string& path, i32 w, i32 h);

    std::filesystem::path assets_dir_;
    AssetManager* assets_ = nullptr;
    Engine* engine_ = nullptr;
    std::function<void(std::string)> say_;
    std::vector<std::string> pictures_;
    std::optional<Canvas> canvas_;

    Tool tool_ = Tool::Pencil;
    paint::Brush brush_;
    bool pixel_perfect_ = true;
    i32 tolerance_ = 0;
    bool contiguous_ = true, sample_all_ = false;
    bool filled_ = false, diamond_ = false, dither_ = true;
    paint::Pixel primary_ = paint::rgba(242, 196, 92), secondary_ = paint::rgba(30, 30, 40);
    paint::Hsv hsv_ = paint::to_hsv(paint::rgba(242, 196, 92)); // the color square's state (keeps hue at grey)
    std::vector<paint::Pixel> kept_; // swatches kept, in the editor's settings
    Stroke stroke_;
    bool selecting_ = false;
    paint::Mask::Mode select_mode_ = paint::Mask::Mode::Replace;
    bool moving_ = false;
    paint::Point move_from_;
    std::optional<paint::Image> clipboard_;

    // View
    f32 zoom_ = 8;
    Vec2 pan_;
    bool grid_ = true;
    Rect canvas_area_; // the screen area between the panels
    bool fit_pending_ = true;
    gfx::SpriteBatch batch_;
    bool over_canvas_ = false;

    // Dialogs
    bool new_dialog_ = false, size_dialog_ = false, adjust_dialog_ = false, replace_dialog_ = false;
    char new_name_[96] = "art/new_picture";
    i32 new_w_ = 32, new_h_ = 32, size_w_ = 0, size_h_ = 0;
    i32 anchor_ = 7; // 0..8, left to right then top to bottom: 7 is the bottom middle (the foot)
    f32 adj_hue_ = 0, adj_sat_ = 0, adj_bright_ = 0;
    std::optional<paint::Image> adjust_base_;
    char filter_[64] = {};
};

} // namespace bowl::editor
