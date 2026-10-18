// The Paint workspace: Bowl Editor's pixel-art studio. See paint_studio.hpp.

#include "paint_studio.hpp"

#include "ui.hpp"

#include "bowl/core/file.hpp"
#include "bowl/gfx/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace bowl::editor {

namespace fs = std::filesystem;
using namespace bowl::paint;

namespace {

constexpr float options_h = 46, tools_w = 92, side_w = 300, timeline_h = 128;
constexpr ImGuiWindowFlags fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
const ImVec4 muted = ImGui::ColorConvertU32ToFloat4(ui::color::muted);

ImVec4 to_imgui(Pixel p) { return {red(p) / 255.0f, green(p) / 255.0f, blue(p) / 255.0f, alpha(p) / 255.0f}; }
ImTextureID texture_id(const gfx::Texture& t) { return static_cast<ImTextureID>(reinterpret_cast<intptr_t>(t.handle())); }

// FNV-1a: does the PNG still match the layers kept for it?
u64 fingerprint(const std::vector<u8>& bytes) {
    u64 h = 1469598103934665603ull;
    for (u8 b : bytes) h = (h ^ b) * 1099511628211ull;
    return h;
}

struct ToolInfo {
    PaintStudio::Tool tool;
    ui::Icon icon;
    const char* tip;
};
// The tools, two to a row, top to bottom, with their keys.
constexpr ToolInfo tool_list[] = {
    {PaintStudio::Tool::Move, ui::Icon::Pan, "Move: lift the selection (or the layer) and drag it. Enter puts it down, Esc puts it back|V"},
    {PaintStudio::Tool::Select, ui::Icon::Region, "Select a box (or an ellipse). Shift adds to the selection, Alt takes away|M"},
    {PaintStudio::Tool::Lasso, ui::Icon::Lasso, "Lasso: drag around what to select, or click corner by corner (Enter or a double-click closes it). Shift adds, Alt takes away|Q"},
    {PaintStudio::Tool::Wand, ui::Icon::Wand, "Magic wand: select an area of one color. Shift adds, Alt takes away|W"},
    {PaintStudio::Tool::Pencil, ui::Icon::Brush, "Pencil: the left button paints the first color, the right the second. Shift+click: a line from the last. Ctrl+B makes the selection a brush|B"},
    {PaintStudio::Tool::Eraser, ui::Icon::Eraser, "Eraser|E"},
    {PaintStudio::Tool::Dither, ui::Icon::Dither, "Dither pen (Piskel's): a checkerboard of the two colors. The right button swaps which goes where|N"},
    {PaintStudio::Tool::Spray, ui::Icon::Spray, "Spray can: scatters pixels while held, more the longer you hold|Y"},
    {PaintStudio::Tool::Fill, ui::Icon::Bucket, "Fill an area of one color|G"},
    {PaintStudio::Tool::Recolor, ui::Icon::Recolor, "Recolor (Piskel's paint-all-of-a-color): every pixel of the clicked color becomes the first color. Shift: in every frame|A"},
    {PaintStudio::Tool::Pick, ui::Icon::Pipette, "Pick a color from the picture (or hold Alt with any tool)|I"},
    {PaintStudio::Tool::Shade, ui::Icon::Shade, "Shade: the left button lightens, the right darkens, a step at a time|S"},
    {PaintStudio::Tool::Blur, ui::Icon::Blur, "Blur, or Jumble (Aseprite's): softens edges, or scatters pixels for a rough texture|U"},
    {PaintStudio::Tool::Gradient, ui::Icon::Gradient, "Gradient from the first color to the second, over the selection or the layer|D"},
    {PaintStudio::Tool::Line, ui::Icon::Line, "Line. Shift keeps it flat, upright, 45 degrees or on the 2:1 iso slope|L"},
    {PaintStudio::Tool::Curve, ui::Icon::Curve, "Curve: drag a line, then move the mouse to bend it and click. Esc cancels|C"},
    {PaintStudio::Tool::Box, ui::Icon::Rect, "Box (rounded, or an iso diamond). Shift: square|R"},
    {PaintStudio::Tool::Ellipse, ui::Icon::Circle, "Ellipse. Shift: circle|O"},
    {PaintStudio::Tool::Polygon, ui::Icon::Polygon, "Polygon: click corner by corner (Enter or a double-click finishes), or drag a freehand shape|P"},
    {PaintStudio::Tool::Text, ui::Icon::Text, "Text in a 3 x 5 pixel font: type it above, then click where it goes and drag it into place|T"},
};

const char* tool_name(PaintStudio::Tool t) {
    static constexpr const char* names[] = {"Move", "Select", "Lasso", "Wand", "Pencil", "Eraser", "Dither", "Spray", "Fill", "Recolor",
                                            "Pick", "Shade", "Blur", "Gradient", "Line", "Curve", "Box", "Ellipse", "Polygon", "Text"};
    return names[static_cast<int>(t)];
}

} // namespace

// --- Files ---------------------------------------------------------------------------------------------------------

void PaintStudio::init(Engine& engine, fs::path assets_dir, std::function<void(std::string)> say) {
    engine_ = &engine;
    assets_ = &engine.assets();
    assets_dir_ = std::move(assets_dir);
    say_ = std::move(say);
    // Swatches kept from last time.
    std::string kept = engine.settings().get("editor.paint.swatches", std::string());
    for (usize at = 0; at + 8 <= kept.size(); at += 9)
        kept_.push_back(static_cast<Pixel>(std::strtoul(kept.substr(at, 8).c_str(), nullptr, 16)));
    scan();
}

void PaintStudio::scan() {
    pictures_.clear();
    std::error_code ec;
    if (assets_dir_.empty()) return;
    for (auto it = fs::recursive_directory_iterator(assets_dir_, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec || !it->is_regular_file()) continue;
        std::string ext = it->path().extension().string();
        if (ext == ".png") pictures_.push_back(fs::relative(it->path(), assets_dir_, ec).generic_string());
    }
    std::sort(pictures_.begin(), pictures_.end());
}

fs::path PaintStudio::sheet_file(const std::string& path) const {
    fs::path f = assets_dir_ / path;
    f.replace_extension(".json");
    return f;
}

bool PaintStudio::open_sheet(const std::string& path, const Image& png, Canvas& c) {
    auto text = read_file_text(sheet_file(path));
    if (!text) return false;
    auto doc = json::parse(*text);
    if (!doc) return false;
    const json::Value& frames = doc.value["frames"];
    // Aseprite's "array" layout, or its "hash" one (frame names as keys, in order).
    std::vector<const json::Value*> list;
    if (frames.is_array())
        for (const json::Value& f : frames.array()) list.push_back(&f);
    else if (frames.is_object())
        for (const auto& [name, f] : frames.object()) list.push_back(&f);
    if (list.size() < 2) return false;
    std::string image(doc.value["meta"]["image"].as_string());
    if (!image.empty() && fs::path(image).filename() != fs::path(path).filename()) return false; // a sheet for another picture
    // Every frame the same size, untrimmed, inside the picture: otherwise it's opened as one picture.
    std::vector<Image> cut;
    std::vector<i32> durations;
    i32 w = 0, h = 0;
    for (const json::Value* f : list) {
        const json::Value& r = (*f)["frame"];
        Box b{static_cast<i32>(r["x"].as_int()), static_cast<i32>(r["y"].as_int()), static_cast<i32>(r["w"].as_int()), static_cast<i32>(r["h"].as_int())};
        if (cut.empty()) w = b.w, h = b.h;
        if (b.w != w || b.h != h || b.w <= 0 || b.x + b.w > png.width() || b.y + b.h > png.height() || (*f)["trimmed"].as_bool(false)) return false;
        cut.push_back(png.cut(b));
        durations.push_back(static_cast<i32>((*f)["duration"].as_int(100)));
    }
    std::vector<Tag> tags;
    for (const json::Value& t : doc.value["meta"]["frameTags"].array()) {
        Tag tag{std::string(t["name"].as_string()), static_cast<usize>(t["from"].as_int()), static_cast<usize>(t["to"].as_int()),
                std::string(t["direction"].as_string("forward"))};
        if (tag.from <= tag.to && tag.to < cut.size()) tags.push_back(std::move(tag));
    }
    c.doc = Document::from_frames(std::move(cut), std::move(durations), std::move(tags));
    return true;
}

fs::path PaintStudio::layers_file(const std::string& path) const {
    return assets_dir_.parent_path() / "art-layers" / (path + ".json");
}

bool PaintStudio::open(const std::string& path) {
    if (canvas_ && canvas_->path == path) return true;
    if (unsaved()) save(); // painting never loses work
    fs::path file = assets_dir_ / path;
    auto bytes = read_file_bytes(file);
    u32 w = 0, h = 0;
    std::vector<u8> rgba8;
    if (!bytes || !gfx::decode_png(*bytes, w, h, rgba8)) {
        say_("Can't open " + path + " for painting");
        return false;
    }
    Image img(static_cast<i32>(w), static_cast<i32>(h));
    std::memcpy(img.pixels().data(), rgba8.data(), rgba8.size());
    Canvas c{path, file, Document(img)};
    // Its layers (and frames), if they were kept and the PNG hasn't been repainted elsewhere since; otherwise, a sprite
    // sheet's frames, cut out by the Aseprite JSON beside it.
    bool layered = false;
    if (auto text = read_file_text(layers_file(path))) {
        auto doc = json::parse(*text);
        if (doc && static_cast<u64>(doc.value["png"].as_number()) == fingerprint(*bytes) % (1ull << 52))
            if (auto kept = Document::from_json(doc.value["document"])) c.doc = std::move(*kept), layered = true;
    }
    if (!layered) open_sheet(path, img, c);
    canvas_ = std::move(c);
    fit_pending_ = true;
    stroke_ = {};
    path_.clear();
    path_dragging_ = path_clicking_ = curve_bend_ = false;
    thumbs_.clear();
    thumbs_stale_ = true;
    play_frame_ = 0;
    return true;
}

bool PaintStudio::save() {
    if (!canvas_) return false;
    Canvas& c = *canvas_;
    c.doc.put_down();
    // One frame: the picture. More: a sprite sheet, its frames in a row, with Aseprite's JSON beside it (which the
    // game's sprite sheets read: frames, their times, and tags as animations).
    bool animated = c.doc.frame_count() > 1;
    std::error_code ec;
    fs::path sheet_json = sheet_file(c.path);
    Image flat = animated ? c.doc.sheet() : c.doc.composite();
    std::span<const u8> bytes(reinterpret_cast<const u8*>(flat.pixels().data()), flat.pixels().size() * 4);
    if (!gfx::write_png(c.file, static_cast<u32>(flat.width()), static_cast<u32>(flat.height()), bytes)) {
        say_("Couldn't save " + c.path);
        return false;
    }
    if (animated || fs::exists(sheet_json, ec)) {
        json::Value made = c.doc.sheet_json(fs::path(c.path).filename().string());
        // Keep what else was in it (Aseprite's slices, a pivot for the feet), replacing the frames and tags.
        if (auto old = read_file_text(sheet_json))
            if (auto parsed = json::parse(*old); parsed && parsed.value.is_object() && parsed.value["meta"].is_object()) {
                json::Value merged = parsed.value;
                json::Value meta = parsed.value["meta"];
                for (const char* k : {"image", "size", "frameTags", "format"}) meta.set(k, made["meta"][k]);
                merged.set("meta", std::move(meta));
                merged.set("frames", made["frames"]);
                made = std::move(merged);
            }
        std::string text = json::stringify(made, 2);
        write_file_bytes(sheet_json, text.data(), text.size());
    }
    // Layers are kept beside the assets (never shipped), with a fingerprint of the PNG they flatten to.
    fs::path lf = layers_file(c.path);
    if (c.doc.layers.size() > 1) {
        auto png = read_file_bytes(c.file);
        json::Value doc;
        doc.set("png", static_cast<f64>(fingerprint(png ? *png : std::vector<u8>{}) % (1ull << 52)));
        doc.set("document", c.doc.to_json());
        std::string text = json::stringify(doc, -1);
        fs::create_directories(lf.parent_path(), ec);
        write_file_bytes(lf, text.data(), text.size());
    } else {
        fs::remove(lf, ec);
    }
    c.doc.mark_saved();
    say_("Saved " + c.path);
    if (std::find(pictures_.begin(), pictures_.end(), c.path) == pictures_.end()) scan();
    return true;
}

