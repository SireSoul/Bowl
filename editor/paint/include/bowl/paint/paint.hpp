#pragma once

#include "bowl/core/json.hpp"
#include "bowl/core/types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

// Bowl Editor's pixel painting, without any windows: pictures in layers, a selection, the tools' shapes (brushes,
// lines, boxes, ellipses, fills, gradients), adjustments, and an undo history. The Paint workspace draws and drives
// it; tests/test_paint.cpp checks it.
namespace bowl::paint {

// A pixel: RGBA, 8 bits each, red in the low byte (the order PNGs and textures use), straight (not premultiplied) alpha.
using Pixel = u32;

constexpr Pixel rgba(u8 r, u8 g, u8 b, u8 a = 255) {
    return static_cast<Pixel>(r) | static_cast<Pixel>(g) << 8 | static_cast<Pixel>(b) << 16 | static_cast<Pixel>(a) << 24;
}
constexpr u8 red(Pixel p) { return static_cast<u8>(p); }
constexpr u8 green(Pixel p) { return static_cast<u8>(p >> 8); }
constexpr u8 blue(Pixel p) { return static_cast<u8>(p >> 16); }
constexpr u8 alpha(Pixel p) { return static_cast<u8>(p >> 24); }
constexpr Pixel clear = 0;

// `over` drawn on `under` at `opacity` (0 to 1).
Pixel blend(Pixel under, Pixel over, f32 opacity = 1);

struct Hsv {
    f32 h = 0, s = 0, v = 0; // hue in degrees, the others 0 to 1
};
Hsv to_hsv(Pixel p);
Pixel from_hsv(Hsv c, u8 a = 255);

struct Point {
    i32 x = 0, y = 0;
    bool operator==(const Point&) const = default;
};
struct Box {
    i32 x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(i32 px, i32 py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

class Image {
public:
    Image() = default;
    Image(i32 w, i32 h, Pixel fill = clear) : w_(w), h_(h), px_(static_cast<usize>(w) * static_cast<usize>(h), fill) {}
    i32 width() const { return w_; }
    i32 height() const { return h_; }
    bool inside(i32 x, i32 y) const { return x >= 0 && y >= 0 && x < w_ && y < h_; }
    Pixel get(i32 x, i32 y) const { return inside(x, y) ? px_[index(x, y)] : clear; }
    void set(i32 x, i32 y, Pixel p) {
        if (inside(x, y)) px_[index(x, y)] = p;
    }
    std::vector<Pixel>& pixels() { return px_; }
    const std::vector<Pixel>& pixels() const { return px_; }
    bool operator==(const Image&) const = default;

    // A copy of part of it (pixels outside it are clear).
    Image cut(Box b) const;
    // Another image drawn on it with its corner at (x, y), each pixel replacing (or, with `over`, blended onto) this one's.
    void paste(const Image& src, i32 x, i32 y, bool over = false);

private:
    usize index(i32 x, i32 y) const { return static_cast<usize>(y) * static_cast<usize>(w_) + static_cast<usize>(x); }
    i32 w_ = 0, h_ = 0;
    std::vector<Pixel> px_;
};

// Which pixels are selected. Empty (no pixels at all): nothing is selected, and tools paint everywhere.
class Mask {
public:
    Mask() = default;
    Mask(i32 w, i32 h) : w_(w), h_(h), on_(static_cast<usize>(w) * static_cast<usize>(h), 0) {}
    i32 width() const { return w_; }
    i32 height() const { return h_; }
    bool test(i32 x, i32 y) const { return x >= 0 && y >= 0 && x < w_ && y < h_ && on_[index(x, y)]; }
    void set(i32 x, i32 y, bool on) {
        if (x >= 0 && y >= 0 && x < w_ && y < h_) on_[index(x, y)] = on;
    }
    bool any() const;
    Box bounds() const; // of the selected pixels
    usize count() const;
    void invert();
    // Combining another mask: replace, add (Shift), or take away (Ctrl).
    enum class Mode { Replace, Add, Subtract };
    void combine(const Mask& m, Mode mode);
    bool operator==(const Mask&) const = default;
    const std::vector<u8>& bits() const { return on_; }

private:
    usize index(i32 x, i32 y) const { return static_cast<usize>(y) * static_cast<usize>(w_) + static_cast<usize>(x); }
    i32 w_ = 0, h_ = 0;
    std::vector<u8> on_;
};

// --- Shapes: the pixels a tool covers, handed to `put` ------------------------------------------------------------

using Put = std::function<void(i32 x, i32 y)>;

// A straight line, one pixel thick, with no gaps.
void line(Point a, Point b, const Put& put);
// Where a line from `a` toward `b` ends when held to flat, upright, 45 degrees, or the isometric 2:1 slope.
Point snap_line(Point a, Point b);
// A box from one corner to the other: its outline, or filled. `diamond`: the 2:1 isometric diamond inside the box.
void box(Point a, Point b, bool filled, bool diamond, const Put& put);
// The ellipse inside the box from one corner to the other: a clean one-pixel outline, or filled.
void ellipse(Point a, Point b, bool filled, const Put& put);
// A stroke's points with the L-shaped corners of one-pixel lines taken out (pixel-perfect drawing).
std::vector<Point> pixel_perfect(const std::vector<Point>& points);

struct Brush {
    i32 size = 1;          // 1 to 64
    bool square = false;   // or round
    i32 density = 16;      // 1 to 16: a 4 x 4 ordered dither; 16 paints every pixel
    bool mirror_x = false; // also paints mirrored about the canvas's middle, across
    bool mirror_y = false; //   and down
};
// The pixels one dab of the brush covers at (x, y), on a canvas of w by h (for mirroring).
void dab(const Brush& b, i32 x, i32 y, i32 w, i32 h, const Put& put);
// Whether the 4 x 4 ordered dither paints this pixel at this density (1 to 16).
bool dithered(i32 x, i32 y, i32 density);

// The pixels connected to (x, y) whose color is within `tolerance` (0 to 255, on each channel) of its color, or with
// `contiguous` off, every such pixel in the picture. For fills and the magic wand.
Mask region(const Image& img, i32 x, i32 y, i32 tolerance, bool contiguous);

// --- Changing pixels -----------------------------------------------------------------------------------------------

// From color `a` at point `from` to color `b` at point `to`, over the pixels of `area` (or everything, if it's empty);
// dithered (a 4 x 4 ordered dither between the two) or smooth.
void gradient(Image& img, const Mask& area, Point from, Point to, Pixel a, Pixel b, bool dither);
// One step lighter (+1) or darker (-1): along the colors in `ramp` when the pixel is one of them, otherwise brighter
// leaning warm, or darker leaning violet, the way painters shade.
Pixel shade(Pixel p, i32 direction, const std::vector<Pixel>& ramp = {});
// Changes the selected pixels' (or all, if `area` is empty) hue (degrees), saturation and brightness (-1 to 1).
void adjust(Image& img, const Mask& area, f32 hue, f32 saturation, f32 brightness);
// Every pixel within `tolerance` of `from` becomes `to`.
void replace(Image& img, const Mask& area, Pixel from, Pixel to, i32 tolerance);
// A one-pixel outline in `color` around what's painted (only in clear pixels next to it; `corners`: diagonally too).
void outline(Image& img, const Mask& area, Pixel color, bool corners);
// Every partly see-through pixel made solid.
void opaque(Image& img, const Mask& area);
Image flipped(const Image& img, bool across); // across: left and right swap; otherwise top and bottom
Image turned(const Image& img, bool clockwise);
// The smallest box holding every pixel that isn't clear.
Box painted_bounds(const Image& img);
// The colors in a picture, most used first.
std::vector<Pixel> colors_of(const Image& img, usize most);

// --- A picture in layers, with a selection and undo ----------------------------------------------------------------

struct Layer {
    std::string name;
    Image image;
    bool visible = true, locked = false;
    f32 opacity = 1;
};

// Pixels lifted off a layer by the Move tool or pasted: drawn over the layer at (x, y) until they're put down.
struct Floating {
    Image image;
    i32 x = 0, y = 0;
};

class Document {
public:
    Document() = default;
    // One layer holding the picture.
    explicit Document(Image picture);
    Document(i32 w, i32 h);

    i32 width() const { return w_; }
    i32 height() const { return h_; }
    std::vector<Layer> layers;
    usize active = 0;
    Mask selection;                    // as big as the canvas
    std::optional<Floating> floating;  // over the active layer
    Layer& layer() { return layers[active]; }
    const Layer& layer() const { return layers[active]; }

    // Every visible layer blended, bottom first, with what's floating.
    Image composite() const;
    // Whether a tool may change this pixel: inside the selection (when there is one) and on an unlocked layer.
    bool editable(i32 x, i32 y) const;

    // History. The document's own changes (layers, the canvas, selections, moving) record themselves; after changing a
    // layer's pixels directly (a stroke, a fill), call commit(). Its label names the step in the History panel.
    void commit(std::string label);
    bool undo();
    bool redo();
    bool can_undo() const { return cursor_ > 0; }
    bool can_redo() const { return cursor_ + 1 < history_.size(); }
    // The labels, oldest first, and which is now ("Opened" first).
    std::vector<std::string> history() const;
    usize history_now() const { return cursor_; }
    void go_to(usize step);
    // Whether anything changed since mark_saved().
    bool modified() const { return cursor_ != saved_; }
    void mark_saved() { saved_ = cursor_; }

    // Layers
    void add_layer(std::string name);
    void duplicate_layer();
    void remove_layer();
    void move_layer(i32 by); // up (+1, toward the top) or down
    void merge_down();
    void flatten();

    // The canvas. The anchor is which edge or corner stays put: 0 left/top, 0.5 middle, 1 right/bottom.
    void resize(i32 w, i32 h, f32 anchor_x, f32 anchor_y);
    void crop(Box b);
    // Crops to what's painted. Returns the box it kept (in the old picture), or an empty one if there was nothing.
    Box trim();
    void flip(bool across);

    // Selection
    void select_all();
    void select_none();
    void select_box(Box b, Mask::Mode mode);
    void select_mask(const Mask& m, Mask::Mode mode);
    void invert_selection();

    // The selection's pixels on the active layer (all of it, with no selection), and where they were.
    Image copy(Point* where = nullptr) const;
    void clear_selection_pixels(); // Delete: makes the selected pixels clear
    // Lifts pixels into `floating` (the selection, or the whole layer), leaving clear pixels behind.
    void lift();
    void put_down();               // the floating pixels onto the active layer
    void paste(const Image& img, Point at);

    // Saving layers: a document without its history, as JSON (pixels base64-encoded), and back.
    json::Value to_json() const;
    static std::optional<Document> from_json(const json::Value& v);

    // After changing `selection` by hand (the history steps do this themselves).
    void selection_changed();

private:
    struct State {
        std::string label;
        i32 w = 0, h = 0;
        std::vector<Layer> layers;
        usize active = 0;
        Mask selection;
    };
    State state(std::string label) const;
    void restore(const State& s);

    i32 w_ = 0, h_ = 0;
    bool selection_any_ = false; // selection.any(), kept so editable() is quick
    std::string floating_label_;
    std::vector<State> history_;
    usize cursor_ = 0, saved_ = 0;
};

std::string base64(const std::vector<u8>& bytes);
std::vector<u8> unbase64(std::string_view text);

} // namespace bowl::paint
