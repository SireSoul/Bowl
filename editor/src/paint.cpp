// Painting pixel art: the Paint workspace (one picture on a big canvas) and the Brush tool, which paints on a picture
// where it stands in the level. Both work on the same canvas, and every stroke updates the game's own texture, so the
// level shows the change at once. Saving writes the PNG.

#include "editor.hpp"

#include "ph/core/file.hpp"
#include "ph/gfx/sprite.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace ph::editor {

namespace fs = std::filesystem;

namespace {

constexpr float top_h = 48, status_h = 28, left_w = 300, right_w = 340;
constexpr ImGuiWindowFlags fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;

u32 pack(Color c) {
    auto b = [](f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return b(c.r) | b(c.g) << 8 | b(c.b) << 16 | b(c.a) << 24;
}
Color unpack(u32 p) { return Color::rgba8(static_cast<u8>(p), static_cast<u8>(p >> 8), static_cast<u8>(p >> 16), static_cast<u8>(p >> 24)); }

} // namespace

bool Editor::paint_open(Engine& engine, const std::string& path) {
    if (canvas_ && canvas_->path == path) return true;
    if (canvas_ && canvas_->modified) paint_save(engine); // painting never loses work
    fs::path file = assets_dir_ / path;
    auto bytes = read_file_bytes(file);
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = bytes ? stbi_load_from_memory(bytes->data(), static_cast<int>(bytes->size()), &w, &h, &channels, 4)
                            : nullptr;
    if (!pixels) {
        say("Can't open " + path + " for painting");
        return false;
    }
    Canvas c;
    c.path = path;
    c.file = file;
    c.w = static_cast<u32>(w);
    c.h = static_cast<u32>(h);
    c.pixels.assign(pixels, pixels + static_cast<usize>(w) * h * 4);
    stbi_image_free(pixels);
    c.history = {c.pixels};
    c.dirty = true;
    canvas_ = std::move(c);
    // The palette starts with the colors already in the picture (most used first, up to 32).
    std::vector<std::pair<u32, u32>> counts;
    for (usize i = 0; i < canvas_->pixels.size(); i += 4) {
        u32 p;
        std::memcpy(&p, &canvas_->pixels[i], 4);
        if ((p >> 24) == 0) continue;
        auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& kv) { return kv.first == p; });
        if (it != counts.end()) ++it->second;
        else if (counts.size() < 256) counts.emplace_back(p, 1);
    }
    std::sort(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    paint_palette_.clear();
    for (usize i = 0; i < counts.size() && i < 32; ++i) paint_palette_.push_back(counts[i].first);
    paint_zoom_ = std::clamp(std::floor(400.0f / static_cast<f32>(std::max(w, h))), 1.0f, 32.0f);
    paint_pan_ = {};
    paint_upload(engine);
    return true;
}

void Editor::paint_upload(Engine& engine) {
    if (!canvas_ || !canvas_->dirty) return;
    Canvas& c = *canvas_;
    if (!c.view || c.view->width() != c.w || c.view->height() != c.h)
        c.view = engine.gpu().create_texture({.width = c.w, .height = c.h, .rgba8 = c.pixels.data(), .filter = gfx::Filter::Nearest,
                                              .mipmaps = false, .name = "paint canvas"});
    else
        engine.gpu().update_texture(*c.view, c.pixels.data(), false);
    // The game's texture too, so the level shows the stroke.
    if (auto tex = engine.assets().load<gfx::Texture>(c.path); tex && tex->width() == c.w && tex->height() == c.h)
        engine.gpu().update_texture(*tex, c.pixels.data(), false);
    c.dirty = false;
}

bool Editor::paint_save(Engine& engine) {
    if (!canvas_) return false;
    if (!gfx::write_png(canvas_->file, canvas_->w, canvas_->h, canvas_->pixels)) {
        say("Couldn't save " + canvas_->path);
        return false;
    }
    canvas_->modified = false;
    say("Saved " + canvas_->path);
    (void)engine;
    return true;
}

void Editor::paint_commit() {
    if (!canvas_) return;
    Canvas& c = *canvas_;
    if (c.history[c.cursor] == c.pixels) return;
    c.history.resize(c.cursor + 1);
    c.history.push_back(c.pixels);
    if (c.history.size() > 100) c.history.erase(c.history.begin()); // pictures are small; 100 steps is plenty
    c.cursor = c.history.size() - 1;
}

void Editor::paint_undo(int direction) {
    if (!canvas_) return;
    Canvas& c = *canvas_;
    if (direction < 0 && c.cursor > 0) --c.cursor;
    else if (direction > 0 && c.cursor + 1 < c.history.size()) ++c.cursor;
    else return;
    c.pixels = c.history[c.cursor];
    c.modified = c.dirty = true;
}