void PaintStudio::new_picture(const std::string& path, i32 w, i32 h) {
    if (unsaved()) save();
    canvas_ = Canvas{path, assets_dir_ / path, Document(w, h)};
    thumbs_.clear();
    thumbs_stale_ = true;
    std::error_code ec;
    fs::create_directories(canvas_->file.parent_path(), ec);
    canvas_->doc.commit("New picture"); // unsaved until it's saved
    fit_pending_ = true;
}

void PaintStudio::refresh(Engine& engine) {
    if (!canvas_ || !canvas_->dirty) return;
    Canvas& c = *canvas_;
    Document& d = c.doc;
    auto upload = [&](std::unique_ptr<gfx::Texture>& tex, const Image& img, const char* name) {
        u32 w = static_cast<u32>(img.width()), h = static_cast<u32>(img.height());
        const void* px = img.pixels().data();
        if (!tex || tex->width() != w || tex->height() != h)
            tex = engine.gpu().create_texture({.width = w, .height = h, .rgba8 = px, .filter = gfx::Filter::Nearest, .mipmaps = false, .name = name});
        else
            engine.gpu().update_texture(*tex, px, false);
    };
    Image flat = d.composite();
    c.flat.resize(flat.pixels().size() * 4);
    std::memcpy(c.flat.data(), flat.pixels().data(), c.flat.size());
    upload(c.view, flat, "paint canvas");

    // The frames' thumbnails: while a stroke goes on, only this frame's changes. They're enlarged in whole pixels first,
    // so the preview stays crisp (ImGui smooths what it draws).
    i32 big = std::max(d.width(), d.height());
    i32 k = std::clamp(192 / std::max(1, big), 1, 16);
    auto thumb = [&](const Image& img) { return k > 1 ? scaled(img, img.width() * k, img.height() * k) : img; };
    thumbs_.resize(d.frame_count());
    for (usize f = 0; f < d.frame_count(); ++f)
        if (f == d.frame()) upload(thumbs_[f], thumb(flat), "paint frame");
        else if (thumbs_stale_ || !thumbs_[f]) upload(thumbs_[f], thumb(d.composite(f)), "paint frame");
    thumbs_stale_ = stroke_.on || path_dragging_ || curve_bend_ ? thumbs_stale_ : false;

    // The onion skin: the frame before in red, the one after in blue, faint.
    if (onion_ && d.frame_count() > 1) {
        Image skin(d.width(), d.height());
        auto tint = [&](usize f, Pixel color) {
            Image img = d.composite(f);
            for (i32 y = 0; y < img.height(); ++y)
                for (i32 x = 0; x < img.width(); ++x)
                    if (u8 a = alpha(img.get(x, y)))
                        skin.set(x, y, blend(skin.get(x, y), (color & 0xffffff) | static_cast<Pixel>(a * 90 / 255) << 24));
        };
        if (d.frame() > 0) tint(d.frame() - 1, rgba(230, 70, 70));
        if (d.frame() + 1 < d.frame_count()) tint(d.frame() + 1, rgba(70, 120, 240));
        upload(onion_view_, skin, "paint onion skin");
    } else {
        onion_view_.reset();
    }

    // The game's texture too, so the level (and a running game, once saved) shows the stroke: the sheet, with frames.
    if (auto tex = engine.assets().load<gfx::Texture>(c.path)) {
        if (d.frame_count() == 1 && tex->width() == static_cast<u32>(flat.width()) && tex->height() == static_cast<u32>(flat.height())) {
            engine.gpu().update_texture(*tex, c.flat.data(), false);
        } else if (d.frame_count() > 1 && !stroke_.on) {
            Image sheet = d.sheet();
            if (tex->width() == static_cast<u32>(sheet.width()) && tex->height() == static_cast<u32>(sheet.height()))
                engine.gpu().update_texture(*tex, sheet.pixels().data(), false);
        }
    }
    c.dirty = stroke_.on && d.frame_count() > 1; // the sheet once the stroke's done
}

// --- The Level's Brush ---------------------------------------------------------------------------------------------

void PaintStudio::brush(i32 x, i32 y, bool first) {
    if (!canvas_) return;
    Tool was = tool_;
    tool_ = Tool::Pencil;
    if (first) begin({x, y}, false);
    else drag({x, y});
    tool_ = was;
}

void PaintStudio::brush_end() {
    Tool was = tool_;
    tool_ = Tool::Pencil;
    end();
    tool_ = was;
    if (engine_) refresh(*engine_);
}

// --- Tools ---------------------------------------------------------------------------------------------------------

bool PaintStudio::paintable(i32& x, i32& y) const {
    const Document& d = canvas_->doc;
    if (tiled_) x = ((x % d.width()) + d.width()) % d.width(), y = ((y % d.height()) + d.height()) % d.height();
    if (!d.editable(x, y)) return false;
    // Lock alpha (Aseprite's): only over what's painted already, so shading stays inside the shape.
    bool erasing = tool_ == Tool::Eraser;
    if (lock_alpha_ && !erasing && stroke_.base.inside(x, y) && !alpha(stroke_.base.get(x, y))) return false;
    return true;
}

void PaintStudio::paint_points(Image& img, const std::vector<Point>& pts, Pixel c) {
    paint_points(img, pts, [c](i32, i32) { return c; });
}

void PaintStudio::paint_points(Image& img, const std::vector<Point>& pts, const std::function<Pixel(i32, i32)>& color) {
    Document& d = canvas_->doc;
    // A custom brush stamps its own picture (in the second color with the right button; the eraser erases its shape).
    bool stamp = custom_brush_ && (tool_ == Tool::Pencil || tool_ == Tool::Eraser);
    for (Point p : pts) {
        if (stamp) {
            const Image& b = *custom_brush_;
            for (i32 by = 0; by < b.height(); ++by)
                for (i32 bx = 0; bx < b.width(); ++bx) {
                    Pixel bp = b.get(bx, by);
                    if (!alpha(bp)) continue;
                    i32 x = p.x + bx - b.width() / 2, y = p.y + by - b.height() / 2;
                    if (!paintable(x, y)) continue;
                    img.set(x, y, tool_ == Tool::Eraser ? clear : stroke_.secondary ? (secondary_ & 0xffffff) | (bp & 0xff000000) : bp);
                }
            continue;
        }
        dab(brush_, p.x, p.y, d.width(), d.height(), [&](i32 x, i32 y) {
            if (paintable(x, y)) img.set(x, y, color(x, y));
        });
    }
}

Point PaintStudio::snapped_end(Point a, Point b) const {
    bool shift = ImGui::GetIO().KeyShift;
    if (!shift) return b;
    if (tool_ == Tool::Line) return snap_line(a, b);
    // Boxes and ellipses: square (a 2:1 diamond for iso diamonds).
    i32 dx = b.x - a.x, dy = b.y - a.y;
    if (tool_ == Tool::Box && diamond_) {
        i32 k = std::max(std::abs(dx), std::abs(dy) * 2);
        return {a.x + (dx < 0 ? -k : k), a.y + (dy < 0 ? -k / 2 : k / 2)};
    }
    i32 k = std::max(std::abs(dx), std::abs(dy));
    return {a.x + (dx < 0 ? -k : k), a.y + (dy < 0 ? -k : k)};
}

void PaintStudio::begin(Point p, bool secondary) {
    if (!canvas_) return;
    Document& d = canvas_->doc;
    bool shift = ImGui::GetIO().KeyShift, alt = ImGui::GetIO().KeyAlt, cmd = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
    // Alt with a painting tool picks a color for a moment.
    Tool t = tool_;
    bool painting = t == Tool::Pencil || t == Tool::Fill || t == Tool::Line || t == Tool::Box || t == Tool::Ellipse || t == Tool::Gradient ||
                    t == Tool::Dither || t == Tool::Spray || t == Tool::Recolor || t == Tool::Curve || t == Tool::Polygon;
    if (alt && painting && !path_clicking_) t = Tool::Pick;
    // In tiled mode, a click beside the picture lands on the same spot in it.
    if (tiled_ && (t == Tool::Pick || t == Tool::Fill || t == Tool::Recolor || t == Tool::Wand))
        p = {((p.x % d.width()) + d.width()) % d.width(), ((p.y % d.height()) + d.height()) % d.height()};
    // A curve being bent: this click puts it down.
    if (curve_bend_) {
        curve_bend_ = false;
        stroke_.on = false;
        if (!(d.layer().image == stroke_.base)) d.commit("Curve");
        changed();
        return;
    }
    switch (t) {
    case Tool::Pick: {
        Pixel c = d.composite().get(p.x, p.y);
        (secondary ? secondary_ : primary_) = c;
        if (!secondary) hsv_ = to_hsv(c);
        return;
    }
    case Tool::Select:
        selecting_ = true;
        select_mode_ = shift ? Mask::Mode::Add : alt || cmd ? Mask::Mode::Subtract : Mask::Mode::Replace;
        stroke_ = {.on = true, .start = p, .last = p};
        return;
    case Tool::Wand: {
        Image sample = sample_all_ ? d.composite() : d.layer().image;
        d.select_mask(region(sample, p.x, p.y, tolerance_, contiguous_),
                      shift ? Mask::Mode::Add : alt || cmd ? Mask::Mode::Subtract : Mask::Mode::Replace);
        changed();
        return;
    }
    case Tool::Move:
        if (d.layer().locked) {
            say_("That layer is locked");
            return;
        }
        if (!d.floating) d.lift();
        moving_ = true;
        move_from_ = p;
        changed();
        return;
    case Tool::Fill: {
        if (d.layer().locked) return;
        Image sample = sample_all_ ? d.composite() : d.layer().image;
        Mask area = region(sample, p.x, p.y, tolerance_, contiguous_);
        Pixel c = color(secondary);
        for (i32 y = 0; y < d.height(); ++y)
            for (i32 x = 0; x < d.width(); ++x)
                if (area.test(x, y) && d.editable(x, y)) d.layer().image.set(x, y, c);
        d.commit("Fill");
        changed();
        return;
    }
    case Tool::Recolor:
        recolor(p, secondary);
        return;
    case Tool::Text: {
        if (d.layer().locked) {
            say_("That layer is locked");
            return;
        }
        Point size = text_size(text_, text_scale_);
        if (size.x <= 0 || size.y <= 0) {
            say_("Type some text first (above)");
            return;
        }
        Image img(size.x, size.y);
        Pixel c = color(secondary);
        text(text_, {0, 0}, text_scale_, [&](i32 x, i32 y) { img.set(x, y, c); });
        d.paste(img, p);
        tool_ = Tool::Move; // drag it into place; Enter puts it down
        changed();
        return;
    }
    case Tool::Lasso:
    case Tool::Polygon: {
        if (t == Tool::Polygon && d.layer().locked) {
            say_("That layer is locked");
            return;
        }
        if (path_clicking_) { // another corner, or closing the shape
            bool closes = path_.size() >= 3 && (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ||
                                                (std::abs(p.x - path_.front().x) <= 1 && std::abs(p.y - path_.front().y) <= 1));
            if (closes) finish_path();
            else if (!(p == path_.back())) path_.push_back(p);
            changed();
            return;
        }
        if (t == Tool::Lasso) select_mode_ = shift ? Mask::Mode::Add : alt || cmd ? Mask::Mode::Subtract : Mask::Mode::Replace;
        path_ = {p};
        path_dragging_ = true;
        path_secondary_ = secondary;
        path_base_ = d.layer().image;
        stroke_ = Stroke{.on = true, .secondary = secondary, .base = d.layer().image, .start = p, .last = p};
        return;
    }
    default: break;
    }
    if (d.layer().locked) {
        say_("That layer is locked");
        return;
    }
    // Strokes and shapes: drawn from the layer as it was, each frame.
    Point from = p;
    bool freehand = t == Tool::Pencil || t == Tool::Eraser || t == Tool::Dither;
    bool line_from_last = shift && freehand && stroke_.last.x >= 0 && !stroke_.points.empty();
    if (line_from_last) from = stroke_.last;
    stroke_ = Stroke{.on = true, .secondary = secondary, .base = d.layer().image, .start = from, .last = p};
    stroke_.touched.assign(static_cast<usize>(d.width()) * static_cast<usize>(d.height()), 0);
    if (freehand || t == Tool::Shade) {
        if (line_from_last) line(from, p, [&](i32 x, i32 y) { stroke_.points.push_back({x, y}); });
        else stroke_.points.push_back(p);
    }
    drag(p);
}

