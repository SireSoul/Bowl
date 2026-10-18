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

constexpr float options_h = 46, tools_w = 52, side_w = 300;
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
// The tools, top to bottom, with their keys.
constexpr ToolInfo tool_list[] = {
    {PaintStudio::Tool::Move, ui::Icon::Pan, "Move: lift the selection (or the layer) and drag it. Enter puts it down, Esc puts it back|V"},
    {PaintStudio::Tool::Select, ui::Icon::Region, "Select a box. Shift adds to the selection, Alt takes away|M"},
    {PaintStudio::Tool::Wand, ui::Icon::Wand, "Magic wand: select an area of one color. Shift adds, Alt takes away|W"},
    {PaintStudio::Tool::Pencil, ui::Icon::Brush, "Pencil: the left button paints the first color, the right the second. Shift+click: a line from the last|B"},
    {PaintStudio::Tool::Eraser, ui::Icon::Eraser, "Eraser|E"},
    {PaintStudio::Tool::Fill, ui::Icon::Bucket, "Fill an area of one color|G"},
    {PaintStudio::Tool::Pick, ui::Icon::Pipette, "Pick a color from the picture (or hold Alt with any tool)|I"},
    {PaintStudio::Tool::Line, ui::Icon::Line, "Line. Shift keeps it flat, upright, 45 degrees or on the 2:1 iso slope|L"},
    {PaintStudio::Tool::Box, ui::Icon::Rect, "Box (or an iso diamond). Shift: square|R"},
    {PaintStudio::Tool::Ellipse, ui::Icon::Circle, "Ellipse. Shift: circle|O"},
    {PaintStudio::Tool::Gradient, ui::Icon::Gradient, "Gradient from the first color to the second, over the selection or the layer|D"},
    {PaintStudio::Tool::Shade, ui::Icon::Shade, "Shade: the left button lightens, the right darkens, a step at a time|S"},
};

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
    Canvas c{path, file, Document(std::move(img))};
    // Its layers, if they were kept and the PNG hasn't been repainted elsewhere since.
    if (auto text = read_file_text(layers_file(path))) {
        auto doc = json::parse(*text);
        if (doc && static_cast<u64>(doc.value["png"].as_number()) == fingerprint(*bytes) % (1ull << 52))
            if (auto layered = Document::from_json(doc.value["document"])) c.doc = std::move(*layered);
    }
    canvas_ = std::move(c);
    fit_pending_ = true;
    stroke_ = {};
    return true;
}

bool PaintStudio::save() {
    if (!canvas_) return false;
    Canvas& c = *canvas_;
    c.doc.put_down();
    Image flat = c.doc.composite();
    std::span<const u8> bytes(reinterpret_cast<const u8*>(flat.pixels().data()), flat.pixels().size() * 4);
    if (!gfx::write_png(c.file, static_cast<u32>(flat.width()), static_cast<u32>(flat.height()), bytes)) {
        say_("Couldn't save " + c.path);
        return false;
    }
    // Layers are kept beside the assets (never shipped), with a fingerprint of the PNG they flatten to.
    fs::path lf = layers_file(c.path);
    std::error_code ec;
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
    std::error_code ec;
    fs::create_directories(canvas_->file.parent_path(), ec);
    canvas_->doc.commit("New picture"); // unsaved until it's saved
    fit_pending_ = true;
}