// One dab of the current tool at a pixel, with a line from the last one so fast strokes have no gaps.
void Editor::paint_stroke(i32 x, i32 y, bool first) {
    if (!canvas_) return;
    Canvas& c = *canvas_;
    auto put = [&](i32 px, i32 py) {
        if (px < 0 || py < 0 || px >= static_cast<i32>(c.w) || py >= static_cast<i32>(c.h)) return;
        u8* p = &c.pixels[(static_cast<usize>(py) * c.w + static_cast<usize>(px)) * 4];
        u32 v = paint_tool_ == PaintTool::Eraser ? 0u : pack(paint_color_);
        std::memcpy(p, &v, 4);
        c.modified = c.dirty = true;
    };
    if (paint_tool_ == PaintTool::Picker) {
        if (x >= 0 && y >= 0 && x < static_cast<i32>(c.w) && y < static_cast<i32>(c.h)) {
            u32 v;
            std::memcpy(&v, &c.pixels[(static_cast<usize>(y) * c.w + static_cast<usize>(x)) * 4], 4);
            paint_color_ = unpack(v);
        }
        return;
    }
    if (paint_tool_ == PaintTool::Fill) {
        if (first) paint_fill(x, y);
        return;
    }
    i32 x0 = first ? x : paint_last_x_, y0 = first ? y : paint_last_y_;
    // Bresenham from the last dab.
    i32 dx = std::abs(x - x0), dy = -std::abs(y - y0), sx = x0 < x ? 1 : -1, sy = y0 < y ? 1 : -1, err = dx + dy;
    for (;;) {
        put(x0, y0);
        if (x0 == x && y0 == y) break;
        i32 e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
    paint_last_x_ = x;
    paint_last_y_ = y;
}

void Editor::paint_fill(i32 x, i32 y) {
    Canvas& c = *canvas_;
    if (x < 0 || y < 0 || x >= static_cast<i32>(c.w) || y >= static_cast<i32>(c.h)) return;
    auto at = [&](i32 px, i32 py) { return &c.pixels[(static_cast<usize>(py) * c.w + static_cast<usize>(px)) * 4]; };
    u32 from, to = pack(paint_color_);
    std::memcpy(&from, at(x, y), 4);
    if (from == to) return;
    std::vector<std::pair<i32, i32>> todo{{x, y}};
    while (!todo.empty()) {
        auto [px, py] = todo.back();
        todo.pop_back();
        if (px < 0 || py < 0 || px >= static_cast<i32>(c.w) || py >= static_cast<i32>(c.h)) continue;
        u32 v;
        std::memcpy(&v, at(px, py), 4);
        if (v != from) continue;
        std::memcpy(at(px, py), &to, 4);
        todo.insert(todo.end(), {{px + 1, py}, {px - 1, py}, {px, py + 1}, {px, py - 1}});
    }
    c.modified = c.dirty = true;
}

gfx::Camera2D Editor::paint_camera(Engine& engine) const {
    gfx::Camera2D cam = engine.camera();
    cam.zoom = paint_zoom_;
    cam.rotation = 0;
    // The canvas sits at the origin; center it in the space between the panels.
    const ImGuiIO& io = ImGui::GetIO();
    Vec2 area_center{(left_w + io.DisplaySize.x - right_w) * 0.5f, (top_h + io.DisplaySize.y - status_h) * 0.5f};
    Vec2 screen_center = io.DisplaySize.x > 0 ? Vec2{io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f} : Vec2{};
    Vec2 canvas_center = canvas_ ? Vec2{canvas_->w * 0.5f, canvas_->h * 0.5f} : Vec2{};
    cam.position = canvas_center + paint_pan_ - (area_center - screen_center) / paint_zoom_;
    return cam;
}

void Editor::paint_mouse(Engine& engine) {
    if (!canvas_) return;
    const ImGuiIO& io = ImGui::GetIO();
    gfx::Camera2D cam = paint_camera(engine);
    Vec2 mouse{io.MousePos.x, io.MousePos.y};
    Vec2 p = cam.screen_to_world(mouse);
    i32 x = static_cast<i32>(std::floor(p.x)), y = static_cast<i32>(std::floor(p.y));
    if (!io.WantCaptureMouse) {
        if (io.MouseWheel != 0) {
            f32 before = paint_zoom_;
            paint_zoom_ = std::clamp(paint_zoom_ * (io.MouseWheel > 0 ? 1.25f : 0.8f), 0.5f, 64.0f);
            paint_pan_ += (p - cam.position) * (1 - before / paint_zoom_); // zoom toward the mouse
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
            paint_pan_ -= Vec2{io.MouseDelta.x, io.MouseDelta.y} / paint_zoom_;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            painting_ = true;
            paint_stroke(x, y, true);
        }
    }
    if (painting_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) paint_stroke(x, y, false);
        else {
            painting_ = false;
            paint_commit();
        }
    }
    paint_upload(engine);
}