void PaintStudio::drag(Point p) {
    if (!canvas_) return;
    Document& d = canvas_->doc;
    if (moving_ && d.floating) {
        d.floating->x += p.x - move_from_.x;
        d.floating->y += p.y - move_from_.y;
        move_from_ = p;
        changed();
        return;
    }
    if (path_dragging_) {
        if (!(p == path_.back())) line(path_.back(), p, [&](i32 x, i32 y) {
            if (!(Point{x, y} == path_.back())) path_.push_back({x, y});
        });
        if (tool_ == Tool::Polygon) { // the shape so far, closed
            d.layer().image = path_base_;
            std::vector<Point> pts;
            polygon(path_, filled_, [&](i32 x, i32 y) { pts.push_back({x, y}); });
            Image& img = d.layer().image;
            if (filled_) {
                for (Point q : pts)
                    if (paintable(q.x, q.y)) img.set(q.x, q.y, color(path_secondary_));
            } else {
                paint_points(img, pts, color(path_secondary_));
            }
            changed();
        }
        return;
    }
    if (!stroke_.on) return;
    if (tool_ == Tool::Select) {
        stroke_.last = p;
        return;
    }
    Image& img = d.layer().image;
    switch (tool_) {
    case Tool::Pencil:
    case Tool::Eraser:
    case Tool::Dither: {
        if (!(p == stroke_.last) || stroke_.points.size() <= 1)
            line(stroke_.last, p, [&](i32 x, i32 y) {
                if (stroke_.points.empty() || !(stroke_.points.back() == Point{x, y})) stroke_.points.push_back({x, y});
            });
        stroke_.last = p;
        img = stroke_.base;
        bool perfect = pixel_perfect_ && brush_.size == 1 && !custom_brush_;
        const std::vector<Point>& pts = perfect ? pixel_perfect(stroke_.points) : stroke_.points;
        if (tool_ == Tool::Dither) {
            // A checkerboard of the two colors, fixed to the canvas so strokes line up.
            Pixel a = color(stroke_.secondary), b = color(!stroke_.secondary);
            paint_points(img, pts, [a, b](i32 x, i32 y) { return (x + y) % 2 == 0 ? a : b; });
        } else {
            paint_points(img, pts, tool_ == Tool::Eraser ? clear : color(stroke_.secondary));
        }
        break;
    }
    case Tool::Spray: {
        // More pixels every frame it's held, around the brush: not redrawn from the start like a stroke.
        i32 radius = std::max(2, brush_.size);
        Pixel c = color(stroke_.secondary);
        spray(p, radius, flow_, seed_, [&](i32 x, i32 y) {
            if (paintable(x, y)) img.set(x, y, c);
        });
        stroke_.last = p;
        break;
    }
    case Tool::Shade:
    case Tool::Blur: {
        if (tool_ == Tool::Blur && p == stroke_.last && stroke_.points.size() > 1) break; // only where the brush moves
        stroke_.points.push_back(p);
        Image before = img; // blur reads the picture as it was this frame
        line(stroke_.last, p, [&](i32 x, i32 y) {
            dab(brush_, x, y, d.width(), d.height(), [&](i32 px, i32 py) {
                if (!paintable(px, py)) return;
                if (tool_ == Tool::Blur) {
                    img.set(px, py, jumble_ ? jumbled(before, px, py, seed_) : blurred(before, px, py));
                    return;
                }
                u8& seen = stroke_.touched[static_cast<usize>(py) * static_cast<usize>(d.width()) + static_cast<usize>(px)];
                if (seen) return; // once a stroke: go over it again for another step
                seen = 1;
                img.set(px, py, shade(img.get(px, py), stroke_.secondary ? -1 : 1, kept_));
            });
        });
        stroke_.last = p;
        break;
    }
    case Tool::Line:
    case Tool::Curve:
    case Tool::Box:
    case Tool::Ellipse: {
        stroke_.last = tool_ == Tool::Curve ? (ImGui::GetIO().KeyShift ? snap_line(stroke_.start, p) : p) : snapped_end(stroke_.start, p);
        img = stroke_.base;
        std::vector<Point> pts;
        auto collect = [&](i32 x, i32 y) { pts.push_back({x, y}); };
        if (tool_ == Tool::Line || tool_ == Tool::Curve) line(stroke_.start, stroke_.last, collect);
        else if (tool_ == Tool::Box && !diamond_ && corner_ > 0) rounded_box(stroke_.start, stroke_.last, corner_, filled_, collect);
        else if (tool_ == Tool::Box) box(stroke_.start, stroke_.last, filled_, diamond_, collect);
        else ellipse(stroke_.start, stroke_.last, filled_, collect);
        // Outlines use the brush's size; filled shapes are filled pixel by pixel.
        bool solid = filled_ && tool_ != Tool::Line && tool_ != Tool::Curve;
        if (solid) {
            for (Point q : pts)
                if (paintable(q.x, q.y)) img.set(q.x, q.y, color(stroke_.secondary));
        } else {
            paint_points(img, pts, color(stroke_.secondary));
        }
        break;
    }
    case Tool::Gradient: {
        stroke_.last = ImGui::GetIO().KeyShift ? snap_line(stroke_.start, p) : p;
        img = stroke_.base;
        Mask area = d.selection.any() ? d.selection : Mask();
        Image g = img;
        gradient(g, area, stroke_.start, stroke_.last, color(stroke_.secondary), color(!stroke_.secondary), dither_);
        for (i32 y = 0; y < d.height(); ++y)
            for (i32 x = 0; x < d.width(); ++x)
                if (d.editable(x, y)) img.set(x, y, g.get(x, y));
        break;
    }
    default: break;
    }
    changed();
}

void PaintStudio::end() {
    if (!canvas_) return;
    Document& d = canvas_->doc;
    if (moving_) {
        moving_ = false;
        return; // still floating: Enter (or another tool) puts it down
    }
    if (path_dragging_) {
        path_dragging_ = false;
        if (path_.size() <= 1) { // a click, not a drag: corners from here on, a click at a time
            path_clicking_ = true;
            stroke_.on = false;
        } else {
            finish_path();
        }
        changed();
        return;
    }
    if (!stroke_.on) return;
    if (tool_ == Tool::Curve) { // the line's drawn: now bend it
        curve_a_ = stroke_.start, curve_b_ = stroke_.last;
        curve_bend_ = !(curve_a_ == curve_b_);
        if (!curve_bend_) stroke_.on = false;
        return;
    }
    stroke_.on = false;
    if (tool_ == Tool::Select) {
        selecting_ = false;
        Point a = stroke_.start, b = stroke_.last;
        Box bx{std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x) + 1, std::abs(b.y - a.y) + 1};
        if (a == b && select_mode_ == Mask::Mode::Replace) {
            d.select_none(); // a click: nothing selected
        } else if (select_ellipse_) {
            Mask m(d.width(), d.height());
            ellipse(a, b, true, [&](i32 x, i32 y) { m.set(x, y, true); });
            d.select_mask(m, select_mode_);
        } else {
            d.select_box(bx, select_mode_);
        }
        changed();
        return;
    }
    if (!(d.layer().image == stroke_.base)) d.commit(tool_name(tool_));
}

void PaintStudio::finish_path() {
    Document& d = canvas_->doc;
    std::vector<Point> pts = path_;
    bool lasso = tool_ == Tool::Lasso;
    path_.clear();
    path_dragging_ = path_clicking_ = false;
    stroke_.on = false;
    if (pts.size() < 3 && lasso) return;
    if (lasso) {
        Mask m(d.width(), d.height());
        polygon(pts, true, [&](i32 x, i32 y) { m.set(x, y, true); });
        d.select_mask(m, select_mode_);
    } else {
        d.layer().image = path_base_;
        stroke_.base = path_base_;
        std::vector<Point> shape;
        polygon(pts, filled_, [&](i32 x, i32 y) { shape.push_back({x, y}); });
        if (filled_) {
            for (Point q : shape)
                if (paintable(q.x, q.y)) d.layer().image.set(q.x, q.y, color(path_secondary_));
        } else {
            paint_points(d.layer().image, shape, color(path_secondary_));
        }
        if (!(d.layer().image == path_base_)) d.commit("Polygon");
    }
    changed();
}

void PaintStudio::cancel_path() {
    if (path_.empty() && !curve_bend_) return;
    if (tool_ == Tool::Polygon && canvas_ && !path_.empty()) canvas_->doc.layer().image = path_base_;
    if (curve_bend_ && canvas_) canvas_->doc.layer().image = stroke_.base;
    path_.clear();
    path_dragging_ = path_clicking_ = curve_bend_ = false;
    stroke_.on = false;
    changed();
}

void PaintStudio::bend_curve(Point through) {
    Document& d = canvas_->doc;
    Image& img = d.layer().image;
    img = stroke_.base;
    std::vector<Point> pts;
    curve(curve_a_, control_through(curve_a_, curve_b_, through), curve_b_, [&](i32 x, i32 y) { pts.push_back({x, y}); });
    paint_points(img, pts, color(stroke_.secondary));
    changed();
}

void PaintStudio::recolor(Point p, bool secondary) {
    Document& d = canvas_->doc;
    if (!d.layer().image.inside(p.x, p.y)) return;
    Pixel from = d.layer().image.get(p.x, p.y), to = color(secondary);
    if (from == to) return;
    // On this layer (or every layer), in this frame (or every frame: Shift, or the option).
    bool frames = recolor_frames_ || ImGui::GetIO().KeyShift;
    usize shown = d.frame(), active = d.active;
    for (usize f = 0; f < d.frame_count(); ++f) {
        if (!frames && f != shown) continue;
        d.show_frame(f);
        for (usize li = 0; li < d.layers.size(); ++li) {
            if ((!recolor_layers_ && li != active) || d.layers[li].locked) continue;
            replace(d.layers[li].image, d.selection, from, to, tolerance_);
        }
    }
    d.show_frame(shown);
    d.commit("Recolor");
    changed();
}

// --- View ----------------------------------------------------------------------------------------------------------

gfx::Camera2D PaintStudio::camera(Engine& engine) const {
    gfx::Camera2D cam = engine.camera();
    cam.zoom = zoom_;
    cam.rotation = 0;
    const ImGuiIO& io = ImGui::GetIO();
    Vec2 area_center = canvas_area_.center();
    Vec2 screen_center{io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f};
    Vec2 canvas_center = canvas_ ? Vec2{canvas_->doc.width() * 0.5f, canvas_->doc.height() * 0.5f} : Vec2{};
    cam.position = canvas_center + pan_ - (area_center - screen_center) / zoom_;
    return cam;
}

Point PaintStudio::pixel_at(Engine& engine, ImVec2 screen) const {
    Vec2 p = camera(engine).screen_to_world({screen.x, screen.y});
    return {static_cast<i32>(std::floor(p.x)), static_cast<i32>(std::floor(p.y))};
}

void PaintStudio::fit() {
    if (!canvas_ || canvas_area_.w <= 0) return;
    f32 k = std::min(canvas_area_.w / static_cast<f32>(canvas_->doc.width()), canvas_area_.h / static_cast<f32>(canvas_->doc.height())) * 0.85f;
    zoom_ = std::clamp(k >= 1 ? std::floor(k) : k, 0.25f, 64.0f);
    pan_ = {};
}

void PaintStudio::canvas_input(Engine& engine) {
    const ImGuiIO& io = ImGui::GetIO();
    Vec2 mouse{io.MousePos.x, io.MousePos.y};
    over_canvas_ = canvas_area_.contains(mouse) && !io.WantCaptureMouse;
    if (!canvas_) return;
    gfx::Camera2D cam = camera(engine);
    Point p = pixel_at(engine, io.MousePos);
    bool space = ImGui::IsKeyDown(ImGuiKey_Space) && !io.WantTextInput;
    if (over_canvas_) {
        if (io.MouseWheel != 0) {
            Vec2 at = cam.screen_to_world(mouse);
            f32 before = zoom_;
            zoom_ = std::clamp(zoom_ * (io.MouseWheel > 0 ? 1.25f : 0.8f), 0.25f, 64.0f);
            pan_ += (at - cam.position) * (1 - before / zoom_); // toward the mouse
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || (space && ImGui::IsMouseDragging(ImGuiMouseButton_Left)))
            pan_ -= Vec2{io.MouseDelta.x, io.MouseDelta.y} / zoom_;
        if (!space) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) begin(p, false);
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) begin(p, true);
        }
    }
    if (curve_bend_) {
        bend_curve(p);
        return;
    }
    if (stroke_.on || moving_ || path_dragging_) {
        bool held = ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right);
        if (held) drag(p);
        else end();
    }
}

