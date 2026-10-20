// The Materials workspace: the lists of materials and shaders, a material as a form, making glow and normal maps,
// and the preview the game's renderer draws. The code editor and the shader graph are in materials_shaders.cpp.

#include "materials.hpp"

#include "ui.hpp"

#include "bowl/core/file.hpp"
#include "bowl/core/time.hpp"
#include "bowl/gfx/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>

namespace bowl::editor {

namespace fs = std::filesystem;

namespace {

constexpr f32 list_w = 250, side_w = 360;
constexpr ImGuiWindowFlags fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
const ImVec4 muted = ImGui::ColorConvertU32ToFloat4(ui::color::muted);
const ImVec4 danger = ImGui::ColorConvertU32ToFloat4(ui::color::danger);

ImTextureID texture_id(const gfx::Texture& t) { return static_cast<ImTextureID>(reinterpret_cast<intptr_t>(t.handle())); }

std::string hex(Color c) {
    auto b = [](f32 v) { return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return std::format("#{:02x}{:02x}{:02x}", b(c.r), b(c.g), b(c.b));
}

Color color_of(const json::Value& v, Color fallback) {
    if (v.is_array() && v.size() >= 3) return {v[0].as_float(), v[1].as_float(), v[2].as_float(), v.size() > 3 ? v[3].as_float(1) : 1.0f};
    std::string_view s = v.as_string();
    if (s.size() == 7 && s[0] == '#') {
        u32 n = static_cast<u32>(std::strtoul(std::string(s.substr(1)).c_str(), nullptr, 16));
        return Color::hex(n);
    }
    return fallback;
}

// "glow_color" reads "Glow color".
std::string readable(std::string_view name) {
    if (name.starts_with("u_")) name.remove_prefix(2);
    std::string out(name);
    for (char& c : out)
        if (c == '_') c = ' ';
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

std::string file_stem(const std::string& path) {
    std::string s = fs::path(path).filename().string();
    for (std::string_view ext : {".frag.glsl", ".frag", ".shadergraph", ".material", ".png"})
        if (s.ends_with(ext)) return s.substr(0, s.size() - ext.size());
    return s;
}

// A small gem to try shaders on when there are no pictures.
std::unique_ptr<gfx::Texture> make_test_card(gfx::GpuDevice& gpu) {
    constexpr i32 n = 32;
    std::vector<u8> px(n * n * 4, 0);
    for (i32 y = 0; y < n; ++y)
        for (i32 x = 0; x < n; ++x) {
            f32 dx = (static_cast<f32>(x) + 0.5f - 16) / 14, dy = (static_cast<f32>(y) + 0.5f - 16) / 14;
            if (std::abs(dx) + std::abs(dy) > 1) continue;
            f32 light = 0.62f + 0.3f * (dx < 0 ? 1.0f : -0.4f) * (dy < 0 ? 1.0f : 0.6f);
            if (std::abs(dx) + std::abs(dy) > 0.82f) light *= 0.7f; // a darker rim
            if (std::abs(dx + 0.3f) + std::abs(dy + 0.35f) < 0.16f) light = 1.6f; // a glint
            u8* p = &px[static_cast<usize>(y * n + x) * 4];
            p[0] = static_cast<u8>(std::min(255.0f, 70 * light + 20));
            p[1] = static_cast<u8>(std::min(255.0f, 150 * light + 20));
            p[2] = static_cast<u8>(std::min(255.0f, 235 * light));
            p[3] = 255;
        }
    return gpu.create_texture({.width = n, .height = n, .rgba8 = px.data(), .filter = gfx::Filter::Nearest, .mipmaps = false,
                               .name = "test card"});
}

} // namespace

void MaterialsWorkspace::init(Engine& engine, fs::path assets_dir, std::function<void(std::string)> say) {
    engine_ = &engine;
    assets_ = &engine.assets();
    assets_dir_ = std::move(assets_dir);
    say_ = std::move(say);
    mono_ = ImGui::GetIO().Fonts->AddFontDefaultVector();
    test_card_ = make_test_card(engine.gpu());
    glow_look_.fragment = assets_->load<gfx::Shader>("engine/shaders/emissive.frag");
    Settings& prefs = engine.settings();
    lighting_ = prefs.get("editor.materials.lighting", true);
    bloom_ = prefs.get("editor.materials.bloom", true);
    if (prefs.has("editor.materials.sharp")) sharp_ = prefs.get("editor.materials.sharp", true);
    ambient_ = prefs.get("editor.materials.ambient", ambient_);
    light_color_ = prefs.get("editor.materials.light_color", light_color_);
    light_radius_ = prefs.get("editor.materials.light_radius", light_radius_);
    light_height_ = prefs.get("editor.materials.light_height", light_height_);
    scan();
}

void MaterialsWorkspace::scan() {
    materials_.clear();
    shaders_.clear();
    graphs_.clear();
    std::error_code ec;
    if (assets_dir_.empty()) return;
    for (auto it = fs::recursive_directory_iterator(assets_dir_, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec || !it->is_regular_file()) continue;
        std::string rel = fs::relative(it->path(), assets_dir_, ec).generic_string();
        if (rel.ends_with(".material")) materials_.push_back(rel);
        else if (rel.ends_with(".shadergraph")) graphs_.push_back(rel);
        else if (rel.ends_with(".frag.glsl")) shaders_.push_back(rel.substr(0, rel.size() - 5));
    }
    // A graph's shader is a shader too, before it's first saved.
    for (const std::string& g : graphs_) {
        std::string frag = g.substr(0, g.size() - 12) + ".frag";
        if (std::find(shaders_.begin(), shaders_.end(), frag) == shaders_.end()) shaders_.push_back(frag);
    }
    std::sort(materials_.begin(), materials_.end());
    std::sort(shaders_.begin(), shaders_.end());
    std::sort(graphs_.begin(), graphs_.end());
}

// Opening and saving

bool MaterialsWorkspace::open(const std::string& path) {
    if (unsaved()) save();
    glow_dialog_ = normal_dialog_ = false;
    selected_.clear();
    if (path.ends_with(".material")) {
        auto text = read_file_text(assets_dir_ / path);
        if (!text) {
            say_("Can't read " + path);
            return false;
        }
        doc_.load(*text);
        saved_ = *text;
        history_ = {*text};
        cursor_ = 0;
        kind_ = Kind::Material;
        path_ = path;
        rebuild_ = true;
        fit_ = true;
        return true;
    }
    std::string base = path;
    if (base.ends_with(".glsl")) base.resize(base.size() - 5);
    if (base.ends_with(".shadergraph")) base = base.substr(0, base.size() - 12) + ".frag";
    std::string graph = base.ends_with(".frag") ? base.substr(0, base.size() - 5) + ".shadergraph" : std::string();
    if (!graph.empty() && fs::exists(assets_dir_ / graph)) { // the graph that writes this shader
        auto text = read_file_text(assets_dir_ / graph);
        auto parsed = text ? json::parse(*text) : json::ParseResult{};
        std::string error;
        auto g = parsed ? shadergraph::from_json(parsed.value, &error) : std::nullopt;
        if (!g) {
            say_(std::format("Can't read {}: {}", graph, parsed ? error : text ? parsed.error->message : "missing"));
            return false;
        }
        graph_ = std::move(*g);
        graph_saved_ = json::stringify(shadergraph::to_json(graph_));
        graph_history_ = {graph_saved_};
        graph_cursor_ = 0;
        kind_ = Kind::Graph;
        path_ = graph;
        graph_fit_ = true;
        compiler_.source_name = fs::path(base).filename().string() + ".glsl";
        compiler_.source_path = assets_dir_ / (base + ".glsl");
        compiler_.shader = {};
        graph_changed(false);
        compile_soon(generated_, 0);
        try_rebuild_ = true;
        fit_ = true;
        return true;
    }
    auto text = read_file_text(assets_dir_ / (base + ".glsl"));
    if (!text) {
        say_("Can't read " + base + ".glsl");
        return false;
    }
    code_.set_text(*text);
    code_.set_marks({});
    code_saved_ = code_.text();
    kind_ = Kind::Code;
    path_ = base + ".glsl";
    compiler_.source_name = fs::path(path_).filename().string();
    compiler_.source_path = assets_dir_ / path_;
    compiler_.shader = {};
    compile_soon(code_.text(), 0);
    try_rebuild_ = true;
    fit_ = true;
    return true;
}

bool MaterialsWorkspace::unsaved() const {
    switch (kind_) {
    case Kind::Material: return doc_.text() != saved_;
    case Kind::Code: return code_.text() != code_saved_;
    case Kind::Graph: return json::stringify(shadergraph::to_json(graph_)) != graph_saved_;
    case Kind::None: return false;
    }
    return false;
}

bool MaterialsWorkspace::save() {
    switch (kind_) {
    case Kind::Material: {
        const std::string& text = doc_.text();
        if (!write_file_bytes(assets_dir_ / path_, text.data(), text.size())) {
            say_("Can't write " + path_);
            return false;
        }
        saved_ = text;
        say_("Saved " + fs::path(path_).filename().string());
        return true;
    }
    case Kind::Code: return save_code();
    case Kind::Graph: return save_graph();
    case Kind::None: return true;
    }
    return true;
}

std::string MaterialsWorkspace::sibling(const std::string& picture, std::string_view suffix) const {
    fs::path p(picture);
    return (p.parent_path() / (p.stem().string() + "_" + std::string(suffix) + ".png")).generic_string();
}

std::string MaterialsWorkspace::new_material(const std::string& picture, const std::string& shader) {
    fs::path where = picture.empty() ? fs::path("materials") / "material" : fs::path(picture).parent_path() / fs::path(picture).stem();
    std::string base = where.generic_string(), path = base + ".material";
    for (int n = 2; fs::exists(assets_dir_ / path); ++n) path = std::format("{}_{}.material", base, n);
    json::Value doc;
    doc.make_object();
    if (!picture.empty()) doc.set("preview", picture);
    if (!shader.empty()) doc.set("shader", shader);
    if (!picture.empty()) {
        for (std::string_view s : {"emissive", "glow"})
            if (std::string glow = sibling(picture, s); fs::exists(assets_dir_ / glow)) {
                json::Value e;
                e.set("map", glow);
                e.set("color", "#ffffff");
                e.set("strength", 2);
                doc.set("emissive", std::move(e));
                break;
            }
        for (std::string_view s : {"normal", "n"})
            if (std::string normal = sibling(picture, s); fs::exists(assets_dir_ / normal)) {
                doc.set("normal", normal);
                break;
            }
    }
    std::string text = json::stringify(doc, 2) + "\n";
    std::error_code ec;
    fs::create_directories((assets_dir_ / path).parent_path(), ec);
    if (!write_file_bytes(assets_dir_ / path, text.data(), text.size())) {
        say_("Can't write " + path);
        return {};
    }
    scan();
    open(path);
    say_("Made " + path);
    return path;
}

// Before the frame

void MaterialsWorkspace::update(Engine& engine) {
    finish_compiles(engine);
    if (kind_ == Kind::Material && rebuild_) {
        rebuild_ = false;
        warnings_.clear();
        preview_ = gfx::parse_material(doc_.valid() ? doc_.value() : json::Value{}, *assets_, &warnings_);
        if (!doc_.valid() && doc_.error())
            warnings_.insert(warnings_.begin(), std::format("The file isn't JSON (line {}: {}): fix it as text",
                                                            doc_.error()->line, doc_.error()->message));
    }
    if ((kind_ == Kind::Code || kind_ == Kind::Graph) && try_rebuild_) {
        try_rebuild_ = false;
        rebuild_try_material();
    }
    if (light_look_height_ != light_height_) {
        light_look_ = gfx::light_material(*assets_, {.normals = true, .height = light_height_, .shadows = true});
        light_look_height_ = light_height_;
    }
    // The map being made, shown in the preview while its dialog is open.
    if ((glow_dialog_ || normal_dialog_) && map_stale_) {
        map_stale_ = false;
        std::string picture = kind_ == Kind::Material && preview_ ? preview_->preview : std::string();
        auto img = read_picture(picture);
        if (img && img->width() > 0) {
            paint::Image map;
            if (normal_dialog_) map = paint::normal_map(*img, normal_options_);
            else
                map = paint::emissive_map(*img, [&](paint::Pixel p) {
                    return glow_colors_.contains(p | 0xff000000u) || paint::to_hsv(p).v >= glow_threshold_;
                });
            map_preview_is_normal_ = normal_dialog_;
            if (map_preview_ && map_preview_->width() == static_cast<u32>(map.width()) &&
                map_preview_->height() == static_cast<u32>(map.height()))
                engine.gpu().update_texture(*map_preview_, map.pixels().data(), false);
            else
                map_preview_ = engine.gpu().create_texture({.width = static_cast<u32>(map.width()), .height = static_cast<u32>(map.height()),
                                                            .rgba8 = map.pixels().data(), .filter = gfx::Filter::Nearest,
                                                            .mipmaps = false, .name = "map preview"});
        }
    }
    if (!glow_dialog_ && !normal_dialog_) map_preview_.reset();
}

// The panels

void MaterialsWorkspace::draw(Engine& engine, Rect area) {
    list_panel({area.x, area.y, list_w, area.h});
    Rect rest{area.x + list_w, area.y, std::max(1.0f, area.w - list_w), area.h};
    Rect middle{rest.x, rest.y, std::max(1.0f, rest.w - side_w), rest.h};
    Rect side{rest.right() - side_w, rest.y, side_w, rest.h};
    switch (kind_) {
    case Kind::Material:
        stage_ = middle;
        stage_panel(engine, stage_);
        material_panel(engine, side);
        break;
    case Kind::Code:
        code_panel(engine, middle);
        try_panel(engine, side);
        break;
    case Kind::Graph:
        graph_canvas(engine, middle);
        try_panel(engine, side);
        break;
    case Kind::None: {
        stage_ = {};
        ImGui::SetNextWindowPos({rest.center().x, rest.center().y}, 0, {0.5f, 0.5f});
        ImGui::Begin("##materials_empty", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
        ImGui::TextColored(muted, "Open a material or a shader on the left, or make one.");
        ImGui::Spacing();
        if (ImGui::Button("New material...")) new_material_dialog_ = true;
        ImGui::SameLine();
        if (ImGui::Button("New shader...")) new_shader_dialog_ = true;
        ImGui::End();
        break;
    }
    }
    new_dialogs(engine);
    maps_dialogs(engine);
    apply_edits();
    track_history();
    shortcuts(engine);
    if (!pending_open_.empty()) {
        std::string p = std::move(pending_open_);
        pending_open_.clear();
        open(p);
    }
}

void MaterialsWorkspace::list_panel(Rect area) {
    ImGui::SetNextWindowPos({area.x, area.y});
    ImGui::SetNextWindowSize({area.w, area.h});
    ImGui::Begin("##materials_list", nullptr, fixed);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Filter", filter_, sizeof filter_);
    auto shown = [&](const std::string& p) { return !filter_[0] || p.find(filter_) != std::string::npos; };
    auto row = [&](const std::string& path, ui::Icon icon, const std::string& open_as) {
        if (!shown(path)) return;
        ImGui::PushID(path.c_str());
        bool on = path_ == open_as || path_ == path;
        ImVec2 p = ImGui::GetCursorScreenPos();
        f32 h = 30;
        if (ImGui::Selectable("##row", on, 0, {0, h})) pending_open_ = open_as;
        ui::tip(path);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 c = on ? ui::color::accent : ui::color::text;
        ui::draw_icon(dl, icon, {p.x + 12, p.y + h * 0.5f}, 15, c);
        std::string name = file_stem(path);
        if (on && unsaved()) name += "  \xe2\x80\xa2"; // a dot: unsaved changes
        dl->AddText({p.x + 28, p.y + 2}, c, name.c_str());
        std::string folder = fs::path(path).parent_path().generic_string();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.8f);
        dl->AddText({p.x + 28, p.y + 16}, ui::color::muted, folder.empty() ? "(top)" : folder.c_str());
        ImGui::PopFont();
        ImGui::PopID();
    };
    ImGui::Spacing();
    ui::section("Materials");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 14);
    if (ui::tool_button("new_material", ui::Icon::Plus, false, "A new material: a picture's shader, glow, normal map and shadow", 22))
        new_material_dialog_ = true;
    for (const std::string& m : materials_) row(m, ui::Icon::Material, m);
    if (materials_.empty()) ui::hint("None yet. A material gives a picture its own shader, a glow (an emissive map), a normal map for lights, and a shadow.");
    ImGui::Dummy({0, 8});
    ui::section("Shaders");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 14);
    if (ui::tool_button("new_shader", ui::Icon::Plus, false, "A new shader: written as code, or built from nodes", 22))
        new_shader_dialog_ = true;
    for (const std::string& g : graphs_) row(g, ui::Icon::Graph, g);
    for (const std::string& s : shaders_) {
        std::string graph = s.substr(0, s.size() - 5) + ".shadergraph";
        if (std::find(graphs_.begin(), graphs_.end(), graph) != graphs_.end()) continue; // its graph is listed
        row(s + ".glsl", ui::Icon::Code, s + ".glsl");
    }
    if (shaders_.empty()) ui::hint("None yet. Shaders are how a material draws: write one as GLSL, or join nodes in a graph.");
    ImGui::End();
}

bool MaterialsWorkspace::picture_combo(const char* id, std::string& path, bool allow_none) {
    bool changed = false;
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##pic", path.empty() ? "(none)" : path.c_str(), ImGuiComboFlags_HeightLargest)) {
        static char find[64];
        if (ImGui::IsWindowAppearing()) {
            find[0] = 0;
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##find", "search", find, sizeof find);
        if (allow_none && ImGui::Selectable("(none)", path.empty())) {
            path.clear();
            changed = true;
        }
        if (hooks.pictures)
            for (const std::string& p : hooks.pictures()) {
                if (find[0] && p.find(find) == std::string::npos) continue;
                ImGui::PushID(p.c_str());
                thumbnail(p, 28);
                ImGui::SameLine(40);
                if (ImGui::Selectable(p.c_str(), p == path, 0, {0, 28})) {
                    path = p;
                    changed = true;
                }
                ImGui::PopID();
            }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return changed;
}

void MaterialsWorkspace::thumbnail(const std::string& path, f32 box) {
    auto t = path.empty() ? Asset<gfx::Texture>{} : assets_->load<gfx::Texture>(path);
    if (!t) {
        ImGui::Dummy({box, box});
        return;
    }
    f32 k = box / static_cast<f32>(std::max(t->width(), t->height()));
    ImGui::Image(texture_id(*t), {static_cast<f32>(t->width()) * k, static_cast<f32>(t->height()) * k});
}

// The material's form

void MaterialsWorkspace::set_key(json::TextDocument::Path container, const std::string& key, json::Value value) {
    edit([container, key, value](json::TextDocument& d) {
        // Make the objects on the way there.
        json::TextDocument::Path at;
        for (const auto& step : container) {
            json::TextDocument::Path next = at;
            next.push_back(step);
            const json::Value* v = &d.value();
            for (const auto& s : next) v = std::holds_alternative<std::string>(s) ? &(*v)[std::get<std::string>(s)] : &(*v)[std::get<usize>(s)];
            if (!v->is_object()) d.insert(at, json::Object{}, std::get<std::string>(step));
            at = next;
        }
        d.insert(container, value, key);
    });
}

void MaterialsWorkspace::erase_key(json::TextDocument::Path path) {
    edit([path](json::TextDocument& d) { d.erase(path); });
}

void MaterialsWorkspace::apply_edits() {
    if (edits_.empty()) return;
    if (!doc_.valid()) {
        edits_.clear();
        return;
    }
    for (auto& e : edits_) e(doc_);
    edits_.clear();
    rebuild_ = true;
}

void MaterialsWorkspace::track_history() {
    if (kind_ == Kind::Material) {
        if (ImGui::IsAnyItemActive() || history_.empty() || history_[cursor_] == doc_.text()) return;
        history_.resize(cursor_ + 1);
        history_.push_back(doc_.text());
        if (history_.size() > 200) history_.erase(history_.begin());
        cursor_ = history_.size() - 1;
    } else if (kind_ == Kind::Graph) {
        if (ImGui::IsAnyItemActive() || graph_drag_ != GraphDrag::None || graph_history_.empty()) return;
        std::string now = json::stringify(shadergraph::to_json(graph_));
        if (graph_history_[graph_cursor_] == now) return;
        graph_history_.resize(graph_cursor_ + 1);
        graph_history_.push_back(std::move(now));
        if (graph_history_.size() > 200) graph_history_.erase(graph_history_.begin());
        graph_cursor_ = graph_history_.size() - 1;
    }
}

void MaterialsWorkspace::undo(int direction) {
    if (kind_ == Kind::Material) {
        if ((direction < 0 && cursor_ == 0) || (direction > 0 && cursor_ + 1 >= history_.size())) return;
        cursor_ = static_cast<usize>(static_cast<i64>(cursor_) + direction);
        doc_.load(history_[cursor_]);
        rebuild_ = true;
    } else if (kind_ == Kind::Graph) {
        if ((direction < 0 && graph_cursor_ == 0) || (direction > 0 && graph_cursor_ + 1 >= graph_history_.size())) return;
        graph_cursor_ = static_cast<usize>(static_cast<i64>(graph_cursor_) + direction);
        if (auto g = shadergraph::from_json(json::parse(graph_history_[graph_cursor_]).value)) graph_ = std::move(*g);
        std::erase_if(selected_, [&](u32 id) { return !graph_.node(id); });
        graph_changed(false);
    }
}

void MaterialsWorkspace::shortcuts(Engine& engine) {
    Input& in = engine.input();
    bool cmd = in.down(Key::LeftCtrl) || in.down(Key::RightCtrl) || in.down(Key::LeftSuper) || in.down(Key::RightSuper);
    bool shift = in.down(Key::LeftShift) || in.down(Key::RightShift);
    if (cmd && in.pressed(Key::S)) save();
    bool typing = ImGui::GetIO().WantTextInput;
    if (kind_ == Kind::Code || typing) return; // the code editor has its own undo
    if (cmd && in.pressed(Key::Z)) undo(shift ? 1 : -1);
    if (cmd && in.pressed(Key::Y)) undo(1);
}

void MaterialsWorkspace::params_form(const gfx::Shader* shader, const json::Value* values,
                                     const std::function<void(const std::string&, json::Value)>& set,
                                     const std::function<void(const std::string&)>& reset) {
    if (!shader) return;
    bool any = false;
    for (const ShaderBundle::Uniform& u : shader->uniforms()) {
        if (u.name.starts_with("bowl_") || u.count > 1) continue;
        u32 n = ShaderBundle::components(u.type);
        if (n == 0 || n > 4) continue;
        any = true;
        ImGui::PushID(u.name.c_str());
        // What the file says, or the shader's default (shown dimmer).
        const json::Value* v = values ? values->find(u.name) : nullptr;
        f32 x[4] = {u.value[0], u.value[1], u.value[2], u.value[3]};
        if (!u.has_default) x[0] = x[1] = x[2] = x[3] = 0;
        if (v) {
            if (v->is_number()) x[0] = v->as_float();
            else if (v->is_bool()) x[0] = v->as_bool() ? 1.0f : 0.0f;
            else if (v->is_array())
                for (u32 i = 0; i < n && i < v->size(); ++i) x[i] = (*v)[i].as_float();
            else if (v->is_string()) {
                Color c = color_of(*v, Color::white());
                x[0] = c.r, x[1] = c.g, x[2] = c.b, x[3] = c.a;
            }
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(v ? ImGui::ColorConvertU32ToFloat4(ui::color::text) : muted, "%s", readable(u.name).c_str());
        if (ImGui::IsItemHovered()) {
            std::string help = u.help.empty() ? u.name : u.help + "  (" + u.name + ")";
            ui::tip(help, v ? "Right-click: back to the shader's default" : "Not in the file: the shader's default shows");
        }
        if (v && ImGui::BeginPopupContextItem("##reset")) {
            if (ImGui::MenuItem("Back to the default")) reset(u.name);
            ImGui::EndPopup();
        }
        ImGui::SameLine(124);
        ImGui::SetNextItemWidth(-1);
        bool integer = u.type == ShaderBundle::ValueType::Int || u.type == ShaderBundle::ValueType::IVec2 ||
                       u.type == ShaderBundle::ValueType::IVec3 || u.type == ShaderBundle::ValueType::IVec4 ||
                       u.type == ShaderBundle::ValueType::UInt;
        bool changed = false;
        if (u.color && n >= 3)
            changed = n == 4 ? ImGui::ColorEdit4("##v", x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_AlphaBar)
                             : ImGui::ColorEdit3("##v", x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        else if (integer) {
            int iv[4] = {static_cast<int>(std::round(x[0])), static_cast<int>(std::round(x[1])), static_cast<int>(std::round(x[2])),
                         static_cast<int>(std::round(x[3]))};
            if (n == 1 && u.max > u.min) changed = ImGui::SliderInt("##v", iv, static_cast<int>(u.min), static_cast<int>(u.max));
            else changed = ImGui::DragScalarN("##v", ImGuiDataType_S32, iv, static_cast<int>(n), 0.1f);
            for (u32 i = 0; i < 4; ++i) x[i] = static_cast<f32>(iv[i]);
        } else if (n == 1 && u.max > u.min) {
            changed = ImGui::SliderFloat("##v", x, u.min, u.max, "%.3f");
        } else {
            changed = ImGui::DragScalarN("##v", ImGuiDataType_Float, x, static_cast<int>(n), 0.01f, nullptr, nullptr, "%.3f");
        }
        if (changed) {
            if (n == 1) set(u.name, x[0]);
            else {
                json::Value a;
                for (u32 i = 0; i < n; ++i) a.push(std::round(x[i] * 10000.0f) / 10000.0f);
                set(u.name, std::move(a));
            }
        }
        ImGui::PopID();
    }
    if (!any) ui::hint("This shader has no parameters. Its Params block's members show here, with the defaults, ranges and help their comments give.");
}

void MaterialsWorkspace::textures_form(const gfx::Shader* shader, const json::Value* values,
                                       const std::function<void(const std::string&, std::string)>& set) {
    if (!shader) return;
    const auto& slots = shader->textures();
    if (slots.size() <= 1) return;
    for (usize b = 1; b < slots.size() && b <= 3; ++b) {
        ImGui::PushID(static_cast<int>(b));
        std::string current(values ? (*values)[slots[b].name].as_string() : std::string_view{});
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(readable(slots[b].name).c_str());
        if (ImGui::IsItemHovered()) ui::tip(slots[b].help.empty() ? slots[b].name : slots[b].help + "  (" + slots[b].name + ")");
        ImGui::SameLine(124);
        if (picture_combo("tex", current, true)) set(slots[b].name, current);
        ImGui::PopID();
    }
}

void MaterialsWorkspace::material_panel(Engine& engine, Rect area) {
    ImGui::SetNextWindowPos({area.x, area.y});
    ImGui::SetNextWindowSize({area.w, area.h});
    ImGui::Begin("##material_form", nullptr, fixed);
    const json::Value& v = doc_.value();
    // Its name, saving, its file as text, undo
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::color::accent), "%s", file_stem(path_).c_str());
    if (unsaved()) {
        ImGui::SameLine(0, 4);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::color::accent), "*");
        ui::tip("Unsaved changes", "Ctrl+S saves");
    }
    ImGui::SameLine(area.w - 4 * 30 - 8);
    if (ui::tool_button("undo", ui::Icon::Undo, false, "Undo|Ctrl+Z", 26, cursor_ > 0)) undo(-1);
    ImGui::SameLine(0, 2);
    if (ui::tool_button("redo", ui::Icon::Redo, false, "Redo|Ctrl+Shift+Z", 26, cursor_ + 1 < history_.size())) undo(1);
    ImGui::SameLine(0, 2);
    if (ui::tool_button("text", ui::Icon::Text, false, "The file as text: for anything this form doesn't show", 26) && hooks.open_text) {
        save();
        hooks.open_text(path_);
    }
    ImGui::SameLine(0, 2);
    if (ui::tool_button("save", ui::Icon::Save, false, "Save|Ctrl+S", 26)) save();
    ImGui::TextColored(muted, "%s", path_.c_str());
    for (const std::string& w : warnings_) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(danger, "%s", w.c_str());
        ImGui::PopTextWrapPos();
    }
    if (!doc_.valid()) {
        ImGui::End();
        return;
    }
    ImGui::Spacing();

    // The picture it's shown on
    if (ui::begin_card("Shown on", "picture")) {
        std::string preview(v["preview"].as_string());
        thumbnail(preview, 48);
        ImGui::SameLine();
        ImGui::BeginGroup();
        if (picture_combo("preview", preview, true)) {
            if (preview.empty()) erase_key({"preview"});
            else set_key({}, "preview", preview);
            fit_ = true;
        }
        ui::hint("For this preview: in the game, a sprite's own picture is drawn with it.");
        ImGui::EndGroup();
        ui::end_card();
    }

    // Its shader and blend mode
    const gfx::Shader* shader = preview_ && preview_->shader ? preview_->shader.get() : nullptr;
    if (ui::begin_card("Shader", "shader")) {
        std::string current(v["shader"].as_string());
        ImGui::SetNextItemWidth(-64);
        if (ImGui::BeginCombo("##shader", current.empty() ? "The sprite shader (plain)" : current.c_str())) {
            if (ImGui::Selectable("The sprite shader (plain)", current.empty())) erase_key({"shader"});
            for (const std::string& s : shaders_)
                if (ImGui::Selectable(s.c_str(), s == current)) set_key({}, "shader", s);
            ImGui::EndCombo();
        }
        ui::tip("How it draws: a shader of yours (Shaders, on the left), or the plain sprite shader");
        ImGui::SameLine(0, 4);
        ImGui::BeginDisabled(current.empty());
        if (ui::tool_button("edit_shader", ui::Icon::Code, false, "Edit this shader", 26)) pending_open_ = current;
        ImGui::EndDisabled();
        ImGui::SameLine(0, 2);
        if (ui::tool_button("new_shader_here", ui::Icon::Plus, false, "A new shader for this material", 26)) new_shader_dialog_ = true;
        if (!current.empty() && !shader) ImGui::TextColored(danger, "It didn't compile: open it to see why.");

        const char* blends[] = {"Alpha (normal)", "Additive (glows, fire)", "Multiply (shadows, tints)", "Opaque"};
        auto blend = gfx::blend_from_name(v["blend"].as_string());
        int b = blend ? static_cast<int>(*blend) : 0;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Blend");
        ImGui::SameLine(124);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##blend", &b, blends, 4)) {
            if (b == 0) erase_key({"blend"});
            else set_key({}, "blend", std::string(gfx::blend_name(static_cast<gfx::BlendMode>(b))));
        }
        ui::tip("How its pixels combine with what's behind them");
        ui::end_card();
    }

    if (shader && ui::begin_card("Parameters", "params")) {
        params_form(shader, &v["params"], [&](const std::string& name, json::Value x) { set_key({"params"}, name, std::move(x)); },
                    [&](const std::string& name) { erase_key({"params", name}); });
        ImGui::Spacing();
        textures_form(shader, &v["textures"], [&](const std::string& name, std::string p) {
            if (p.empty()) erase_key({"textures", name});
            else set_key({"textures"}, name, p);
        });
        ui::end_card();
    }
    glow_card(engine);
    normal_card(engine);

    if (ui::begin_card("Shadow", "shadow")) {
        const char* shadows[] = {"None: light goes through", "Its shape blocks light", "Faint: blocks nothing, never shadowed"};
        std::string_view s = v["shadow"].as_string();
        int k = s == "shape" ? 1 : s == "faint" ? 2 : 0;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##shadow", &k, shadows, 3)) {
            if (k == 0) erase_key({"shadow"});
            else set_key({}, "shadow", k == 1 ? "shape" : "faint");
        }
        ui::hint("For lights drawn with light_material in the game, and this preview's light. Faint suits the upright part of "
                 "something whose footprint casts the shadow.");
        ui::end_card();
    }
    ImGui::End();
}