// The Brush tool: paint on a sprite in the level, at the pixel under the mouse, through its transform.
void Editor::brush_mouse(Engine& engine) {
    const ImGuiIO& io = ImGui::GetIO();
    ecs::World& world = engine.world();
    Vec2 p = mouse_world(engine);
    bool start = !io.WantCaptureMouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (!start && !painting_) return;
    if (start) {
        ecs::Entity e = pick(engine, p);
        const gfx::Sprite* s = e ? world.try_get<const gfx::Sprite>(e) : nullptr;
        if (!s || !s->texture || s->texture.path().empty()) {
            say("The brush paints on art: click a picture in the level");
            return;
        }
        if (!paint_open(engine, std::string(s->texture.path()))) return;
        brush_target_ = e;
    }
    const gfx::Sprite* s = world.alive(brush_target_) ? world.try_get<const gfx::Sprite>(brush_target_) : nullptr;
    const auto* g = world.try_get<const ecs::GlobalTransform2D>(brush_target_);
    if (!s || !g || !canvas_) {
        painting_ = false;
        return;
    }
    // World point -> the sprite's own space -> a pixel of its region.
    Vec2 size = gfx::sprite_size(*s) * g->scale;
    Vec2 local = (p - g->position).rotated(-g->rotation);
    Vec2 uv = Vec2{local.x / size.x, local.y / size.y} + s->origin;
    Rect region = s->region.w > 0 && s->region.h > 0 ? s->region : Rect{0, 0, f32(canvas_->w), f32(canvas_->h)};
    i32 x = static_cast<i32>(std::floor(region.x + uv.x * region.w));
    i32 y = static_cast<i32>(std::floor(region.y + uv.y * region.h));
    if (start) {
        painting_ = true;
        paint_stroke(x, y, true);
    } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        paint_stroke(x, y, false);
    } else {
        painting_ = false;
        paint_commit();
    }
    paint_upload(engine);
}

void Editor::paint_render(Engine& engine) {
    if (!canvas_ || !canvas_->view) return;
    gfx::Camera2D cam = paint_camera(engine);
    paint_batch_.begin(cam, gfx::SpriteSort::None);
    Canvas& c = *canvas_;
    f32 w = static_cast<f32>(c.w), h = static_cast<f32>(c.h), px = 1.0f / paint_zoom_;
    // A checkerboard under the picture shows what's transparent.
    f32 cell = std::max(1.0f, std::ceil(8.0f / paint_zoom_));
    for (f32 y = 0; y < h; y += cell)
        for (f32 x = 0; x < w; x += cell) {
            bool dark = (static_cast<int>(x / cell) + static_cast<int>(y / cell)) % 2;
            paint_batch_.draw_rect({x, y, std::min(cell, w - x), std::min(cell, h - y)}, dark ? Color::hex(0x2a2e38) : Color::hex(0x353a46));
        }
    gfx::SpriteInstance s;
    s.position = {0, 0};
    s.size = {w, h};
    s.origin = {0, 0};
    paint_batch_.draw(*c.view, s);
    // A pixel grid once pixels are big enough to see it.
    if (paint_zoom_ >= 6) {
        for (f32 x = 0; x <= w; x += 1) paint_batch_.draw_rect({x - px * 0.5f, 0, px, h}, Color::hex(0xffffff, 0.08f));
        for (f32 y = 0; y <= h; y += 1) paint_batch_.draw_rect({0, y - px * 0.5f, w, px}, Color::hex(0xffffff, 0.08f));
    }
    paint_batch_.draw_outline({-px, -px, w + 2 * px, h + 2 * px}, Color::hex(0xf2c45c, 0.6f), px);
    // The pixel under the mouse.
    if (!ImGui::GetIO().WantCaptureMouse) {
        Vec2 m = cam.screen_to_world({ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y});
        Rect under{std::floor(m.x), std::floor(m.y), 1, 1};
        if (under.x >= 0 && under.y >= 0 && under.x < w && under.y < h) paint_batch_.draw_outline(under, Color::white(), px * 1.5f);
    }
    engine.renderer().submit(paint_batch_);
}