void PaintStudio::keys(Engine& engine) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || !canvas_) return;
    Document& d = canvas_->doc;
    bool cmd = io.KeyCtrl || io.KeySuper, shift = io.KeyShift;
    auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
    if (cmd) {
        if (key(ImGuiKey_Z)) shift ? d.redo() : d.undo(), changed();
        else if (key(ImGuiKey_Y)) d.redo(), changed();
        else if (key(ImGuiKey_S)) save();
        else if (key(ImGuiKey_A)) d.select_all(), changed();
        else if (key(ImGuiKey_D)) d.select_none(), changed();
        else if (key(ImGuiKey_I) && shift) d.invert_selection(), changed();
        else if (key(ImGuiKey_C)) clipboard_ = d.copy();
        else if (key(ImGuiKey_X)) {
            clipboard_ = d.copy();
            d.clear_selection_pixels();
            changed();
        } else if (key(ImGuiKey_V) && clipboard_) {
            Point at{0, 0};
            if (d.selection.any()) at = {d.selection.bounds().x, d.selection.bounds().y};
            d.paste(*clipboard_, at);
            tool_ = Tool::Move;
            changed();
        } else if (key(ImGuiKey_B) && shift) {
            custom_brush_.reset();
        } else if (key(ImGuiKey_B)) { // the selection (or the clipboard) as a brush, as Aseprite does
            if (d.selection.any()) custom_brush_ = d.copy();
            else if (clipboard_) custom_brush_ = clipboard_;
            else say_("Select something first: it becomes the brush");
            if (custom_brush_) {
                if (tool_ != Tool::Eraser) tool_ = Tool::Pencil;
                say_(std::format("A {} x {} brush. Ctrl+Shift+B goes back to the round one", custom_brush_->width(), custom_brush_->height()));
            }
        }
        return;
    }
    // A shape being clicked out, or a curve being bent: Enter finishes, Esc lets it go.
    if (path_clicking_ || curve_bend_) {
        if ((key(ImGuiKey_Enter) || key(ImGuiKey_KeypadEnter)) && path_clicking_) finish_path();
        else if ((key(ImGuiKey_Enter) || key(ImGuiKey_KeypadEnter)) && curve_bend_) begin(stroke_.last, stroke_.secondary);
        if (key(ImGuiKey_Escape)) cancel_path();
        return;
    }
    // Frames: comma and full stop step through them (as Aseprite's keys do).
    if (key(ImGuiKey_Comma) && d.frame_count() > 1) d.show_frame((d.frame() + d.frame_count() - 1) % d.frame_count()), changed();
    if (key(ImGuiKey_Period) && d.frame_count() > 1) d.show_frame((d.frame() + 1) % d.frame_count()), changed();
    // Tools.
    struct K {
        ImGuiKey key;
        Tool tool;
    };
    for (K k : {K{ImGuiKey_V, Tool::Move}, {ImGuiKey_M, Tool::Select}, {ImGuiKey_Q, Tool::Lasso}, {ImGuiKey_W, Tool::Wand},
                {ImGuiKey_B, Tool::Pencil}, {ImGuiKey_E, Tool::Eraser}, {ImGuiKey_N, Tool::Dither}, {ImGuiKey_Y, Tool::Spray},
                {ImGuiKey_G, Tool::Fill}, {ImGuiKey_A, Tool::Recolor}, {ImGuiKey_I, Tool::Pick}, {ImGuiKey_S, Tool::Shade},
                {ImGuiKey_U, Tool::Blur}, {ImGuiKey_D, Tool::Gradient}, {ImGuiKey_L, Tool::Line}, {ImGuiKey_C, Tool::Curve},
                {ImGuiKey_R, Tool::Box}, {ImGuiKey_O, Tool::Ellipse}, {ImGuiKey_P, Tool::Polygon}, {ImGuiKey_T, Tool::Text}})
        if (key(k.key) && !(shift && d.floating && (k.key == ImGuiKey_V || k.key == ImGuiKey_R))) {
            if (k.tool != Tool::Move) d.put_down(), changed();
            tool_ = k.tool;
        }
    if (key(ImGuiKey_X)) std::swap(primary_, secondary_), hsv_ = to_hsv(primary_);
    if (key(ImGuiKey_LeftBracket)) brush_.size = std::max(1, brush_.size - 1);
    if (key(ImGuiKey_RightBracket)) brush_.size = std::min(64, brush_.size + 1);
    if (key(ImGuiKey_0)) fit();
    if (key(ImGuiKey_1)) zoom_ = 1, pan_ = {};
    if (key(ImGuiKey_K)) grid_ = !grid_;
    if (key(ImGuiKey_Delete) || key(ImGuiKey_Backspace)) {
        if (d.floating) d.floating.reset(), d.commit("Delete");
        else d.clear_selection_pixels();
        changed();
    }
    if (d.floating) {
        if (key(ImGuiKey_Enter) || key(ImGuiKey_KeypadEnter)) d.put_down(), changed();
        if (key(ImGuiKey_Escape)) { // back where it was
            d.put_down();
            d.undo();
            changed();
        }
        i32 step = shift ? 8 : 1;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) d.floating->x -= step, changed();
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) d.floating->x += step, changed();
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) d.floating->y -= step, changed();
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) d.floating->y += step, changed();
        if (shift && key(ImGuiKey_H)) d.floating->image = flipped(d.floating->image, true), changed();
        if (shift && key(ImGuiKey_V)) d.floating->image = flipped(d.floating->image, false), changed();
        if (shift && key(ImGuiKey_R)) d.floating->image = turned(d.floating->image, true), changed();
    } else if (key(ImGuiKey_Escape) && d.selection.any()) {
        d.select_none();
        changed();
    }
    (void)engine;
}

// --- Panels --------------------------------------------------------------------------------------------------------