void MaterialsWorkspace::glow_card(Engine&) {
    const json::Value& e = doc_.value()["emissive"];
    bool on = e.is_object() || e.is_string();
    bool open = ui::begin_card("Glow", "glow");
    if (!open) return;
    if (ImGui::Checkbox("Glows", &on)) {
        if (on) {
            json::Value o;
            o.set("color", "#ffffff");
            o.set("strength", 2);
            set_key({}, "emissive", std::move(o));
        } else {
            erase_key({"emissive"});
        }
    }
    ui::tip("Glowing parts stay bright in the dark and bloom: lamps, eyes, screens, lava");
    if (!on) {
        ui::end_card();
        return;
    }
    // The short form ("emissive": "map.png") becomes the long one before anything else changes.
    auto as_object = [&] {
        if (!e.is_string()) return;
        json::Value o;
        o.set("map", std::string(e.as_string()));
        o.set("strength", 1);
        set_key({}, "emissive", std::move(o));
    };
    std::string map(e.is_string() ? e.as_string() : e["map"].as_string());
    std::string preview(doc_.value()["preview"].as_string());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Map");
    ImGui::SameLine(124);
    if (picture_combo("glow_map", map, true)) {
        as_object();
        if (map.empty()) erase_key({"emissive", "map"});
        else set_key({"emissive"}, "map", map);
    }
    ui::tip("Bright where it glows, black (or see-through) where it doesn't, laid out like the picture. None: the picture "
            "itself glows where it's bright");
    ImGui::BeginDisabled(preview.empty());
    if (ImGui::Button("Make from the picture...")) {
        glow_dialog_ = true;
        normal_dialog_ = false;
        map_stale_ = true;
        auto img = read_picture(preview);
        picture_colors_ = img ? paint::colors_of(*img, 48) : std::vector<paint::Pixel>{};
    }
    ui::tip("Pick the picture's colors that glow, or everything brighter than a level", preview.empty() ? "The material needs a picture to show it on" : "");
    ImGui::SameLine();
    if (ImGui::Button("New blank")) {
        auto img = read_picture(preview);
        std::string path = sibling(preview, "emissive");
        if (img && write_picture(path, paint::Image(img->width(), img->height()))) {
            as_object();
            set_key({"emissive"}, "map", path);
            if (hooks.paint) hooks.paint(path);
        }
    }
    ui::tip("A see-through picture the size of this one, to paint the glowing parts on (opens in Paint, with the picture under it)");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(map.empty());
    if (ImGui::Button("Paint") && hooks.paint) hooks.paint(map);
    ImGui::EndDisabled();
    ui::tip("Paint the glow map, with the picture under it");

    Color c = color_of(e["color"], Color::white());
    f32 rgb[3] = {c.r, c.g, c.b};
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Color");
    ImGui::SameLine(124);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::ColorEdit3("##glow_color", rgb)) {
        as_object();
        set_key({"emissive"}, "color", hex({rgb[0], rgb[1], rgb[2], 1}));
    }
    ui::tip("Multiplies the map's colors: white keeps them");
    f32 strength = e.is_object() ? e["strength"].as_float(1) : 1;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Strength");
    ImGui::SameLine(124);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##glow_strength", &strength, 0, 16, "%.2f", ImGuiSliderFlags_Logarithmic)) {
        as_object();
        set_key({"emissive"}, "strength", std::round(strength * 100) / 100);
    }
    ui::tip("Above 1 it's brighter than white, and blooms when the game turns bloom on");
    ui::end_card();
}