void Editor::paint_workspace(Engine& engine) {
    const ImGuiIO& io = ImGui::GetIO();
    float h = io.DisplaySize.y - top_h - status_h;

    // Left: pictures to paint (the same art as the shelf).
    ImGui::SetNextWindowPos({0, top_h});
    ImGui::SetNextWindowSize({left_w, h});
    ImGui::Begin("##paintfiles", nullptr, fixed);
    ui::hint("Pick a picture to paint.");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "filter", art_filter_, sizeof art_filter_);
    ui::rule();
    std::string_view filter = art_filter_;
    for (Art& a : art_) {
        if (!filter.empty() && a.path.find(filter) == std::string::npos) continue;
        bool open = canvas_ && canvas_->path == a.path;
        std::string label = open && canvas_->modified ? a.path + " *" : a.path;
        if (ImGui::Selectable(label.c_str(), open)) paint_open(engine, a.path);
    }
    ImGui::End();

    // Right: tools, color, palette.
    ImGui::SetNextWindowPos({io.DisplaySize.x - right_w, top_h});
    ImGui::SetNextWindowSize({right_w, h});
    ImGui::Begin("##painttools", nullptr, fixed);
    if (!canvas_) {
        ui::hint("No picture open. The Brush tool in the Level paints on art where it stands, too.");
    } else {
        Canvas& c = *canvas_;
        ImGui::TextUnformatted(c.path.c_str());
        ImGui::TextDisabled("%u x %u%s", c.w, c.h, c.modified ? "  unsaved" : "");
        if (ImGui::Button("Save")) paint_save(engine);
        ImGui::SameLine();
        ImGui::BeginDisabled(c.cursor == 0);
        if (ImGui::Button("Undo")) paint_undo(-1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(c.cursor + 1 >= c.history.size());
        if (ImGui::Button("Redo")) paint_undo(1);
        ImGui::EndDisabled();
        ui::rule();
        struct T {
            PaintTool tool;
            const char* name;
            const char* tip;
        };
        for (T t : {T{PaintTool::Pencil, "Pencil", "Paint pixels|B"}, T{PaintTool::Eraser, "Eraser", "Make pixels transparent|E"},
                    T{PaintTool::Fill, "Fill", "Fill the area of one color|F"}, T{PaintTool::Picker, "Pick", "Take a color from the picture|I, or hold Alt"}}) {
            bool on = paint_tool_ == t.tool;
            ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::color::on : ui::color::button);
            if (ImGui::Button(t.name, {70, 0})) paint_tool_ = t.tool;
            ImGui::PopStyleColor();
            std::string_view tip(t.tip);
            ui::tip(tip.substr(0, tip.find('|')), tip.substr(tip.find('|') + 1));
            ImGui::SameLine();
        }
        ImGui::NewLine();
        ui::rule();
        ImGui::ColorPicker4("##color", &paint_color_.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview,
                            nullptr);
        ui::rule();
        ui::hint("Colors in this picture");
        for (usize i = 0; i < paint_palette_.size(); ++i) {
            Color col = unpack(paint_palette_[i]);
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::ColorButton("##p", {col.r, col.g, col.b, col.a}, ImGuiColorEditFlags_AlphaPreview, {22, 22}))
                paint_color_ = col;
            ImGui::PopID();
            if ((i + 1) % 10 != 0) ImGui::SameLine();
        }
        ImGui::NewLine();
        ImGui::TextDisabled("Zoom %.0fx. Wheel zooms, right-drag moves.", paint_zoom_);
    }
    ImGui::End();

    // Keys: tools, undo, save.
    if (!io.WantCaptureKeyboard) {
        Input& in = engine.input();
        bool cmd = in.down(Key::LeftCtrl) || in.down(Key::RightCtrl) || in.down(Key::LeftSuper) || in.down(Key::RightSuper);
        bool shift = in.down(Key::LeftShift) || in.down(Key::RightShift);
        if (cmd) {
            if (in.pressed(Key::Z)) paint_undo(shift ? 1 : -1);
            else if (in.pressed(Key::Y)) paint_undo(1);
            else if (in.pressed(Key::S)) paint_save(engine);
        } else {
            if (in.pressed(Key::B)) paint_tool_ = PaintTool::Pencil;
            if (in.pressed(Key::E)) paint_tool_ = PaintTool::Eraser;
            if (in.pressed(Key::F)) paint_tool_ = PaintTool::Fill;
            if (in.pressed(Key::I)) paint_tool_ = PaintTool::Picker;
        }
    }
    PaintTool held = paint_tool_;
    if (engine.input().down(Key::LeftAlt) && !painting_) paint_tool_ = PaintTool::Picker; // Alt picks for a moment
    paint_mouse(engine);
    paint_tool_ = held;
}

} // namespace ph::editor