void PaintStudio::draw(Engine& engine, Rect area) {
    canvas_area_ = {area.x + tools_w, area.y + options_h, area.w - tools_w - side_w, area.h - options_h - timeline_h};
    if (fit_pending_ && canvas_) {
        fit();
        fit_pending_ = false;
    }
    options_bar(engine, {area.x, area.y, area.w, options_h});
    tools_column({area.x, area.y + options_h, tools_w, area.h - options_h});
    side_panels(engine, {area.right() - side_w, area.y + options_h, side_w, area.h - options_h});
    timeline({canvas_area_.x, canvas_area_.bottom(), canvas_area_.w, timeline_h});
    dialogs(engine);
    if (!canvas_) {
        ImGui::SetNextWindowPos({canvas_area_.center().x, canvas_area_.center().y}, 0, {0.5f, 0.5f});
        ImGui::Begin("##paint_empty", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
        ImGui::TextColored(muted, "Open a picture from the menu at the top left, or make a new one.");
        if (ImGui::Button("New picture...")) new_dialog_ = true;
        ImGui::End();
    }
    keys(engine);
    canvas_input(engine);
    refresh(engine);
}

void PaintStudio::tools_column(Rect area) {
    ImGui::SetNextWindowPos({area.x, area.y});
    ImGui::SetNextWindowSize({area.w, area.h});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {7, 8});
    ImGui::Begin("##paint_tools", nullptr, fixed | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    // Two to a row, in groups: selecting, painting, color, shapes.
    for (usize i = 0; i < std::size(tool_list); ++i) {
        const ToolInfo& t = tool_list[i];
        ImGui::PushID(static_cast<int>(t.tool));
        if (i % 2) ImGui::SameLine(0, 4);
        if (ui::tool_button("t", t.icon, tool_ == t.tool, t.tip, 36)) {
            if (canvas_ && t.tool != Tool::Move) canvas_->doc.put_down(), changed();
            if (t.tool != tool_) cancel_path();
            tool_ = t.tool;
        }
        ImGui::PopID();
        if (t.tool == Tool::Wand || t.tool == Tool::Spray || t.tool == Tool::Gradient) ImGui::Dummy({0, 4}); // groups
    }
    // The two colors: click the second to swap them.
    ImGui::Dummy({0, 6});
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto chip = [&](ImVec2 at, Pixel c) {
        dl->AddRectFilled(at, {at.x + 24, at.y + 24}, IM_COL32(60, 60, 70, 255), 4);
        dl->AddRectFilled({at.x + 2, at.y + 2}, {at.x + 22, at.y + 22}, ImGui::ColorConvertFloat4ToU32(to_imgui(c | 0xff000000)), 3);
        dl->AddRect(at, {at.x + 24, at.y + 24}, ui::color::line, 4);
    };
    p.x += 16;
    chip({p.x + 12, p.y + 12}, secondary_);
    chip({p.x, p.y}, primary_);
    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton("##swap", {38, 38});
    if (ImGui::IsItemClicked()) std::swap(primary_, secondary_), hsv_ = to_hsv(primary_);
    ui::tip("The first color (left button) and the second (right button). Click to swap", "X");
    ImGui::End();
}

void PaintStudio::options_bar(Engine& engine, Rect area) {
    ImGui::SetNextWindowPos({area.x, area.y});
    ImGui::SetNextWindowSize({area.w, area.h});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::color::bar);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10, 9});
    ImGui::Begin("##paint_options", nullptr, fixed | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
    ImGui::PushStyleColor(ImGuiCol_Border, ui::color::line);

    // The picture: a menu of every picture, with a filter and New.
    std::string title = canvas_ ? fs::path(canvas_->path).filename().string() + (unsaved() ? "  \xe2\x80\xa2" : "") : "Open a picture";
    ImGui::SetNextItemWidth(190);
    if (ImGui::BeginCombo("##picture", title.c_str(), ImGuiComboFlags_HeightLargest)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##find", "search", filter_, sizeof filter_);
        if (ImGui::Selectable("+ New picture...")) new_dialog_ = true;
        ImGui::Separator();
        for (const std::string& p : pictures_) {
            if (filter_[0] && p.find(filter_) == std::string::npos) continue;
            ImGui::PushID(p.c_str());
            if (auto t = assets_->load<gfx::Texture>(p)) {
                f32 k = 26 / std::max<f32>(1, static_cast<f32>(std::max(t->width(), t->height())));
                ImGui::Image(texture_id(*t), {t->width() * k, t->height() * k});
                ImGui::SameLine(36);
            }
            if (ImGui::Selectable(p.c_str(), canvas_ && canvas_->path == p, 0, {0, 26})) open(p);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ui::tip("Every picture in the assets; the dot means unsaved changes");
    if (canvas_) {
        ImGui::SameLine();
        ImGui::TextColored(muted, "%d x %d", canvas_->doc.width(), canvas_->doc.height());
    }

    // Menus.
    Document* d = canvas_ ? &canvas_->doc : nullptr;
    auto menu = [&](const char* name, const std::function<void()>& body) {
        ImGui::SameLine(0, 6);
        if (ImGui::Button(name)) ImGui::OpenPopup(name);
        if (ImGui::BeginPopup(name)) {
            body();
            ImGui::EndPopup();
        }
    };
    ImGui::BeginDisabled(!d);
    menu("Edit", [&] {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, d->can_undo())) d->undo(), changed();
        if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, d->can_redo())) d->redo(), changed();
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", "Ctrl+X")) clipboard_ = d->copy(), d->clear_selection_pixels(), changed();
        if (ImGui::MenuItem("Copy", "Ctrl+C")) clipboard_ = d->copy();
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, clipboard_.has_value())) d->paste(*clipboard_, {0, 0}), tool_ = Tool::Move, changed();
        if (ImGui::MenuItem("Delete", "Delete")) d->clear_selection_pixels(), changed();
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S")) save();
    });
    menu("Select", [&] {
        if (ImGui::MenuItem("All", "Ctrl+A")) d->select_all(), changed();
        if (ImGui::MenuItem("None", "Ctrl+D")) d->select_none(), changed();
        if (ImGui::MenuItem("Invert", "Ctrl+Shift+I")) d->invert_selection(), changed();
        ImGui::Separator();
        if (ImGui::MenuItem("Everything painted on this layer")) {
            Mask m(d->width(), d->height());
            for (i32 y = 0; y < d->height(); ++y)
                for (i32 x = 0; x < d->width(); ++x) m.set(x, y, alpha(d->layer().image.get(x, y)) > 0);
            d->select_mask(m, Mask::Mode::Replace);
            changed();
        }
        if (ImGui::MenuItem("Crop to the selection", nullptr, false, d->selection.any())) d->crop(d->selection.bounds()), changed();
    });
    menu("Image", [&] {
        if (ImGui::MenuItem("Canvas size...")) size_dialog_ = true, size_w_ = d->width(), size_h_ = d->height();
        if (ImGui::MenuItem("Scale...")) scale_dialog_ = true, scale_w_ = d->width(), scale_h_ = d->height();
        if (ImGui::MenuItem("Trim to what's painted")) d->trim(), changed();
        ImGui::Separator();
        if (ImGui::MenuItem("Flip across")) d->flip(true), changed();
        if (ImGui::MenuItem("Flip upside down")) d->flip(false), changed();
        ImGui::Separator();
        ImGui::MenuItem("Tiled mode", nullptr, &tiled_);
        ui::tip("The picture repeated around itself, and strokes wrapping round its edges: for tiles that join up");
    });
    menu("Frame", [&] {
        ImGui::TextColored(muted, "Frame %zu of %zu", d->frame() + 1, d->frame_count());
        if (ImGui::MenuItem("New frame")) d->add_frame(false), changed();
        if (ImGui::MenuItem("Duplicate frame")) d->add_frame(true), changed();
        if (ImGui::MenuItem("Delete frame", nullptr, false, d->frame_count() > 1)) d->remove_frame(), changed();
        ImGui::Separator();
        if (ImGui::MenuItem("Previous", ",", false, d->frame_count() > 1)) d->show_frame((d->frame() + d->frame_count() - 1) % d->frame_count()), changed();
        if (ImGui::MenuItem("Next", ".", false, d->frame_count() > 1)) d->show_frame((d->frame() + 1) % d->frame_count()), changed();
        if (ImGui::MenuItem("Move earlier", nullptr, false, d->frame() > 0)) d->move_frame(-1), changed();
        if (ImGui::MenuItem("Move later", nullptr, false, d->frame() + 1 < d->frame_count())) d->move_frame(1), changed();
        ImGui::Separator();
        ImGui::MenuItem("Onion skin", nullptr, &onion_);
        if (ImGui::MenuItem("Tags...")) tags_dialog_ = true;
    });
    menu("Adjust", [&] {
        ImGui::TextColored(muted, "On the selection, or the whole layer");
        if (ImGui::MenuItem("Hue, saturation, brightness...")) {
            adjust_dialog_ = true;
            adjust_base_ = d->layer().image;
            adj_hue_ = adj_sat_ = adj_bright_ = 0;
        }
        if (ImGui::MenuItem("Replace the first color with the second...")) replace_dialog_ = true;
        ImGui::Separator();
        auto apply = [&](const char* label, const std::function<void(Image&, const Mask&)>& f) {
            if (!ImGui::MenuItem(label)) return;
            f(d->layer().image, d->selection);
            d->commit(label);
            changed();
        };
        apply("Lighter", [](Image& i, const Mask& m) { adjust(i, m, 0, 0, 0.08f); });
        apply("Darker", [](Image& i, const Mask& m) { adjust(i, m, 0, 0, -0.08f); });
        apply("Outline in the first color", [&](Image& i, const Mask& m) { outline(i, m, primary_, false); });
        apply("Outline with corners", [&](Image& i, const Mask& m) { outline(i, m, primary_, true); });
        apply("Make solid", [](Image& i, const Mask& m) { opaque(i, m); });
    });
    ImGui::EndDisabled();

    // The tool's settings.
    ImGui::SameLine(0, 24);
    ImGui::TextColored(muted, "|");
    auto slider = [&](const char* label, i32& v, i32 lo, i32 hi, const char* tip, float w = 90) {
        ImGui::SameLine(0, 12);
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(w);
        ImGui::SliderInt((std::string("##") + label).c_str(), &v, lo, hi);
        ui::tip(tip);
    };
    auto check = [&](const char* label, bool& v, const char* tip) {
        ImGui::SameLine(0, 12);
        ImGui::Checkbox(label, &v);
        ui::tip(tip);
    };
    bool outline = (tool_ == Tool::Box || tool_ == Tool::Ellipse || tool_ == Tool::Polygon) && !filled_;
    bool stamped = custom_brush_ && (tool_ == Tool::Pencil || tool_ == Tool::Eraser);
    bool brushy = tool_ == Tool::Pencil || tool_ == Tool::Eraser || tool_ == Tool::Dither || tool_ == Tool::Shade || tool_ == Tool::Blur ||
                  tool_ == Tool::Line || tool_ == Tool::Curve || outline;
    if (stamped) {
        ImGui::SameLine(0, 12);
        ImGui::TextColored(muted, "Brush: the %d x %d selection", custom_brush_->width(), custom_brush_->height());
        ImGui::SameLine();
        if (ImGui::SmallButton("Round brush")) custom_brush_.reset();
        ui::tip("Back to the round (or square) brush", "Ctrl+Shift+B");
    } else if (brushy || tool_ == Tool::Spray) {
        slider(tool_ == Tool::Spray ? "Radius" : "Size", brush_.size, 1, 64, "Brush size. [ and ] change it", 70);
        if (tool_ != Tool::Spray) {
            ImGui::SameLine(0, 6);
            if (ui::tool_button("shape", brush_.square ? ui::Icon::Rect : ui::Icon::Circle, false,
                                brush_.square ? "A square brush: click for round" : "A round brush: click for square", 26))
                brush_.square = !brush_.square;
        }
        bool freehand = tool_ == Tool::Pencil || tool_ == Tool::Eraser || tool_ == Tool::Dither || tool_ == Tool::Shade;
        if (freehand) {
            if (tool_ != Tool::Dither) slider("Density", brush_.density, 1, 16, "A 4 x 4 dither: 16 paints every pixel, 8 every other", 70);
            ImGui::SameLine(0, 10);
            ImGui::TextUnformatted("Mirror");
            ImGui::SameLine(0, 4);
            if (ui::tool_button("mx", ui::Icon::AlignCenterX, brush_.mirror_x, "Also paint mirrored left and right, about the middle", 26))
                brush_.mirror_x = !brush_.mirror_x;
            ImGui::SameLine(0, 2);
            if (ui::tool_button("my", ui::Icon::AlignCenterY, brush_.mirror_y, "Also paint mirrored top and bottom", 26))
                brush_.mirror_y = !brush_.mirror_y;
        }
        if (tool_ == Tool::Pencil || tool_ == Tool::Eraser || tool_ == Tool::Dither)
            check("Pixel perfect", pixel_perfect_, "Takes out the L-shaped corners of one-pixel strokes");
    }
    if (tool_ == Tool::Spray) slider("Flow", flow_, 1, 40, "Pixels sprayed each frame it's held", 70);
    if (tool_ == Tool::Pencil || tool_ == Tool::Dither || tool_ == Tool::Spray || tool_ == Tool::Shade || tool_ == Tool::Line ||
        tool_ == Tool::Curve)
        check("Lock alpha", lock_alpha_, "Only paints over what's painted already: shade inside a shape without going over its edge");
    if (tool_ == Tool::Fill || tool_ == Tool::Wand || tool_ == Tool::Recolor) {
        slider("Tolerance", tolerance_, 0, 255, "How different a color can be and still count as the same", 90);
        if (tool_ != Tool::Recolor) check("Contiguous", contiguous_, "Only the connected area; off: that color everywhere");
        if (tool_ != Tool::Recolor) check("All layers", sample_all_, "Looks at the picture as you see it, not only this layer");
    }
    if (tool_ == Tool::Recolor) {
        check("Every layer", recolor_layers_, "Recolors it on every (unlocked) layer, not only this one");
        check("Every frame", recolor_frames_, "In every frame of the animation (or hold Shift)");
    }
    if (tool_ == Tool::Blur) check("Jumble", jumble_, "Scatters pixels among their neighbors instead of softening them");
    if (tool_ == Tool::Box || tool_ == Tool::Ellipse || tool_ == Tool::Polygon) check("Filled", filled_, "Filled, or an outline");
    if (tool_ == Tool::Box) {
        check("Iso diamond", diamond_, "A 2:1 floor diamond in the box. Shift keeps it 2:1");
        if (!diamond_) slider("Corners", corner_, 0, 16, "Rounded corners, so many pixels round", 70);
    }
    if (tool_ == Tool::Select) {
        ImGui::SameLine(0, 12);
        if (ui::tool_button("box", ui::Icon::Rect, !select_ellipse_, "Select a box", 26)) select_ellipse_ = false;
        ImGui::SameLine(0, 2);
        if (ui::tool_button("ellipse", ui::Icon::Circle, select_ellipse_, "Select an ellipse", 26)) select_ellipse_ = true;
    }
    if (tool_ == Tool::Text) {
        ImGui::SameLine(0, 12);
        ImGui::SetNextItemWidth(220);
        ImGui::InputTextWithHint("##text", "What it says", text_, sizeof text_);
        ui::tip("Letters, digits and punctuation in a 3 x 5 pixel font (lower case comes out as capitals)");
        slider("Scale", text_scale_, 1, 6, "Each font pixel as so many pixels square", 60);
        ImGui::SameLine(0, 12);
        ImGui::TextColored(muted, "Click where it goes, then drag it");
    }
    if (tool_ == Tool::Curve) {
        ImGui::SameLine(0, 12);
        ImGui::TextColored(muted, curve_bend_ ? "Move to bend it, click to put it down, Esc cancels" : "Drag the line, then bend it");
    }
    if (tool_ == Tool::Polygon || tool_ == Tool::Lasso) {
        ImGui::SameLine(0, 12);
        ImGui::TextColored(muted, path_clicking_ ? "Click the next corner; Enter, a double-click or the first corner closes it; Esc cancels"
                                                 : "Drag a freehand shape, or click corner by corner");
    }
    if (tool_ == Tool::Move && d) {
        ImGui::SameLine(0, 12);
        if (ImGui::Button("Flip across")) {
            if (!d->floating) d->lift();
            d->floating->image = flipped(d->floating->image, true), changed();
        }
        ui::tip("", "Shift+H");
        ImGui::SameLine();
        if (ImGui::Button("Flip down")) {
            if (!d->floating) d->lift();
            d->floating->image = flipped(d->floating->image, false), changed();
        }
        ui::tip("", "Shift+V");
        ImGui::SameLine();
        if (ImGui::Button("Turn")) {
            if (!d->floating) d->lift();
            d->floating->image = turned(d->floating->image, true), changed();
        }
        ui::tip("A quarter turn clockwise", "Shift+R");
        if (d->floating) {
            ImGui::SameLine();
            if (ImGui::Button("Put down")) d->put_down(), changed();
            ui::tip("", "Enter");
        }
    }
    if (tool_ == Tool::Select || tool_ == Tool::Wand) {
        ImGui::SameLine(0, 12);
        ImGui::TextColored(muted, "Shift adds, Alt takes away");
    }
    if (tool_ == Tool::Shade) {
        ImGui::SameLine(0, 12);
        ImGui::TextColored(muted, kept_.empty() ? "Light leans warm, shadow violet" : "Steps along your swatches when it's one of them");
    }

    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::End();

    // The view, in the canvas's bottom right corner.
    ImGui::SetNextWindowPos({canvas_area_.right() - 12, canvas_area_.bottom() - 12}, 0, {1, 1});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8);
    ImGui::Begin("##paint_view", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleVar();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(muted, "%.0f%%", zoom_ * 100);
    ImGui::SameLine();
    if (ImGui::SmallButton("Fit")) fit();
    ui::tip("Fit the picture in the window", "0");
    ImGui::SameLine();
    if (ImGui::SmallButton("1:1")) zoom_ = 1, pan_ = {};
    ui::tip("One screen pixel per picture pixel", "1");
    ImGui::SameLine();
    if (ui::tool_button("grid", ui::Icon::Grid, grid_, "The pixel grid|K", 24)) grid_ = !grid_;
    ImGui::SameLine(0, 2);
    if (ui::tool_button("tiles", ui::Icon::Tiles, tiled_, "Tiled mode: the picture repeated around itself, strokes wrapping round its edges", 24))
        tiled_ = !tiled_;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(84);
    if (ImGui::BeginCombo("##tilegrid", tile_grid_ ? std::format("{} px", tile_grid_).c_str() : "No tiles")) {
        for (i32 n : {0, 8, 16, 24, 32, 48, 64})
            if (ImGui::Selectable(n ? std::format("Every {} px", n).c_str() : "No tile grid", tile_grid_ == n)) tile_grid_ = n;
        ImGui::EndCombo();
    }
    ui::tip("A brighter grid every so many pixels: tiles, or frames in a sheet");
    ImGui::End();
    (void)engine;
}