void MaterialsWorkspace::normal_card(Engine&) {
    if (!ui::begin_card("Normal map", "normal")) return;
    std::string map(doc_.value()["normal"].as_string());
    std::string preview(doc_.value()["preview"].as_string());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Map");
    ImGui::SameLine(124);
    if (picture_combo("normal_map", map, true)) {
        if (map.empty()) erase_key({"normal"});
        else set_key({}, "normal", map);
    }
    ui::tip("Which way each pixel faces (OpenGL's convention: green up), laid out like the picture, so lights shade its bumps");
    ImGui::BeginDisabled(preview.empty());
    if (ImGui::Button("Make from the picture...")) {
        normal_dialog_ = true;
        glow_dialog_ = false;
        map_stale_ = true;
    }
    ui::tip("From the picture's shape (its edges bevel down) and brightness", preview.empty() ? "The material needs a picture to show it on" : "");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(map.empty());
    if (ImGui::Button("Paint") && hooks.paint) hooks.paint(map);
    ImGui::EndDisabled();
    ui::hint("Lights shade it in this preview, and in the game under lights drawn with light_material.");
    ui::end_card();
}

void MaterialsWorkspace::maps_dialogs(Engine&) {
    if (!glow_dialog_ && !normal_dialog_) return;
    std::string preview = preview_ ? preview_->preview : std::string();
    ImGui::SetNextWindowPos({stage_.x + 12, stage_.bottom() - 12}, ImGuiCond_Always, {0, 1});
    ImGui::SetNextWindowSize({std::min(stage_.w - 24, 420.0f), 0});
    bool open = true;
    const char* title = glow_dialog_ ? "Make a glow map###maps" : "Make a normal map###maps";
    ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_AlwaysAutoResize);
    bool changed = false;
    std::string target = sibling(preview, glow_dialog_ ? "emissive" : "normal");
    if (glow_dialog_) {
        ui::hint("Click the colors that glow. The preview shows the result.");
        f32 size = 22;
        f32 room = ImGui::GetContentRegionAvail().x;
        int per_row = std::max(1, static_cast<int>(room / (size + 4)));
        for (usize i = 0; i < picture_colors_.size(); ++i) {
            paint::Pixel p = picture_colors_[i] | 0xff000000u;
            ImGui::PushID(static_cast<int>(i));
            if (i % static_cast<usize>(per_row)) ImGui::SameLine(0, 4);
            ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##c", {size, size})) {
                if (!glow_colors_.erase(p)) glow_colors_.insert(p);
                changed = true;
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImU32 col = IM_COL32(paint::red(p), paint::green(p), paint::blue(p), 255);
            dl->AddRectFilled(at, {at.x + size, at.y + size}, col, 3);
            bool on = glow_colors_.contains(p);
            dl->AddRect(at, {at.x + size, at.y + size}, on ? ui::color::accent : ui::color::line, 3, 0, on ? 3.0f : 1.0f);
            ImGui::PopID();
        }
        bool bright = glow_threshold_ <= 1;
        if (ImGui::Checkbox("And everything brighter than", &bright)) {
            glow_threshold_ = bright ? 0.85f : 1.01f;
            changed = true;
        }
        if (bright) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            changed |= ImGui::SliderFloat("##bright", &glow_threshold_, 0, 1, "%.2f");
        }
    } else {
        ui::hint("The picture's shape bevels down at its edges, and brighter pixels stand out. The preview shows it under the light.");
        changed |= ImGui::SliderInt("Bevel", &normal_options_.bevel, 0, 12, "%d px");
        ui::tip("How far in from the edge the shape rises (0: only brightness counts)");
        changed |= ImGui::SliderFloat("Brightness", &normal_options_.brightness, 0, 1, "%.2f");
        ui::tip("How much brighter pixels stand out");
        changed |= ImGui::SliderFloat("Strength", &normal_options_.strength, 0.25f, 8, "%.2f", ImGuiSliderFlags_Logarithmic);
        ui::tip("How steep the slopes look");
        changed |= ImGui::Checkbox("Smooth", &normal_options_.smooth);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Dents instead of bumps", &normal_options_.invert);
    }
    if (changed) map_stale_ = true;
    ImGui::Spacing();
    bool exists = fs::exists(assets_dir_ / target);
    if (ImGui::Button(exists ? "Save (replaces it)" : "Save")) {
        auto img = read_picture(preview);
        if (img) {
            paint::Image map = glow_dialog_ ? paint::emissive_map(*img, [&](paint::Pixel p) {
                return glow_colors_.contains(p | 0xff000000u) || paint::to_hsv(p).v >= glow_threshold_;
            })
                                            : paint::normal_map(*img, normal_options_);
            if (write_picture(target, map)) {
                if (glow_dialog_) {
                    const json::Value& e = doc_.value()["emissive"];
                    if (!e.is_object()) {
                        json::Value o;
                        o.set("map", target);
                        o.set("color", "#ffffff");
                        o.set("strength", 2);
                        set_key({}, "emissive", std::move(o));
                    } else {
                        set_key({"emissive"}, "map", target);
                    }
                } else {
                    set_key({}, "normal", target);
                }
                say_("Saved " + target);
                open = false;
            }
        }
    }
    ui::tip(target);
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) open = false;
    ImGui::SameLine();
    ImGui::TextColored(muted, "%s", target.c_str());
    ImGui::End();
    if (!open) glow_dialog_ = normal_dialog_ = false;
}