void PaintStudio::refresh(Engine& engine) {
    if (!canvas_ || !canvas_->dirty) return;
    Canvas& c = *canvas_;
    Image flat = c.doc.composite();
    c.flat.resize(flat.pixels().size() * 4);
    std::memcpy(c.flat.data(), flat.pixels().data(), c.flat.size());
    u32 w = static_cast<u32>(flat.width()), h = static_cast<u32>(flat.height());
    if (!c.view || c.view->width() != w || c.view->height() != h)
        c.view = engine.gpu().create_texture({.width = w, .height = h, .rgba8 = c.flat.data(), .filter = gfx::Filter::Nearest,
                                              .mipmaps = false, .name = "paint canvas"});
    else
        engine.gpu().update_texture(*c.view, c.flat.data(), false);
    // The game's texture too, so the level (and a running game, once saved) shows the stroke.
    if (auto tex = engine.assets().load<gfx::Texture>(c.path); tex && tex->width() == w && tex->height() == h)
        engine.gpu().update_texture(*tex, c.flat.data(), false);
    c.dirty = false;
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

void PaintStudio::paint_points(Image& img, const std::vector<Point>& pts, Pixel c) {
    Document& d = canvas_->doc;
    for (Point p : pts)
        dab(brush_, p.x, p.y, d.width(), d.height(), [&](i32 x, i32 y) {
            if (d.editable(x, y)) img.set(x, y, c);
        });
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
    if (alt && (t == Tool::Pencil || t == Tool::Fill || t == Tool::Line || t == Tool::Box || t == Tool::Ellipse || t == Tool::Gradient))
        t = Tool::Pick;
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
    default: break;
    }
    if (d.layer().locked) {
        say_("That layer is locked");
        return;
    }
    // Strokes and shapes: drawn from the layer as it was, each frame.
    Point from = p;
    bool line_from_last = shift && (t == Tool::Pencil || t == Tool::Eraser) && stroke_.last.x >= 0 && !stroke_.points.empty();
    if (line_from_last) from = stroke_.last;
    stroke_ = Stroke{.on = true, .secondary = secondary, .base = d.layer().image, .start = from, .last = p};
    stroke_.touched.assign(static_cast<usize>(d.width()) * static_cast<usize>(d.height()), 0);
    if (t == Tool::Pencil || t == Tool::Eraser || t == Tool::Shade) {
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
    if (!stroke_.on) return;
    if (tool_ == Tool::Select) {
        stroke_.last = p;
        return;
    }
    Image& img = d.layer().image;
    switch (tool_) {
    case Tool::Pencil:
    case Tool::Eraser: {
        if (!(p == stroke_.last) || stroke_.points.size() <= 1)
            line(stroke_.last, p, [&](i32 x, i32 y) {
                if (stroke_.points.empty() || !(stroke_.points.back() == Point{x, y})) stroke_.points.push_back({x, y});
            });
        stroke_.last = p;
        img = stroke_.base;
        bool perfect = pixel_perfect_ && brush_.size == 1;
        paint_points(img, perfect ? pixel_perfect(stroke_.points) : stroke_.points, tool_ == Tool::Eraser ? clear : color(stroke_.secondary));
        break;
    }
    case Tool::Shade: {
        line(stroke_.last, p, [&](i32 x, i32 y) {
            dab(brush_, x, y, d.width(), d.height(), [&](i32 px, i32 py) {
                if (!d.editable(px, py)) return;
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
    case Tool::Box:
    case Tool::Ellipse: {
        stroke_.last = snapped_end(stroke_.start, p);
        img = stroke_.base;
        std::vector<Point> pts;
        auto collect = [&](i32 x, i32 y) { pts.push_back({x, y}); };
        if (tool_ == Tool::Line) line(stroke_.start, stroke_.last, collect);
        else if (tool_ == Tool::Box) box(stroke_.start, stroke_.last, filled_, diamond_, collect);
        else ellipse(stroke_.start, stroke_.last, filled_, collect);
        // Outlines use the brush's size; filled shapes are filled pixel by pixel.
        if (filled_ && tool_ != Tool::Line) {
            for (Point q : pts)
                if (d.editable(q.x, q.y)) img.set(q.x, q.y, color(stroke_.secondary));
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
    if (!stroke_.on) return;
    stroke_.on = false;
    if (tool_ == Tool::Select) {
        selecting_ = false;
        Point a = stroke_.start, b = stroke_.last;
        Box bx{std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x) + 1, std::abs(b.y - a.y) + 1};
        if (a == b && select_mode_ == Mask::Mode::Replace) d.select_none(); // a click: nothing selected
        else d.select_box(bx, select_mode_);
        changed();
        return;
    }
    static constexpr const char* names[] = {"Move", "Select", "Wand", "Pencil", "Eraser", "Fill", "Pick", "Line", "Box", "Ellipse", "Gradient", "Shade"};
    if (!(d.layer().image == stroke_.base)) d.commit(names[static_cast<int>(tool_)]);
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
    if (stroke_.on || moving_) {
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
        }
        return;
    }
    // Tools.
    struct K {
        ImGuiKey key;
        Tool tool;
    };
    for (K k : {K{ImGuiKey_V, Tool::Move}, {ImGuiKey_M, Tool::Select}, {ImGuiKey_W, Tool::Wand}, {ImGuiKey_B, Tool::Pencil},
                {ImGuiKey_E, Tool::Eraser}, {ImGuiKey_G, Tool::Fill}, {ImGuiKey_I, Tool::Pick}, {ImGuiKey_L, Tool::Line},
                {ImGuiKey_R, Tool::Box}, {ImGuiKey_O, Tool::Ellipse}, {ImGuiKey_D, Tool::Gradient}, {ImGuiKey_S, Tool::Shade}})
        if (key(k.key)) {
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
    canvas_area_ = {area.x + tools_w, area.y + options_h, area.w - tools_w - side_w, area.h - options_h};
    if (fit_pending_ && canvas_) {
        fit();
        fit_pending_ = false;
    }
    options_bar(engine, {area.x, area.y, area.w, options_h});
    tools_column({area.x, area.y + options_h, tools_w, area.h - options_h});
    side_panels(engine, {area.right() - side_w, area.y + options_h, side_w, area.h - options_h});
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
    for (const ToolInfo& t : tool_list) {
        ImGui::PushID(static_cast<int>(t.tool));
        if (ui::tool_button("t", t.icon, tool_ == t.tool, t.tip, 36)) {
            if (canvas_ && t.tool != Tool::Move) canvas_->doc.put_down(), changed();
            tool_ = t.tool;
        }
        ImGui::PopID();
        if (t.tool == Tool::Wand || t.tool == Tool::Pick || t.tool == Tool::Ellipse) ImGui::Dummy({0, 4}); // groups
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
    chip({p.x + 12, p.y + 12}, secondary_);
    chip({p.x, p.y}, primary_);
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
        if (ImGui::MenuItem("Trim to what's painted")) d->trim(), changed();
        ImGui::Separator();
        if (ImGui::MenuItem("Flip across")) d->flip(true), changed();
        if (ImGui::MenuItem("Flip upside down")) d->flip(false), changed();
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
    bool brushy = tool_ == Tool::Pencil || tool_ == Tool::Eraser || tool_ == Tool::Shade || tool_ == Tool::Line ||
                  ((tool_ == Tool::Box || tool_ == Tool::Ellipse) && !filled_);
    if (brushy) {
        slider("Size", brush_.size, 1, 64, "Brush size. [ and ] change it", 70);
        ImGui::SameLine(0, 6);
        if (ui::tool_button("shape", brush_.square ? ui::Icon::Rect : ui::Icon::Circle, false,
                            brush_.square ? "A square brush: click for round" : "A round brush: click for square", 26))
            brush_.square = !brush_.square;
        if (tool_ != Tool::Line && tool_ != Tool::Box && tool_ != Tool::Ellipse) {
            slider("Density", brush_.density, 1, 16, "A 4 x 4 dither: 16 paints every pixel, 8 every other", 70);
            ImGui::SameLine(0, 10);
            ImGui::TextUnformatted("Mirror");
            ImGui::SameLine(0, 4);
            if (ui::tool_button("mx", ui::Icon::AlignCenterX, brush_.mirror_x, "Also paint mirrored left and right, about the middle", 26))
                brush_.mirror_x = !brush_.mirror_x;
            ImGui::SameLine(0, 2);
            if (ui::tool_button("my", ui::Icon::AlignCenterY, brush_.mirror_y, "Also paint mirrored top and bottom", 26))
                brush_.mirror_y = !brush_.mirror_y;
        }
        if (tool_ == Tool::Pencil || tool_ == Tool::Eraser) check("Pixel perfect", pixel_perfect_, "Takes out the L-shaped corners of one-pixel strokes");
    }
    if (tool_ == Tool::Fill || tool_ == Tool::Wand) {
        slider("Tolerance", tolerance_, 0, 255, "How different a color can be and still count as the same", 90);
        check("Contiguous", contiguous_, "Only the connected area; off: that color everywhere");
        check("All layers", sample_all_, "Looks at the picture as you see it, not only this layer");
    }
    if (tool_ == Tool::Box || tool_ == Tool::Ellipse) check("Filled", filled_, "Filled, or an outline");
    if (tool_ == Tool::Box) check("Iso diamond", diamond_, "A 2:1 floor diamond in the box. Shift keeps it 2:1");
    if (tool_ == Tool::Gradient) check("Dithered", dither_, "In two colors with a dither pattern, or smooth");
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
    batch_.draw(*canvas_->view, s);
    if (grid_ && zoom_ >= 6) {
        for (f32 x = 0; x <= w; x += 1) batch_.draw_rect({x - px * 0.5f, 0, px, h}, Color::hex(0xffffff, 0.07f));
        for (f32 y = 0; y <= h; y += 1) batch_.draw_rect({0, y - px * 0.5f, w, px}, Color::hex(0xffffff, 0.07f));
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
    // Where the brush would paint.
    if (over_canvas_ && !stroke_.on) {
        Point p = pixel_at(engine, ImGui::GetIO().MousePos);
        bool brushy = tool_ == Tool::Pencil || tool_ == Tool::Eraser || tool_ == Tool::Shade;
        if (brushy) {
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