void PaintStudio::color_panel() {
    // A saturation-brightness square, a hue strip, an alpha strip, and the hex.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    f32 w = ImGui::GetContentRegionAvail().x, sq = w - 34;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImU32 hue = ImGui::ColorConvertFloat4ToU32(to_imgui(from_hsv({hsv_.h, 1, 1})));
    dl->AddRectFilledMultiColor(p, {p.x + sq, p.y + sq}, IM_COL32_WHITE, hue, hue, IM_COL32_WHITE);
    dl->AddRectFilledMultiColor(p, {p.x + sq, p.y + sq}, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32_BLACK, IM_COL32_BLACK);
    ImGui::InvisibleButton("##sv", {sq, sq});
    if (ImGui::IsItemActive()) {
        hsv_.s = std::clamp((ImGui::GetIO().MousePos.x - p.x) / sq, 0.0f, 1.0f);
        hsv_.v = 1 - std::clamp((ImGui::GetIO().MousePos.y - p.y) / sq, 0.0f, 1.0f);
        primary_ = from_hsv(hsv_, alpha(primary_));
    }
    ImVec2 knob{p.x + hsv_.s * sq, p.y + (1 - hsv_.v) * sq};
    dl->AddCircle(knob, 6, IM_COL32_BLACK, 0, 3);
    dl->AddCircle(knob, 6, IM_COL32_WHITE, 0, 1.5f);
    // Hue, top to bottom.
    ImGui::SameLine(0, 8);
    ImVec2 hp = ImGui::GetCursorScreenPos();
    for (int k = 0; k < 6; ++k) {
        ImU32 a = ImGui::ColorConvertFloat4ToU32(to_imgui(from_hsv({k * 60.0f, 1, 1})));
        ImU32 b = ImGui::ColorConvertFloat4ToU32(to_imgui(from_hsv({(k + 1) * 60.0f, 1, 1})));
        dl->AddRectFilledMultiColor({hp.x, hp.y + sq * k / 6}, {hp.x + 22, hp.y + sq * (k + 1) / 6}, a, a, b, b);
    }
    ImGui::InvisibleButton("##hue", {22, sq});
    if (ImGui::IsItemActive()) {
        hsv_.h = std::clamp((ImGui::GetIO().MousePos.y - hp.y) / sq, 0.0f, 0.9999f) * 360;
        primary_ = from_hsv(hsv_, alpha(primary_));
    }
    f32 hy = hp.y + hsv_.h / 360 * sq;
    dl->AddRect({hp.x - 2, hy - 2}, {hp.x + 24, hy + 2}, IM_COL32_WHITE, 1, 0, 1.5f);
    // Alpha.
    ImVec2 ap = ImGui::GetCursorScreenPos();
    ImU32 solid = ImGui::ColorConvertFloat4ToU32(to_imgui(primary_ | 0xff000000));
    dl->AddRectFilledMultiColor(ap, {ap.x + w, ap.y + 12}, solid & 0x00ffffff, solid, solid, solid & 0x00ffffff);
    ImGui::InvisibleButton("##alpha", {w, 12});
    if (ImGui::IsItemActive()) {
        u8 a = static_cast<u8>(std::clamp((ImGui::GetIO().MousePos.x - ap.x) / w, 0.0f, 1.0f) * 255);
        primary_ = (primary_ & 0xffffff) | static_cast<Pixel>(a) << 24;
    }
    f32 ax = ap.x + alpha(primary_) / 255.0f * w;
    dl->AddRect({ax - 2, ap.y - 2}, {ax + 2, ap.y + 14}, IM_COL32_WHITE, 1, 0, 1.5f);
    ui::tip("See-through");
    // Hex.
    char hex[16];
    std::snprintf(hex, sizeof hex, "%02x%02x%02x%02x", red(primary_), green(primary_), blue(primary_), alpha(primary_));
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputText("##hex", hex, sizeof hex, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
        u32 v = static_cast<u32>(std::strtoul(hex, nullptr, 16));
        if (std::strlen(hex) <= 6) v = v << 8 | 0xff;
        primary_ = rgba(static_cast<u8>(v >> 24), static_cast<u8>(v >> 16), static_cast<u8>(v >> 8), static_cast<u8>(v));
        hsv_ = to_hsv(primary_);
    }
    ui::tip("RRGGBBAA (or RRGGBB). Enter takes it");
    ImGui::SameLine();
    if (ImGui::Button("Keep")) {
        if (std::find(kept_.begin(), kept_.end(), primary_) == kept_.end()) kept_.push_back(primary_);
        std::string s;
        for (Pixel k : kept_) s += std::format("{:08x} ", k);
        engine_->settings().set("editor.paint.swatches", s);
    }
    ui::tip("Adds the first color to your swatches (kept between sessions; Shade steps along them)");
}

void PaintStudio::swatches_panel() {
    auto row = [&](const char* id, const std::vector<Pixel>& list, bool removable) {
        f32 avail = ImGui::GetContentRegionAvail().x;
        i32 per = std::max(1, static_cast<i32>(avail / 24));
        for (usize i = 0; i < list.size(); ++i) {
            ImGui::PushID(id);
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::ColorButton("##s", to_imgui(list[i]), ImGuiColorEditFlags_AlphaPreview | ImGuiColorEditFlags_NoTooltip, {20, 20}))
                primary_ = list[i], hsv_ = to_hsv(primary_);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                if (removable && ImGui::GetIO().KeyAlt) {
                    kept_.erase(kept_.begin() + static_cast<std::ptrdiff_t>(i));
                    ImGui::PopID();
                    ImGui::PopID();
                    break;
                }
                secondary_ = list[i];
            }
            ui::tip(std::format("#{:02x}{:02x}{:02x}  left: first color, right: second{}", red(list[i]), green(list[i]), blue(list[i]),
                                removable ? ", Alt+right: remove" : ""));
            ImGui::PopID();
            ImGui::PopID();
            if ((static_cast<i32>(i) + 1) % per != 0 && i + 1 < list.size()) ImGui::SameLine(0, 4);
        }
    };
    ImGui::TextColored(muted, "In this picture");
    if (canvas_) row("pic", colors_of(canvas_->doc.composite(), 48), false);
    ImGui::TextColored(muted, "Yours");
    if (kept_.empty()) ImGui::TextColored(muted, "Keep colors from the color panel.");
    else row("kept", kept_, true);
    // A ramp between the two colors, for shading (Aseprite's palette gradient).
    if (ImGui::SmallButton("Ramp")) {
        for (Pixel c : ramp(primary_, secondary_, ramp_steps_))
            if (std::find(kept_.begin(), kept_.end(), c) == kept_.end()) kept_.push_back(c);
        std::string str;
        for (Pixel k : kept_) str += std::format("{:08x} ", k);
        engine_->settings().set("editor.paint.swatches", str);
    }
    ui::tip("Adds colors stepping from the first color to the second (the hue the short way round) to your swatches");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::SliderInt("##rampn", &ramp_steps_, 3, 12, "%d colors");
}

void PaintStudio::layers_panel() {
    if (!canvas_) return;
    Document& d = canvas_->doc;
    // Top layer first, as in every painting program.
    for (usize n = d.layers.size(); n-- > 0;) {
        Layer& l = d.layers[n];
        ImGui::PushID(static_cast<int>(n));
        bool on = d.active == n;
        if (ui::tool_button("eye", ui::Icon::Eye, l.visible, l.visible ? "Shown: click to hide" : "Hidden: click to show", 24)) {
            l.visible = !l.visible;
            d.commit(l.visible ? "Show layer" : "Hide layer");
            changed();
        }
        ImGui::SameLine(0, 2);
        if (ui::tool_button("lock", ui::Icon::Lock, l.locked, l.locked ? "Locked: click to unlock" : "Lock it (nothing paints on it)", 24)) {
            l.locked = !l.locked;
            d.commit(l.locked ? "Lock layer" : "Unlock layer");
        }
        ImGui::SameLine(0, 6);
        if (on) {
            char name[64];
            std::snprintf(name, sizeof name, "%s", l.name.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##name", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue) && name[0]) {
                l.name = name;
                d.commit("Rename layer");
            }
        } else if (ImGui::Selectable(l.name.c_str(), false, 0, {0, 24})) {
            d.put_down();
            d.active = n;
            changed();
        }
        ImGui::PopID();
    }
    Layer& l = d.layer();
    f32 opacity = l.opacity * 100;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##opacity", &opacity, 0, 100, "Opacity %.0f%%")) l.opacity = opacity / 100, changed();
    if (ImGui::IsItemDeactivatedAfterEdit()) d.commit("Layer opacity");
    // Buttons.
    if (ui::tool_button("new", ui::Icon::Plus, false, "A new layer above this one", 26)) d.add_layer(std::format("Layer {}", d.layers.size() + 1)), changed();
    ImGui::SameLine(0, 2);
    if (ui::tool_button("dup", ui::Icon::Copy, false, "Duplicate the layer", 26)) d.duplicate_layer(), changed();
    ImGui::SameLine(0, 2);
    if (ui::tool_button("up", ui::Icon::ArrowUp, false, "Move it up", 26, d.active + 1 < d.layers.size())) d.move_layer(1), changed();
    ImGui::SameLine(0, 2);
    if (ui::tool_button("down", ui::Icon::ArrowDown, false, "Move it down", 26, d.active > 0)) d.move_layer(-1), changed();
    ImGui::SameLine(0, 2);
    if (ui::tool_button("merge", ui::Icon::AlignBottom, false, "Merge into the layer below", 26, d.active > 0)) d.merge_down(), changed();
    ImGui::SameLine(0, 2);
    if (ui::tool_button("flatten", ui::Icon::Group, false, "Flatten every layer into one", 26, d.layers.size() > 1)) d.flatten(), changed();
    ImGui::SameLine(0, 2);
    if (ui::tool_button("delete", ui::Icon::Trash, false, "Delete the layer", 26, d.layers.size() > 1)) d.remove_layer(), changed();
}