std::optional<paint::Image> MaterialsWorkspace::read_picture(const std::string& path) const {
    if (path.empty()) return std::nullopt;
    auto bytes = read_file_bytes(assets_dir_ / path);
    u32 w = 0, h = 0;
    std::vector<u8> rgba;
    if (!bytes || !gfx::decode_png(*bytes, w, h, rgba)) return std::nullopt;
    paint::Image img(static_cast<i32>(w), static_cast<i32>(h));
    std::memcpy(img.pixels().data(), rgba.data(), rgba.size());
    return img;
}

bool MaterialsWorkspace::write_picture(const std::string& path, const paint::Image& image) {
    const auto* bytes = reinterpret_cast<const u8*>(image.pixels().data());
    std::span<const u8> rgba(bytes, image.pixels().size() * 4);
    if (!gfx::write_png(assets_dir_ / path, static_cast<u32>(image.width()), static_cast<u32>(image.height()), rgba)) {
        say_("Can't write " + path);
        return false;
    }
    if (hooks.pictures_changed) hooks.pictures_changed();
    return true;
}

// New files

void MaterialsWorkspace::new_dialogs(Engine&) {
    if (new_material_dialog_) {
        ImGui::OpenPopup("New material");
        new_material_dialog_ = false;
        if (new_picture_.empty() && hooks.pictures && !hooks.pictures().empty()) new_picture_ = hooks.pictures()[0];
    }
    if (new_shader_dialog_) {
        ImGui::OpenPopup("New shader");
        new_shader_dialog_ = false;
        std::snprintf(new_name_, sizeof new_name_, "shaders/new_shader");
    }
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.4f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({440, 0});
    if (ImGui::BeginPopupModal("New material", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ui::hint("A material for a picture: it's made beside it, and finds the picture's _emissive and _normal maps if they're "
                 "there.");
        ImGui::Spacing();
        thumbnail(new_picture_, 40);
        ImGui::SameLine();
        picture_combo("new_picture", new_picture_, true);
        ImGui::Spacing();
        if (ImGui::Button("Make it", {120, 0})) {
            ImGui::CloseCurrentPopup();
            new_material(new_picture_);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.4f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({460, 0});
    if (ImGui::BeginPopupModal("New shader", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("Where (in the assets, without an extension)");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##name", new_name_, sizeof new_name_);
        ImGui::Spacing();
        if (ImGui::RadioButton("Code: GLSL, with an example to start from", !new_graph_)) new_graph_ = false;
        if (ImGui::RadioButton("Graph: join nodes, no code", new_graph_)) new_graph_ = true;
        ImGui::Spacing();
        std::string base = new_name_;
        while (!base.empty() && (base.back() == '/' || base.back() == ' ')) base.pop_back();
        bool taken = base.empty() || fs::exists(assets_dir_ / (base + ".frag.glsl")) || fs::exists(assets_dir_ / (base + ".shadergraph"));
        if (taken && !base.empty()) ImGui::TextColored(danger, "There's a shader there already.");
        ImGui::BeginDisabled(taken);
        if (ImGui::Button("Make it", {120, 0})) {
            ImGui::CloseCurrentPopup();
            std::error_code ec;
            fs::create_directories((assets_dir_ / base).parent_path(), ec);
            std::string made;
            if (new_graph_) {
                std::string text = json::stringify(shadergraph::to_json(shadergraph::starter()), 2) + "\n";
                if (write_file_bytes(assets_dir_ / (base + ".shadergraph"), text.data(), text.size())) {
                    std::string glsl = shadergraph::generate(shadergraph::starter(), base + ".shadergraph").glsl;
                    write_file_bytes(assets_dir_ / (base + ".frag.glsl"), glsl.data(), glsl.size());
                    made = base + ".shadergraph";
                }
            } else {
                static constexpr std::string_view example = R"(#version 450
// A sprite material. Members of the Params block show up in every material that uses this shader, with the defaults,
// ranges and help their comments give; bowl_time is filled in with the seconds.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;    // the sprite's color, premultiplied
layout(location = 0) out vec4 out_color; // premultiplied

layout(set = 2, binding = 0) uniform sampler2D u_texture; // the sprite's picture
layout(std140, set = 3, binding = 0) uniform Params {
    vec4 tint;       // color = 1, 1, 1, 1   Multiplies the picture
    float shimmer;   // = 0.4 [0, 2]         How much it shimmers
    float speed;     // = 3 [0, 10]          How fast
    float bowl_time; // seconds: the renderer fills it in
};

void main() {
    vec4 c = texture(u_texture, v_uv) * v_color;
    float wave = 0.5 + 0.5 * sin(bowl_time * speed + v_uv.x * 10.0 - v_uv.y * 6.0);
    c.rgb *= tint.rgb * (1.0 + shimmer * wave * wave * wave);
    out_color = c * tint.a;
}
)";
                if (write_file_bytes(assets_dir_ / (base + ".frag.glsl"), example.data(), example.size())) made = base + ".frag.glsl";
            }
            if (made.empty()) say_("Can't write " + base);
            else {
                scan();
                if (kind_ == Kind::Material) { // the material that asked uses it
                    set_key({}, "shader", base + ".frag");
                    apply_edits();
                    save();
                }
                pending_open_ = made;
                say_("Made " + made);
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// The preview

gfx::Camera2D MaterialsWorkspace::camera(Engine& engine) const {
    gfx::Camera2D cam = engine.camera();
    cam.zoom = zoom_;
    cam.rotation = 0;
    cam.pixel_snap = sharp_now_; // whole screen pixels, so sharp pixels stay even
    const ImGuiIO& io = ImGui::GetIO();
    Vec2 screen_center{io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f};
    cam.position = pan_ - (stage_.center() - screen_center) / zoom_;
    return cam;
}

const gfx::Texture* MaterialsWorkspace::stage_texture() {
    std::string picture = kind_ == Kind::Material ? (preview_ ? preview_->preview : std::string()) : try_picture_;
    if (!picture.empty())
        if (auto t = assets_->load<gfx::Texture>(picture)) return t.get();
    return test_card_.get();
}

void MaterialsWorkspace::stage_panel(Engine& engine, Rect area) {
    const gfx::Texture* tex = stage_texture();
    sharp_now_ = sharp_.value_or(!tex || std::max(tex->width(), tex->height()) <= 256);
    if (fit_ && tex && area.w > 32) {
        fit_ = false;
        f32 k = std::min(area.w * 0.55f / static_cast<f32>(tex->width()), (area.h - 40) * 0.55f / static_cast<f32>(tex->height()));
        zoom_ = k >= 1 ? std::floor(k) : k;
        pan_ = {};
        light_ = {-static_cast<f32>(tex->width()) * 0.45f, -static_cast<f32>(tex->height()) * 0.45f};
        light_radius_ = std::max(light_radius_, static_cast<f32>(std::max(tex->width(), tex->height())) * 1.2f);
    }
    ImGui::SetNextWindowPos({area.x, area.y});
    ImGui::SetNextWindowSize({area.w, area.h});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8, 6});
    ImGui::Begin("##stage", nullptr, fixed | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                                         ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    // The bar across the top: what to show, the light, bloom.
    ImVec2 bar0 = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled({area.x, area.y}, {area.right(), area.y + 40}, IM_COL32(18, 20, 26, 220));
    const char* views[] = {"Lit", "Color", "Glow", "Normals"};
    const char* view_tips[] = {"As the game draws it, under the light", "Its colors, without the light or glow",
                               "Only its glow", "Its normal map"};
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine(0, 2);
        bool on = static_cast<int>(view_) == i;
        ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::color::on : ui::color::button);
        ImGui::PushStyleColor(ImGuiCol_Text, on ? ui::color::accent : ui::color::text);
        if (ImGui::Button(views[i])) view_ = static_cast<View>(i);
        ImGui::PopStyleColor(2);
        ui::tip(view_tips[i]);
    }
    ImGui::SameLine(0, 10);
    if (ui::tool_button("light", ui::Icon::Light, lighting_, "The light: dark around it, shading the normal map and casting shadows. Drag in the preview to move it", 26)) {
        lighting_ = !lighting_;
        engine.settings().set("editor.materials.lighting", lighting_);
    }
    ImGui::SameLine(0, 2);
    if (ui::tool_button("bloom", ui::Icon::Glow, bloom_, "Bloom: what's brighter than white glows", 26)) {
        bloom_ = !bloom_;
        engine.settings().set("editor.materials.bloom", bloom_);
    }
    ImGui::SameLine(0, 2);
    if (ui::tool_button("light_settings", ui::Icon::Gear, false, "The light's color, size and height, and the dark around it", 26))
        ImGui::OpenPopup("light_settings");
    if (ImGui::BeginPopup("light_settings")) {
        bool c = false;
        c |= ImGui::ColorEdit3("Light", &light_color_.r, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        c |= ImGui::ColorEdit3("Dark", &ambient_.r, ImGuiColorEditFlags_Float);
        ui::tip("The light where the light doesn't reach (the game's ambient)");
        c |= ImGui::SliderFloat("Size", &light_radius_, 8, 2048, "%.0f px", ImGuiSliderFlags_Logarithmic);
        c |= ImGui::SliderFloat("Height", &light_height_, 0.02f, 2, "%.2f");
        ui::tip("Low lights rake across bumps, high ones light them flat");
        ImGui::Checkbox("Follows the mouse", &light_follows_);
        if (c) {
            Settings& prefs = engine.settings();
            prefs.set("editor.materials.ambient", ambient_);
            prefs.set("editor.materials.light_color", light_color_);
            prefs.set("editor.materials.light_radius", light_radius_);
            prefs.set("editor.materials.light_height", light_height_);
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, 2);
    if (ui::tool_button("sharp", ui::Icon::Grid, sharp_now_,
                        "Sharp pixels: each picture pixel a crisp square however far it's zoomed in, as pixel-art games "
                        "draw them. Off: smooth, for drawn or painted art. Small pictures start sharp",
                        26)) {
        sharp_ = !sharp_now_;
        sharp_now_ = *sharp_;
        engine.settings().set("editor.materials.sharp", *sharp_);
    }
    ImGui::SameLine(0, 2);
    if (ui::tool_button("fit", ui::Icon::Search, false, "Fit the picture in the preview", 26)) fit_ = true;
    if (area.w > 440) {
        ImGui::SameLine(0, 8);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(muted, "%.0f%%", zoom_ * 100);
    }
    (void)bar0;

    // The rest takes the mouse: drag the light, pan with the right or middle button, zoom with the wheel.
    f32 top = 42;
    ImGui::SetCursorScreenPos({area.x, area.y + top});
    ImGui::InvisibleButton("##stage_input", {area.w, std::max(1.0f, area.h - top)},
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const ImGuiIO& io = ImGui::GetIO();
    gfx::Camera2D cam = camera(engine);
    Vec2 mouse{io.MousePos.x, io.MousePos.y};
    if (ImGui::IsItemHovered() && io.MouseWheel != 0) {
        Vec2 before = cam.screen_to_world(mouse);
        // Whole steps from 1 up, so pixels stay square.
        f32 z = zoom_ * std::pow(1.15f, io.MouseWheel);
        if (z >= 1) z = std::round(z);
        if (z == zoom_) z += io.MouseWheel > 0 ? 1.0f : -1.0f;
        zoom_ = std::clamp(z, 0.1f, 64.0f);
        cam = camera(engine);
        pan_ += before - cam.screen_to_world(mouse);
        cam = camera(engine);
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
        pan_ -= Vec2{io.MouseDelta.x, io.MouseDelta.y} / zoom_;
    bool lit = lighting_ && view_ == View::Lit;
    if (lit && ((ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) || (light_follows_ && ImGui::IsItemHovered())))
        light_ = cam.screen_to_world(mouse);
    if (lit) { // the light, as a ring
        Vec2 s = cam.world_to_screen(light_);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 c = ImGui::ColorConvertFloat4ToU32({std::min(light_color_.r, 1.0f), std::min(light_color_.g, 1.0f), std::min(light_color_.b, 1.0f), 1});
        dl->AddCircle({s.x, s.y}, 9, IM_COL32(0, 0, 0, 160), 20, 4);
        dl->AddCircle({s.x, s.y}, 9, c, 20, 2);
        dl->AddCircleFilled({s.x, s.y}, 3, c);
    }
    if (ImGui::IsItemHovered() && !ImGui::IsAnyItemActive())
        ui::tip(lit ? "Drag to move the light. Right or middle drag moves the view, the wheel zooms" : "Right or middle drag moves the view, the wheel zooms");
    ImGui::End();
}

void MaterialsWorkspace::render(Engine& engine) {
    gfx::Renderer& r = engine.renderer();
    if (stage_.w < 8 || stage_.h < 8 || kind_ == Kind::None) return;
    const gfx::Texture* tex = stage_texture();
    if (!tex) return;
    const gfx::SpriteMaterial* m = kind_ == Kind::Material ? preview_.get() : try_material_.get();
    gfx::Camera2D cam = camera(engine);
    layers_.begin(cam);
    layers_.set_filter(sharp_now_ ? std::optional<gfx::Filter>(gfx::Filter::Nearest) : std::nullopt);

    // A checkerboard behind it, over the preview's area.
    Vec2 a = cam.screen_to_world({stage_.x, stage_.y}), b = cam.screen_to_world({stage_.right(), stage_.bottom()});
    f32 cell = 16 / zoom_;
    bool dark = view_ == View::Glow;
    for (f32 y = std::floor(a.y / cell) * cell; y < b.y; y += cell)
        for (f32 x = std::floor(a.x / cell) * cell; x < b.x; x += cell) {
            bool odd = (static_cast<i64>(std::floor(x / cell)) + static_cast<i64>(std::floor(y / cell))) & 1;
            Color c = dark ? Color::hex(0x050507) : odd ? Color::hex(0x4a505c) : Color::hex(0x3e434e);
            layers_.scene.draw_rect({x, y, cell, cell}, c, -1e9f);
        }
    gfx::SpriteInstance s;
    s.size = tex->size();
    s.origin = {0.5f, 0.5f};

    // What glows, and its normal map: the material's, or a map being made.
    const gfx::Texture* glow_tex = nullptr;
    Color glow{};
    if (m && m->glows()) {
        glow_tex = m->emissive ? m->emissive.get() : tex;
        glow = m->glow();
    }
    if (glow_dialog_ && map_preview_ && !map_preview_is_normal_) {
        glow_tex = map_preview_.get();
        if (!m || m->emissive_strength <= 0) glow = {2, 2, 2, 1};
    }
    const gfx::Texture* normal_tex = m && m->normal ? m->normal.get() : nullptr;
    if (normal_dialog_ && map_preview_ && map_preview_is_normal_) normal_tex = map_preview_.get();

    bool lit = lighting_ && view_ == View::Lit;
    switch (view_) {
    case View::Lit:
    case View::Color: {
        layers_.scene.draw(tex, s, m ? m->look() : nullptr);
        if (view_ == View::Lit && glow_tex && glow_look_.fragment) {
            gfx::SpriteInstance g = s;
            g.color = glow;
            layers_.emissive.draw(glow_tex, g, &glow_look_);
        }
        if (lit && normal_tex) layers_.normals.draw(normal_tex, s);
        if (lit && m && m->shadow != gfx::Shadow::None) {
            gfx::SpriteInstance o = s;
            o.color = Color::white().with_alpha(m->shadow == gfx::Shadow::Shape ? 1.0f : 0.25f);
            layers_.occluders.draw(tex, o);
        }
        break;
    }
    case View::Glow:
        if (glow_tex && glow_look_.fragment) {
            gfx::SpriteInstance g = s;
            g.color = glow;
            layers_.scene.draw(glow_tex, g, &glow_look_);
        }
        break;
    case View::Normals:
        if (normal_tex) layers_.scene.draw(normal_tex, s);
        else layers_.scene.draw_rect({-s.size.x * 0.5f, -s.size.y * 0.5f, s.size.x, s.size.y}, Color{0.5f, 0.5f, 1, 1});
        break;
    }
    layers_.submit(r);

    gfx::PostSettings& post = r.post();
    post.bloom = bloom_ && view_ == View::Lit;
    post.lighting = lit;
    if (lit) {
        post.ambient = ambient_;
        lights_.begin(cam, gfx::SpriteSort::None, gfx::BlendMode::Additive, light_look_.fragment ? &light_look_ : nullptr);
        lights_.draw(&r.light_texture(), {.position = light_, .size = {light_radius_ * 2, light_radius_ * 2}, .color = light_color_});
        r.submit_light(lights_);
    }
}

} // namespace bowl::editor
