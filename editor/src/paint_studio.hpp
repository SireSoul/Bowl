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

// The Paint workspace: a pixel-art studio, with the tools of Aseprite, Piskel and Paintbrush. A picture in layers and
// animation frames on a big canvas; tools down the left (move, box and ellipse selection, lasso, magic wand, pencil,
// eraser, dither pen, spray, fill, recolor-all, pick, shade, blur and jumble, gradient, line, curve, box, ellipse,
// polygon, text); the picked tool's settings across the top; color, swatches and ramps, a looping preview, layers
// and history on the right; the frames, their tags and onion skin along the bottom. Saving writes the PNG the game
// reads (with frames, a sprite sheet and its Aseprite JSON), so a running game shows every stroke at once, and keeps
// layers and frames beside the assets in art-layers/ (never shipped). The pixel work itself is bowl::paint
// (editor/paint), which the tests check.
namespace bowl::editor {

class PaintStudio {
public:
    enum class Tool {
        Move, Select, Lasso, Wand, Pencil, Eraser, Dither, Spray, Fill, Recolor, Pick, Shade, Blur, Gradient, Line, Curve,
        Box, Ellipse, Polygon, Text
    };

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
    void preview_panel();
    void timeline(Rect area);
    void dialogs(Engine& engine);
    void canvas_input(Engine& engine);
    void keys(Engine& engine);
    void refresh(Engine& engine); // the composite to the textures, when something changed

    // What a drag does with each tool.
    void begin(paint::Point p, bool secondary);
    void drag(paint::Point p);
    void end();
    void paint_points(paint::Image& img, const std::vector<paint::Point>& pts, paint::Pixel color);
    // With a color for each pixel (the dither pen's checkerboard).
    void paint_points(paint::Image& img, const std::vector<paint::Point>& pts, const std::function<paint::Pixel(i32, i32)>& color);
    // A pixel a brush tool may paint: on the canvas (wrapped round in tiled mode), editable, and (with lock alpha)
    // already painted. Returns false if not; moves x and y onto the canvas in tiled mode.
    bool paintable(i32& x, i32& y) const;
    // Lasso and Polygon: a shape drawn by dragging, or clicked corner by corner.
    void finish_path();
    void cancel_path();
    // Curve: after the line is drawn, the mouse bends it until a click.
    void bend_curve(paint::Point through);
    void recolor(paint::Point p, bool secondary);
    // Opening a sprite sheet: frames cut from the PNG by its Aseprite JSON. False if there's no such JSON.
    bool open_sheet(const std::string& path, const paint::Image& png, Canvas& c);
    std::filesystem::path sheet_file(const std::string& path) const; // the JSON beside the PNG
    paint::Point snapped_end(paint::Point a, paint::Point b) const;
    void preview_shape();
    paint::Pixel color(bool secondary) const { return secondary ? secondary_ : primary_; }
    void changed() {
        if (!canvas_) return;
        canvas_->dirty = true;
        if (!stroke_.on) thumbs_stale_ = true; // a whole-picture change (undo, a frame added...): every thumbnail
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
    bool lock_alpha_ = false;               // brush tools only paint where something's painted already
    bool select_ellipse_ = false;           // the Select tool's shape
    i32 corner_ = 0;                        // a box's rounded corners
    i32 flow_ = 6;                          // the spray: pixels each frame
    bool jumble_ = false;                   // the Blur tool scatters instead
    bool recolor_layers_ = false, recolor_frames_ = false;
    char text_[128] = "Hello";
    i32 text_scale_ = 1;
    std::optional<paint::Image> custom_brush_; // Ctrl+B: the selection as a brush
    u32 seed_ = 1;
    paint::Pixel primary_ = paint::rgba(242, 196, 92), secondary_ = paint::rgba(30, 30, 40);
    paint::Hsv hsv_ = paint::to_hsv(paint::rgba(242, 196, 92)); // the color square's state (keeps hue at grey)
    std::vector<paint::Pixel> kept_; // swatches kept, in the editor's settings
    Stroke stroke_;
    bool selecting_ = false;
    paint::Mask::Mode select_mode_ = paint::Mask::Mode::Replace;
    bool moving_ = false;
    paint::Point move_from_;
    std::optional<paint::Image> clipboard_;
    // Lasso and Polygon
    std::vector<paint::Point> path_;
    bool path_dragging_ = false, path_clicking_ = false, path_secondary_ = false;
    paint::Image path_base_;
    // Curve
    bool curve_bend_ = false;
    paint::Point curve_a_, curve_b_;

    // View
    f32 zoom_ = 8;
    Vec2 pan_;
    bool grid_ = true;
    Rect canvas_area_; // the screen area between the panels
    bool fit_pending_ = true;
    gfx::SpriteBatch batch_;
    bool over_canvas_ = false;
    bool tiled_ = false;     // the picture repeated around itself, strokes wrapping round its edges (for tiles)
    i32 tile_grid_ = 0;      // brighter lines every so many pixels (0: none)
    bool onion_ = false;     // the frames before and after, faint
    std::unique_ptr<gfx::Texture> onion_view_;
    std::vector<std::unique_ptr<gfx::Texture>> thumbs_; // each frame's picture, for the timeline and preview
    bool thumbs_stale_ = true;
    // The preview
    bool playing_ = true;
    f32 play_time_ = 0;
    usize play_frame_ = 0;
    i32 preview_zoom_ = 0; // 0: as big as fits

    // Dialogs
    bool new_dialog_ = false, size_dialog_ = false, adjust_dialog_ = false, replace_dialog_ = false, scale_dialog_ = false,
         tags_dialog_ = false;
    i32 scale_w_ = 0, scale_h_ = 0;
    bool scale_keep_ = true;
    i32 ramp_steps_ = 5;
    std::vector<paint::Tag> tags_before_; // as they were when the Tags dialog opened
    char new_name_[96] = "art/new_picture";
    i32 new_w_ = 32, new_h_ = 32, size_w_ = 0, size_h_ = 0;
    i32 anchor_ = 7; // 0..8, left to right then top to bottom: 7 is the bottom middle (the foot)
    f32 adj_hue_ = 0, adj_sat_ = 0, adj_bright_ = 0;
    std::optional<paint::Image> adjust_base_;
    char filter_[64] = {};
};

} // namespace bowl::editor