void PaintStudio::history_panel() {
    if (!canvas_) return;
    Document& d = canvas_->doc;
    std::vector<std::string> steps = d.history();
    ImGui::BeginChild("##steps", {0, std::min(220.0f, 26.0f * static_cast<f32>(steps.size()) + 8)});
    for (usize i = 0; i < steps.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        bool now = i == d.history_now(), later = i > d.history_now();
        if (later) ImGui::PushStyleColor(ImGuiCol_Text, muted);
        if (ImGui::Selectable(steps[i].c_str(), now)) {
            d.go_to(i);
            changed();
        }
        if (later) ImGui::PopStyleColor();
        if (now && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ui::tip("Click a step to go back to it (or forward again). Undo and redo walk it too");
}

// Tags get colors of their own, as Aseprite gives them.
static ImU32 tag_color(usize i) {
    static constexpr ImU32 colors[] = {IM_COL32(90, 170, 230, 255), IM_COL32(230, 150, 70, 255), IM_COL32(120, 200, 110, 255),
                                       IM_COL32(210, 110, 190, 255), IM_COL32(230, 210, 90, 255), IM_COL32(150, 130, 230, 255)};
    return colors[i % std::size(colors)];
}

void PaintStudio::timeline(Rect area) {
    ImGui::SetNextWindowPos({area.x, area.y});
    ImGui::SetNextWindowSize({area.w, area.h});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::color::bar);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8, 6});
    ImGui::Begin("##paint_timeline", nullptr, fixed | ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (!canvas_) {
        ImGui::End();
        return;
    }
    Document& d = canvas_->doc;
    // The buttons: a new frame, a copy, delete, move, onion skin, tags, and this frame's time.
    if (ui::tool_button("new", ui::Icon::Plus, false, "A new, empty frame after this one", 26)) d.add_frame(false), changed();
    ImGui::SameLine(0, 2);
    if (ui::tool_button("dup", ui::Icon::Copy, false, "A copy of this frame after it", 26)) d.add_frame(true), changed();
    ImGui::SameLine(0, 2);
    if (ui::tool_button("del", ui::Icon::Trash, false, "Delete this frame", 26, d.frame_count() > 1)) d.remove_frame(), changed();
    ImGui::SameLine(0, 2);
    if (ui::tool_button("earlier", ui::Icon::AlignLeft, false, "Move it earlier", 26, d.frame() > 0)) d.move_frame(-1), changed();
    ImGui::SameLine(0, 2);
    if (ui::tool_button("later", ui::Icon::AlignRight, false, "Move it later", 26, d.frame() + 1 < d.frame_count())) d.move_frame(1), changed();
    ImGui::SameLine(0, 10);
    if (ui::tool_button("onion", ui::Icon::Onion, onion_, "Onion skin: the frames before (red) and after (blue), faint, behind this one", 26))
        onion_ = !onion_, changed();
    ImGui::SameLine(0, 2);
    if (ui::tool_button("tags", ui::Icon::Frames, false, "Tags: name runs of frames as animations (walk_down, idle...) for the game", 26))
        tags_dialog_ = true;
    ImGui::SameLine(0, 10);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(muted, "Frame %zu of %zu", d.frame() + 1, d.frame_count());
    ImGui::SameLine(0, 10);
    i32 ms = d.duration(d.frame());
    ImGui::SetNextItemWidth(90);
    if (ImGui::InputInt("ms##duration", &ms, 10, 50, ImGuiInputTextFlags_EnterReturnsTrue)) d.set_duration(d.frame(), ms);
    ui::tip("How long this frame shows, in milliseconds. Shift+click a frame's number below to give every frame this time");
    ImGui::SameLine(0, 10);
    ImGui::TextColored(muted, ", and . step through the frames");

    // The frames, left to right, with their tags above them.
    const f32 cell = 46, gap = 4;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Tags that share frames (walk_down over frames 1 to 4, idle_down on frame 1) go in lanes, one above the other.
    std::vector<std::vector<std::pair<usize, usize>>> lanes;
    for (usize ti = 0; ti < d.tags.size(); ++ti) {
        const Tag& t = d.tags[ti];
        usize lane = 0;
        while (lane < lanes.size() && std::any_of(lanes[lane].begin(), lanes[lane].end(), [&](auto r) { return t.from <= r.second && r.first <= t.to; }))
            ++lane;
        if (lane == lanes.size()) lanes.emplace_back();
        lanes[lane].push_back({t.from, t.to});
        f32 y = origin.y + static_cast<f32>(lane) * 16;
        ImVec2 a{origin.x + static_cast<f32>(t.from) * (cell + gap), y}, b{origin.x + static_cast<f32>(t.to + 1) * (cell + gap) - gap, y + 14};
        dl->AddRectFilled(a, b, tag_color(ti), 3);
        dl->PushClipRect(a, b, true);
        dl->AddText(ImGui::GetFont(), 13, {a.x + 4, a.y}, IM_COL32(20, 22, 30, 255), t.name.c_str());
        dl->PopClipRect();
    }
    ImGui::Dummy({0, 16 * static_cast<f32>(std::max<usize>(1, lanes.size()))});
    for (usize f = 0; f < d.frame_count(); ++f) {
        ImGui::PushID(static_cast<int>(f));
        if (f) ImGui::SameLine(0, gap);
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool on = f == d.frame();
        if (ImGui::InvisibleButton("##frame", {cell, cell})) {
            if (ImGui::GetIO().KeyShift && f != d.frame()) { // the shown frame's time for every frame
                i32 t = d.duration(d.frame());
                for (usize k = 0; k < d.frame_count(); ++k)
                    if (d.duration(k) != t) d.set_duration(k, t);
            } else {
                d.show_frame(f);
                changed();
            }
        }
        bool hot = ImGui::IsItemHovered();
        ui::tip(std::format("Frame {}: {} ms", f + 1, d.duration(f)));
        dl->AddRectFilled(p, {p.x + cell, p.y + cell}, IM_COL32(42, 46, 58, 255), 4);
        if (f < thumbs_.size() && thumbs_[f]) {
            const gfx::Texture& t = *thumbs_[f];
            f32 k = (cell - 8) / static_cast<f32>(std::max(t.width(), t.height()));
            f32 w = static_cast<f32>(t.width()) * k, h = static_cast<f32>(t.height()) * k;
            ImVec2 a{p.x + (cell - w) * 0.5f, p.y + (cell - h) * 0.5f};
            dl->AddImage(texture_id(t), a, {a.x + w, a.y + h});
        }
        dl->AddRect(p, {p.x + cell, p.y + cell}, on ? ui::color::accent : hot ? ui::color::text : ui::color::line, 4, 0, on ? 2.0f : 1.0f);
        std::string n = std::to_string(f + 1);
        dl->AddText(ImGui::GetFont(), 12, {p.x + 4, p.y + cell - 15}, on ? ui::color::accent : ui::color::muted, n.c_str());
        ImGui::PopID();
    }
    ImGui::End();
}

void PaintStudio::preview_panel() {
    if (!canvas_) return;
    Document& d = canvas_->doc;
    usize n = d.frame_count();
    // Loops through the frames at their own times while you paint (Piskel's preview).
    if (playing_ && n > 1) {
        play_time_ += ImGui::GetIO().DeltaTime * 1000;
        while (play_time_ >= static_cast<f32>(d.duration(play_frame_ % n))) {
            play_time_ -= static_cast<f32>(d.duration(play_frame_ % n));
            play_frame_ = (play_frame_ + 1) % n;
        }
    } else if (!playing_ || n == 1) {
        play_frame_ = d.frame();
    }
    play_frame_ %= n;
    f32 avail = ImGui::GetContentRegionAvail().x;
    if (play_frame_ < thumbs_.size() && thumbs_[play_frame_]) {
        const gfx::Texture& t = *thumbs_[play_frame_];
        f32 dw = static_cast<f32>(d.width()), dh = static_cast<f32>(d.height());
        f32 fit = std::min(avail, 200.0f) / std::max(dw, dh);
        f32 k = preview_zoom_ > 0 ? static_cast<f32>(preview_zoom_) : (fit >= 1 ? std::floor(fit) : fit);
        f32 w = dw * k, h = dh * k;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p, {p.x + avail, p.y + h + 8}, IM_COL32(36, 40, 50, 255), 4);
        ImGui::SetCursorScreenPos({p.x + (avail - w) * 0.5f, p.y + 4});
        ImGui::Image(texture_id(t), {w, h});
        ImGui::SetCursorScreenPos({p.x, p.y + h + 12});
    }
    if (n > 1) {
        if (ui::tool_button("play", playing_ ? ui::Icon::Frames : ui::Icon::Play, playing_, playing_ ? "Pause the preview" : "Play the preview", 24))
            playing_ = !playing_;
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(muted, "Frame %zu", play_frame_ + 1);
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(110);
    ImGui::SliderInt("##pzoom", &preview_zoom_, 0, 8, preview_zoom_ ? "%dx" : "Fit");
    ui::tip("The preview's size");
}

void PaintStudio::side_panels(Engine& engine, Rect area) {
    ImGui::SetNextWindowPos({area.x, area.y});
    ImGui::SetNextWindowSize({area.w, area.h});
    ImGui::Begin("##paint_side", nullptr, fixed);
    auto section = [](const char* name) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        return ImGui::CollapsingHeader(name);
    };
    if (section("Color")) color_panel();
    if (section("Swatches")) swatches_panel();
    if (canvas_ && section("Preview")) preview_panel();
    if (canvas_ && section("Layers")) layers_panel();
    if (canvas_ && section("History")) history_panel();
    ImGui::End();
    (void)engine;
}

void PaintStudio::dialogs(Engine& engine) {
    auto open_popup = [](bool& flag, const char* name) {
        if (flag) ImGui::OpenPopup(name), flag = false;
    };
    open_popup(new_dialog_, "New picture");
    open_popup(size_dialog_, "Canvas size");
    open_popup(adjust_dialog_, "Hue, saturation, brightness");
    open_popup(replace_dialog_, "Replace a color");
    open_popup(scale_dialog_, "Scale");
    if (tags_dialog_ && canvas_) tags_before_ = canvas_->doc.tags;
    open_popup(tags_dialog_, "Tags");

    if (ImGui::BeginPopupModal("Scale", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        Document& d = canvas_->doc;
        i32 w = scale_w_, h = scale_h_;
        ImGui::SetNextItemWidth(150);
        ImGui::InputInt("Width", &scale_w_);
        ImGui::SetNextItemWidth(150);
        ImGui::InputInt("Height", &scale_h_);
        ImGui::Checkbox("Keep the shape", &scale_keep_);
        if (scale_keep_ && scale_w_ != w) scale_h_ = std::max(1, static_cast<i32>(std::lround(static_cast<f64>(scale_w_) * d.height() / d.width())));
        else if (scale_keep_ && scale_h_ != h) scale_w_ = std::max(1, static_cast<i32>(std::lround(static_cast<f64>(scale_h_) * d.width() / d.height())));
        scale_w_ = std::clamp(scale_w_, 1, 4096), scale_h_ = std::clamp(scale_h_, 1, 4096);
        for (auto [label, k] : {std::pair{"x2", 2.0}, {"x3", 3.0}, {"x4", 4.0}, {"Half", 0.5}}) {
            if (ImGui::SmallButton(label)) scale_w_ = std::max(1, static_cast<i32>(d.width() * k)), scale_h_ = std::max(1, static_cast<i32>(d.height() * k));
            ImGui::SameLine();
        }
        ImGui::NewLine();
        ui::hint("Without smoothing: every pixel stays a crisp block. Every layer and frame is scaled.");
        if (ImGui::Button("Scale")) {
            d.scale(scale_w_, scale_h_);
            changed();
            fit_pending_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Tags", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        Document& d = canvas_->doc;
        ui::hint("A tag names a run of frames: an animation the game plays by name (walk_down, idle_right...).");
        i32 last = static_cast<i32>(d.frame_count());
        std::optional<usize> remove;
        for (usize i = 0; i < d.tags.size(); ++i) {
            Tag& t = d.tags[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::ColorButton("##c", ImGui::ColorConvertU32ToFloat4(tag_color(i)), ImGuiColorEditFlags_NoTooltip, {14, 22});
            ImGui::SameLine();
            char name[64];
            std::snprintf(name, sizeof name, "%s", t.name.c_str());
            ImGui::SetNextItemWidth(140);
            if (ImGui::InputText("##name", name, sizeof name)) t.name = name;
            ImGui::SameLine();
            i32 from = static_cast<i32>(t.from) + 1, to = static_cast<i32>(t.to) + 1;
            ImGui::SetNextItemWidth(110);
            if (ImGui::DragIntRange2("##frames", &from, &to, 0.1f, 1, last, "from %d", "to %d")) {
                t.from = static_cast<usize>(std::clamp(from, 1, last) - 1);
                t.to = static_cast<usize>(std::clamp(to, from, last) - 1);
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            if (ImGui::BeginCombo("##dir", t.direction.c_str())) {
                for (const char* dir : {"forward", "reverse", "pingpong"})
                    if (ImGui::Selectable(dir, t.direction == dir)) t.direction = dir;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ui::tool_button("x", ui::Icon::Trash, false, "Remove the tag", 24)) remove = i;
            ImGui::PopID();
        }
        if (remove) d.tags.erase(d.tags.begin() + static_cast<std::ptrdiff_t>(*remove));
        if (d.tags.empty()) ImGui::TextColored(muted, "No tags: the game plays every frame in a loop.");
        if (ImGui::Button("+ Tag this frame")) {
            d.tags.push_back({std::format("tag_{}", d.tags.size() + 1), d.frame(), d.frame()});
        }
        ImGui::SameLine();
        if (ImGui::Button("Done")) {
            std::erase_if(d.tags, [](const Tag& t) { return t.name.empty(); });
            if (d.tags != tags_before_) d.commit("Tags"); // one step for everything done here
            changed();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("New picture", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Where (from the assets folder; .png is added)");
        ImGui::SetNextItemWidth(320);
        ImGui::InputText("##name", new_name_, sizeof new_name_);
        ImGui::SetNextItemWidth(150);
        ImGui::InputInt("Width", &new_w_);
        ImGui::SetNextItemWidth(150);
        ImGui::InputInt("Height", &new_h_);
        new_w_ = std::clamp(new_w_, 1, 4096), new_h_ = std::clamp(new_h_, 1, 4096);
        for (i32 s : {16, 24, 32, 48, 64, 128}) {
            ImGui::PushID(s);
            if (ImGui::SmallButton(std::format("{0}x{0}", s).c_str())) new_w_ = new_h_ = s;
            ImGui::PopID();
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("Iso tile 32x16")) new_w_ = 32, new_h_ = 16;
        std::string path = std::string(new_name_) + ".png";
        std::error_code ec;
        bool taken = fs::exists(assets_dir_ / path, ec);
        if (taken) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::color::danger), "There's already a picture there.");
        ImGui::BeginDisabled(taken || !new_name_[0]);
        if (ImGui::Button("Make it")) {
            new_picture(path, new_w_, new_h_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Canvas size", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(150);
        ImGui::InputInt("Width", &size_w_);
        ImGui::SetNextItemWidth(150);
        ImGui::InputInt("Height", &size_h_);
        size_w_ = std::clamp(size_w_, 1, 4096), size_h_ = std::clamp(size_h_, 1, 4096);
        ImGui::TextUnformatted("What stays put");
        for (i32 i = 0; i < 9; ++i) {
            ImGui::PushID(i);
            if (i % 3) ImGui::SameLine(0, 4);
            ImGui::PushStyleColor(ImGuiCol_Button, anchor_ == i ? ui::color::on : ui::color::button);
            if (ImGui::Button(anchor_ == i ? "o" : "", {28, 28})) anchor_ = i;
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ui::hint("The bottom middle (the foot) by default, so things standing on the picture's feet don't move.");
        if (ImGui::Button("Resize")) {
            canvas_->doc.resize(size_w_, size_h_, static_cast<f32>(anchor_ % 3) * 0.5f, static_cast<f32>(anchor_ / 3) * 0.5f);
            changed();
            fit_pending_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Hue, saturation, brightness", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        Document& d = canvas_->doc;
        bool moved = ImGui::SliderFloat("Hue", &adj_hue_, -180, 180, "%.0f");
        moved |= ImGui::SliderFloat("Saturation", &adj_sat_, -1, 1, "%.2f");
        moved |= ImGui::SliderFloat("Brightness", &adj_bright_, -1, 1, "%.2f");
        if (moved && adjust_base_) { // live
            d.layer().image = *adjust_base_;
            adjust(d.layer().image, d.selection, adj_hue_, adj_sat_, adj_bright_);
            changed();
        }
        if (ImGui::Button("Apply")) {
            d.commit("Hue and saturation");
            adjust_base_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            if (adjust_base_) d.layer().image = *adjust_base_;
            adjust_base_.reset();
            changed();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Replace a color", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::ColorButton("##from", to_imgui(primary_), 0, {24, 24});
        ImGui::SameLine();
        ImGui::TextUnformatted("becomes");
        ImGui::SameLine();
        ImGui::ColorButton("##to", to_imgui(secondary_), 0, {24, 24});
        ImGui::SetNextItemWidth(200);
        ImGui::SliderInt("Tolerance", &tolerance_, 0, 255);
        if (ImGui::Button("Replace")) {
            replace(canvas_->doc.layer().image, canvas_->doc.selection, primary_, secondary_, tolerance_);
            canvas_->doc.commit("Replace color");
            changed();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    (void)engine;
}

// --- The canvas ----------------------------------------------------------------------------------------------------

void PaintStudio::render(Engine& engine) {
    if (!canvas_ || !canvas_->view) return;
    gfx::Camera2D cam = camera(engine);
    batch_.begin(cam, gfx::SpriteSort::None);
    Document& d = canvas_->doc;
    f32 w = static_cast<f32>(d.width()), h = static_cast<f32>(d.height()), px = 1.0f / zoom_;
    // A checkerboard shows what's see-through.
    f32 cell = std::max(1.0f, std::ceil(8.0f / zoom_));
    for (f32 y = 0; y < h; y += cell)
        for (f32 x = 0; x < w; x += cell) {
            bool dark = (static_cast<int>(x / cell) + static_cast<int>(y / cell)) % 2;
            batch_.draw_rect({x, y, std::min(cell, w - x), std::min(cell, h - y)}, dark ? Color::hex(0x2a2e38) : Color::hex(0x353a46));
        }
    gfx::SpriteInstance s;
    s.size = {w, h};
    s.origin = {0, 0};
    if (onion_view_) batch_.draw(*onion_view_, s); // behind: it shows where this frame is see-through
    batch_.draw(*canvas_->view, s);
    // Tiled mode: the picture around itself, a little dimmer, so seams show.
    if (tiled_)
        for (i32 ty = -1; ty <= 1; ++ty)
            for (i32 tx = -1; tx <= 1; ++tx) {
                if (!tx && !ty) continue;
                gfx::SpriteInstance t = s;
                t.position = {w * static_cast<f32>(tx), h * static_cast<f32>(ty)};
                t.color = Color::white().with_alpha(0.75f);
                batch_.draw(*canvas_->view, t);
            }
    if (grid_ && zoom_ >= 6) {
        for (f32 x = 0; x <= w; x += 1) batch_.draw_rect({x - px * 0.5f, 0, px, h}, Color::hex(0xffffff, 0.07f));
        for (f32 y = 0; y <= h; y += 1) batch_.draw_rect({0, y - px * 0.5f, w, px}, Color::hex(0xffffff, 0.07f));
    }
    // The tile grid: every so many pixels, brighter (a tile's or a frame's edge).
    if (tile_grid_ > 1) {
        for (f32 x = 0; x <= w; x += static_cast<f32>(tile_grid_)) batch_.draw_rect({x - px * 0.5f, 0, px, h}, Color::hex(0x5ad2c8, 0.45f));
        for (f32 y = 0; y <= h; y += static_cast<f32>(tile_grid_)) batch_.draw_rect({0, y - px * 0.5f, w, px}, Color::hex(0x5ad2c8, 0.45f));
    }
    batch_.draw_outline({-px, -px, w + 2 * px, h + 2 * px}, Color::hex(0x5a6070), px);
    // The selection's edge: dashes that crawl along it.
    if (d.selection.any() && !d.floating) {
        f32 t = static_cast<f32>(ImGui::GetTime() * 8);
        for (i32 y = 0; y <= d.height(); ++y)
            for (i32 x = 0; x <= d.width(); ++x) {
                bool in = d.selection.test(x, y);
                bool dash = (static_cast<i32>(static_cast<f32>(x + y) + t) / 2) % 2;
                Color c = dash ? Color::white() : Color::black();
                if (in != d.selection.test(x - 1, y)) batch_.draw_rect({static_cast<f32>(x) - px, static_cast<f32>(y), 2 * px, 1}, c);
                if (in != d.selection.test(x, y - 1)) batch_.draw_rect({static_cast<f32>(x), static_cast<f32>(y) - px, 1, 2 * px}, c);
            }
    }
    // Pixels being moved.
    if (d.floating)
        batch_.draw_outline({static_cast<f32>(d.floating->x), static_cast<f32>(d.floating->y), static_cast<f32>(d.floating->image.width()),
                             static_cast<f32>(d.floating->image.height())},
                            Color::hex(0xf2c45c), 1.5f * px);
    // A box being selected.
    if (selecting_ && stroke_.on) {
        Point a = stroke_.start, b = stroke_.last;
        batch_.draw_outline({static_cast<f32>(std::min(a.x, b.x)), static_cast<f32>(std::min(a.y, b.y)), static_cast<f32>(std::abs(b.x - a.x) + 1),
                             static_cast<f32>(std::abs(b.y - a.y) + 1)},
                            Color::hex(0xf2c45c), 1.5f * px);
    }
    // A gradient's line.
    if (tool_ == Tool::Gradient && stroke_.on) {
        Vec2 a{stroke_.start.x + 0.5f, stroke_.start.y + 0.5f}, b{stroke_.last.x + 0.5f, stroke_.last.y + 0.5f};
        i32 steps = static_cast<i32>((b - a).length() / (px * 3)) + 1;
        for (i32 i = 0; i <= steps; ++i) batch_.draw_rect(Rect::from_center(a + (b - a) * (static_cast<f32>(i) / steps), Vec2{2 * px}), Color::white());
    }
    // A lasso or polygon being drawn: its edges so far, and back to the start.
    if (!path_.empty()) {
        Point mouse = pixel_at(engine, ImGui::GetIO().MousePos);
        std::vector<Point> pts = path_;
        if (path_clicking_) pts.push_back(mouse);
        for (usize i = 0; i + 1 < pts.size(); ++i)
            line(pts[i], pts[i + 1], [&](i32 x, i32 y) { batch_.draw_rect({static_cast<f32>(x), static_cast<f32>(y), 1, 1}, Color::hex(0xf2c45c, 0.7f)); });
        if (pts.size() > 2)
            line(pts.back(), pts.front(), [&](i32 x, i32 y) { batch_.draw_rect({static_cast<f32>(x), static_cast<f32>(y), 1, 1}, Color::hex(0xffffff, 0.25f)); });
        batch_.draw_outline({static_cast<f32>(path_.front().x) - px, static_cast<f32>(path_.front().y) - px, 1 + 2 * px, 1 + 2 * px}, Color::white(), 1.5f * px);
    }
    // A curve being bent: its ends.
    if (curve_bend_)
        for (Point e : {curve_a_, curve_b_})
            batch_.draw_outline({static_cast<f32>(e.x) - px, static_cast<f32>(e.y) - px, 1 + 2 * px, 1 + 2 * px}, Color::hex(0xf2c45c), 1.5f * px);
    // Where the brush would paint.
    if (over_canvas_ && !stroke_.on && path_.empty()) {
        Point p = pixel_at(engine, ImGui::GetIO().MousePos);
        bool brushy = tool_ == Tool::Pencil || tool_ == Tool::Eraser || tool_ == Tool::Shade || tool_ == Tool::Dither || tool_ == Tool::Blur;
        if (custom_brush_ && (tool_ == Tool::Pencil || tool_ == Tool::Eraser)) {
            const Image& b = *custom_brush_;
            for (i32 by = 0; by < b.height(); ++by)
                for (i32 bx = 0; bx < b.width(); ++bx)
                    if (Pixel bp = b.get(bx, by); alpha(bp))
                        batch_.draw_rect({static_cast<f32>(p.x + bx - b.width() / 2), static_cast<f32>(p.y + by - b.height() / 2), 1, 1},
                                         Color::rgba8(red(bp), green(bp), blue(bp), 140));
        } else if (tool_ == Tool::Spray) {
            f32 r = static_cast<f32>(std::max(2, brush_.size));
            for (i32 k = 0; k < 32; ++k) {
                f32 a = static_cast<f32>(k) / 32 * 6.2832f;
                batch_.draw_rect(Rect::from_center({p.x + 0.5f + std::cos(a) * r, p.y + 0.5f + std::sin(a) * r}, Vec2{1.5f * px}), Color::white());
            }
        } else if (tool_ == Tool::Text) {
            Point size = text_size(text_, text_scale_);
            text(text_, p, text_scale_, [&](i32 x, i32 y) {
                batch_.draw_rect({static_cast<f32>(x), static_cast<f32>(y), 1, 1}, Color::rgba8(red(primary_), green(primary_), blue(primary_), 150));
            });
            batch_.draw_outline({static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(size.x), static_cast<f32>(size.y)}, Color::hex(0xffffff, 0.4f), px);
        } else if (brushy) {
            paint::Brush b = brush_;
            b.density = 16;
            b.mirror_x = b.mirror_y = false;
            dab(b, p.x, p.y, d.width(), d.height(), [&](i32 x, i32 y) {
                batch_.draw_rect({static_cast<f32>(x), static_cast<f32>(y), 1, 1}, tool_ == Tool::Eraser ? Color::hex(0xffffff, 0.25f)
                                                                                                          : Color::rgba8(red(primary_), green(primary_), blue(primary_), 128));
            });
        } else {
            batch_.draw_outline({static_cast<f32>(p.x), static_cast<f32>(p.y), 1, 1}, Color::white(), 1.5f * px);
        }
    }
    // Mirror lines.
    if (brush_.mirror_x) batch_.draw_rect({w * 0.5f - px * 0.5f, 0, px, h}, Color::hex(0x5ad2c8, 0.6f));
    if (brush_.mirror_y) batch_.draw_rect({0, h * 0.5f - px * 0.5f, w, px}, Color::hex(0x5ad2c8, 0.6f));
    engine.renderer().submit(batch_);
}

} // namespace bowl::editor
