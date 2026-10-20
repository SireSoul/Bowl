// The Materials workspace's shaders: GLSL in a code editor that compiles as you type, and shader graphs on a canvas of
// nodes, both previewed live beside them on a picture, with test values for their parameters.

#include "materials.hpp"

#include "ui.hpp"

#include "bowl/core/file.hpp"
#include "bowl/core/time.hpp"

#if BOWL_EDITOR_SHADERC
#include "bowl/shaderc/shaderc.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace bowl::editor {

namespace fs = std::filesystem;
namespace sg = bowl::shadergraph;

namespace {

constexpr ImGuiWindowFlags fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
const ImVec4 muted = ImGui::ColorConvertU32ToFloat4(ui::color::muted);
const ImVec4 danger = ImGui::ColorConvertU32ToFloat4(ui::color::danger);

// The node canvas, in canvas units at zoom 1.
constexpr f32 node_w = 184, header_h = 26, sub_h = 18, row_h = 22, pin_r = 5.5f;

ImU32 category_color(std::string_view c) {
    if (c == "Input") return IM_COL32(64, 104, 168, 255);
    if (c == "Parameters") return IM_COL32(150, 116, 40, 255);
    if (c == "Math") return IM_COL32(78, 84, 98, 255);
    if (c == "Vector") return IM_COL32(48, 120, 116, 255);
    if (c == "UV") return IM_COL32(70, 128, 70, 255);
    if (c == "Pattern") return IM_COL32(112, 76, 150, 255);
    if (c == "Color") return IM_COL32(160, 72, 110, 255);
    if (c == "Effect") return IM_COL32(176, 96, 52, 255);
    return IM_COL32(140, 108, 40, 255); // Output
}

ImU32 pin_color(u8 size) {
    switch (size) {
    case 1: return IM_COL32(170, 176, 188, 255);
    case 2: return IM_COL32(120, 210, 120, 255);
    case 3: return IM_COL32(240, 200, 90, 255);
    default: return IM_COL32(236, 120, 170, 255);
    }
}

const char* size_name(u8 size) {
    switch (size) {
    case 1: return "a number";
    case 2: return "a vector (two numbers)";
    case 3: return "a color (no alpha)";
    default: return "a color";
    }
}

// A one-line summary under a node's title: the setting that says most about it.
std::string summary(const sg::Graph& g, const sg::Node& n) {
    const sg::NodeType* t = sg::node_type(n.type);
    if (!t) return {};
    if (n.type == "number") return sg::Writer::literal(g.number(n, "value")[0]);
    if (n.type == "texture") return g.text(n, "name") == "u_texture" ? "the sprite's picture" : g.text(n, "name");
    if (n.type.starts_with("param_")) return g.text(n, "name");
    if (n.type == "time" && g.number(n, "speed")[0] != 1) return std::format("x{}", sg::Writer::literal(g.number(n, "speed")[0]));
    for (const sg::Setting& s : t->settings)
        if (s.kind == sg::Setting::Kind::Choice) return g.text(n, s.key);
    return {};
}

std::string value_text(const sg::Values& v, u8 size) {
    if (size == 1) return sg::Writer::literal(v[0]);
    std::string s;
    for (u8 i = 0; i < size; ++i) s += (i ? ", " : "") + std::format("{:.3g}", v[i]);
    return s;
}

} // namespace

// Compiling

void MaterialsWorkspace::compile_soon(const std::string& text, f64 delay) {
    compiler_.pending = text;
    compiler_.due = now_seconds() + delay;
}

void MaterialsWorkspace::finish_compiles(Engine& engine) {
#if BOWL_EDITOR_SHADERC
    using namespace std::chrono_literals;
    if (compiler_.job.valid() && compiler_.job.wait_for(0s) == std::future_status::ready) {
        auto [ok, log] = compiler_.job.get();
        compiler_.took = now_seconds() - compiler_.took;
        compiler_.log = std::move(log);
        compiler_.failed = !ok;
        if (ok) {
            auto shader = engine.gpu().create_shader(*compiler_.bundle, compiler_.source_name);
            if (shader) compiler_.shader = assets_->add<gfx::Shader>("editor/try/" + compiler_.source_name, std::move(shader));
            else compiler_.failed = true, compiler_.log += "\nThe GPU didn't take it (see the log, F12).";
        }
        mark_problems();
        try_rebuild_ = true;
    }
    if (!compiler_.job.valid() && !compiler_.pending.empty() && now_seconds() >= compiler_.due) {
        shaderc::CompileOptions options;
        options.name = compiler_.source_name;
        options.stage = ShaderStage::Fragment;
        options.source_path = compiler_.source_path;
        if (auto engine_shaders = assets_->resolve("engine/shaders/common.glsl"))
            options.include_roots.emplace_back("engine/shaders/", engine_shaders->parent_path());
        auto bundle = std::make_shared<ShaderBundle>();
        compiler_.bundle = bundle;
        compiler_.took = now_seconds();
        compiler_.job = std::async(std::launch::async, [text = std::move(compiler_.pending), options, bundle] {
            shaderc::CompileResult r = shaderc::compile_glsl(text, options);
            if (r.ok) *bundle = std::move(r.bundle);
            return std::pair<bool, std::string>{r.ok, std::move(r.log)};
        });
        compiler_.pending.clear();
    }
#else
    (void)engine;
    if (!compiler_.pending.empty()) {
        compiler_.pending.clear();
        compiler_.failed = true;
        compiler_.log = "This build of the editor has no shader compiler: shaders show once the game builds them.";
    }
#endif
}

// Problems in the compiler's log, on their lines: "ERROR: /path/water.frag.glsl:12: 'x' : undeclared identifier".
void MaterialsWorkspace::mark_problems() {
    std::vector<CodeEditor::Mark> marks;
    std::string_view log = compiler_.log;
    while (!log.empty()) {
        usize nl = log.find('\n');
        std::string_view line = log.substr(0, nl);
        log = nl == std::string_view::npos ? std::string_view{} : log.substr(nl + 1);
        bool warning = line.starts_with("WARNING:");
        if (line.starts_with("ERROR:")) line.remove_prefix(6);
        else if (warning) line.remove_prefix(8);
        while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
        if (line.empty() || line.find("compilation errors") != std::string_view::npos ||
            line.find("compilation terminated") != std::string_view::npos)
            continue;
        CodeEditor::Mark m;
        m.warning = warning;
        // The first ":<digits>:" after the file's name is the line.
        for (usize i = 0; i + 2 < line.size(); ++i) {
            if (line[i] != ':' || !std::isdigit(static_cast<unsigned char>(line[i + 1]))) continue;
            usize j = i + 1;
            while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) ++j;
            if (j < line.size() && line[j] == ':') {
                std::string_view file = line.substr(0, i);
                bool own = file.ends_with(compiler_.source_name) || file == compiler_.source_path.string();
                if (own) m.line = std::stoi(std::string(line.substr(i + 1, j - i - 1)));
                std::string_view rest = line.substr(j + 1);
                while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
                m.message = own ? std::string(rest) : std::format("{} (in {})", rest, fs::path(file).filename().string());
                break;
            }
        }
        if (m.message.empty()) m.message = std::string(line);
        marks.push_back(std::move(m));
    }
    code_.set_marks(std::move(marks));
}

void MaterialsWorkspace::rebuild_try_material() {
    auto m = std::make_unique<gfx::SpriteMaterial>();
    m->shader = compiler_.shader;
    m->params = try_params_;
    for (const auto& [name, path] : try_textures_)
        if (!path.empty()) m->textures[name] = assets_->load<gfx::Texture>(path);
    try_material_ = std::move(m);
}

bool MaterialsWorkspace::save_code() {
    const std::string& text = code_.text();
    if (!write_file_bytes(assets_dir_ / path_, text.data(), text.size())) {
        say_("Can't write " + path_);
        return false;
    }
    code_saved_ = text;
    say_("Saved " + fs::path(path_).filename().string() + ": materials using it change with it");
    return true;
}

bool MaterialsWorkspace::save_graph() {
    std::string text = json::stringify(sg::to_json(graph_), 2) + "\n";
    std::string glsl_path = path_.substr(0, path_.size() - 12) + ".frag.glsl";
    std::string glsl = sg::generate(graph_, path_).glsl;
    if (!write_file_bytes(assets_dir_ / path_, text.data(), text.size()) ||
        !write_file_bytes(assets_dir_ / glsl_path, glsl.data(), glsl.size())) {
        say_("Can't write " + path_);
        return false;
    }
    graph_saved_ = json::stringify(sg::to_json(graph_));
    say_("Saved " + fs::path(path_).filename().string() + " and " + fs::path(glsl_path).filename().string());
    return true;
}

// The code editor

void MaterialsWorkspace::code_panel(Engine&, Rect area) {
    ImGui::SetNextWindowPos({area.x, area.y});
    ImGui::SetNextWindowSize({area.w, area.h});
    ImGui::Begin("##code_panel", nullptr, fixed | ImGuiWindowFlags_NoScrollbar);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::color::accent), "%s", fs::path(path_).filename().string().c_str());
    if (unsaved()) {
        ImGui::SameLine(0, 4);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::color::accent), "*");
        ui::tip("Unsaved: materials use the saved file", "Ctrl+S saves");
    }
    ImGui::SameLine(0, 16);
    if (compiler_.job.valid() || !compiler_.pending.empty()) ImGui::TextColored(muted, "Compiling...");
    else if (compiler_.failed) {
        usize n = code_.marks().size();
        ImGui::TextColored(danger, "%s", n == 1 ? "1 problem" : std::format("{} problems", n).c_str());
        if (compiler_.shader) ui::tip("The preview shows the last version that compiled");
    } else if (compiler_.shader) ImGui::TextColored(muted, "Compiled in %.0f ms", compiler_.took * 1000);
    std::string where = std::format("Line {}, column {}", code_.cursor_line(), code_.cursor_column());
    ImGui::SameLine(area.w - ImGui::CalcTextSize(where.c_str()).x - 54);
    ImGui::TextColored(muted, "%s", where.c_str());
    ImGui::SameLine(area.w - 40);
    if (ui::tool_button("save_code", ui::Icon::Save, false, "Save|Ctrl+S", 26)) save_code();

    const auto& marks = code_.marks();
    f32 problems_h = marks.empty() ? 0 : std::min(140.0f, 12 + static_cast<f32>(marks.size()) * ImGui::GetTextLineHeightWithSpacing());
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (code_.draw("code", {avail.x, avail.y - problems_h}, mono_)) compile_soon(code_.text(), 0.3);
    if (!marks.empty()) {
        ImGui::BeginChild("##problems", {0, 0});
        for (usize i = 0; i < marks.size(); ++i) {
            const CodeEditor::Mark& m = marks[i];
            ImGui::PushID(static_cast<int>(i));
            std::string label = m.line ? std::format("Line {}: {}", m.line, m.message) : m.message;
            ImGui::PushStyleColor(ImGuiCol_Text, m.warning ? ui::color::accent : ui::color::danger);
            if (ImGui::Selectable(label.c_str()) && m.line) code_.go_to(m.line);
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

// Beside code and graphs: the preview, what it's shown on, test values

void MaterialsWorkspace::try_panel(Engine& engine, Rect area) {
    f32 stage_h = std::clamp(area.h * 0.45f, 160.0f, area.w * 1.1f);
    stage_ = {area.x, area.y, area.w, stage_h};
    stage_panel(engine, stage_);
    ImGui::SetNextWindowPos({area.x, area.y + stage_h});
    ImGui::SetNextWindowSize({area.w, area.h - stage_h});
    ImGui::Begin("##try", nullptr, fixed);
    if (kind_ == Kind::Graph) graph_node_panel(engine);

    if (ui::begin_card("Try it", "try")) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("On");
        ImGui::SameLine(124);
        if (picture_combo("try_picture", try_picture_, true)) fit_ = true;
        ui::tip("The picture the preview shows it on (none: a test gem)");
        const gfx::Shader* shader = compiler_.shader ? compiler_.shader.get() : nullptr;
        if (shader) {
            json::Value values;
            values.make_object();
            for (const auto& [name, v] : try_params_) {
                json::Value a;
                for (f32 x : v) a.push(x);
                values.set(name, std::move(a));
            }
            params_form(shader, &values,
                        [&](const std::string& name, json::Value x) {
                            std::vector<f32> numbers;
                            if (x.is_number()) numbers.push_back(x.as_float());
                            for (const json::Value& n : x.array()) numbers.push_back(n.as_float());
                            try_params_[name] = std::move(numbers);
                            try_rebuild_ = true;
                        },
                        [&](const std::string& name) {
                            try_params_.erase(name);
                            try_rebuild_ = true;
                        });
            json::Value textures;
            textures.make_object();
            for (const auto& [name, p] : try_textures_) textures.set(name, p);
            textures_form(shader, &textures, [&](const std::string& name, std::string p) {
                try_textures_[name] = std::move(p);
                try_rebuild_ = true;
            });
        } else if (!compiler_.job.valid()) {
            ui::hint("It hasn't compiled yet: the problems are listed beside the code.");
        }
        ImGui::Spacing();
        if (ImGui::Button("Make a material with it")) {
            if (unsaved()) save();
            std::string shader_path = kind_ == Kind::Graph ? path_.substr(0, path_.size() - 12) + ".frag"
                                                           : path_.substr(0, path_.size() - 5);
            auto params = try_params_;
            auto textures = try_textures_;
            if (!new_material(try_picture_, shader_path).empty()) {
                for (const auto& [name, v] : params) {
                    json::Value x;
                    if (v.size() == 1) x = v[0];
                    else
                        for (f32 n : v) x.push(n);
                    set_key({"params"}, name, std::move(x));
                }
                for (const auto& [name, p] : textures)
                    if (!p.empty()) set_key({"textures"}, name, p);
                apply_edits();
                save();
            }
        }
        ui::tip("A material that draws with this shader, with these values: put it on a picture in the Level, or name it in the game");
        ui::hint("Values here are for trying it out. A material keeps its own; Ctrl+S saves the shader, and every material using it changes.");
        ui::end_card();
    }
    if (kind_ == Kind::Graph) {
        if (!graph_problems_.empty() || (compiler_.failed && !compiler_.log.empty())) {
            if (ui::begin_card("Problems", "graph_problems")) {
                for (const std::string& p : graph_problems_) {
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextColored(danger, "%s", p.c_str());
                    ImGui::PopTextWrapPos();
                }
                if (compiler_.failed)
                    for (const CodeEditor::Mark& m : code_.marks()) {
                        ImGui::PushTextWrapPos(0);
                        ImGui::TextColored(danger, "%s", m.message.c_str());
                        ImGui::PopTextWrapPos();
                    }
                ui::end_card();
            }
        }
        if (ImGui::Button(show_generated_ ? "Hide the code" : "The code it makes")) show_generated_ = !show_generated_;
        ui::tip("The GLSL this graph writes beside it when it's saved");
    }
    ImGui::End();

    if (kind_ == Kind::Graph && show_generated_) {
        ImGui::SetNextWindowSize({620, 520}, ImGuiCond_FirstUseEver);
        ImGui::Begin("The code the graph makes", &show_generated_, ImGuiWindowFlags_NoSavedSettings);
        if (mono_) ImGui::PushFont(mono_, 0);
        ImGui::InputTextMultiline("##generated", generated_.data(), generated_.size() + 1, {-1, -1}, ImGuiInputTextFlags_ReadOnly);
        if (mono_) ImGui::PopFont();
        ImGui::End();
    }
}

// The graph

void MaterialsWorkspace::graph_changed(bool record) {
    sg::Generated g = sg::generate(graph_, path_);
    graph_problems_ = std::move(g.problems);
    if (g.glsl != generated_) {
        generated_ = std::move(g.glsl);
        compile_soon(generated_, 0.12);
    }
    (void)record; // undo steps are taken once an edit settles (track_history)
}

void MaterialsWorkspace::add_node_menu(Vec2 at) {
    if (!ImGui::BeginPopup("add_node")) return;
    if (ImGui::IsWindowAppearing()) {
        node_filter_[0] = 0;
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##find", "Find a node", node_filter_, sizeof node_filter_);
    std::string want = node_filter_;
    std::transform(want.begin(), want.end(), want.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string picked;
    auto item = [&](const sg::NodeType& t) {
        if (ImGui::Selectable(t.title.c_str())) picked = t.id;
        if (ImGui::IsItemHovered()) ui::tip(t.help);
    };
    bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    if (!want.empty()) {
        bool first = true;
        for (const sg::NodeType& t : sg::node_types()) {
            if (t.id == "output") continue;
            std::string hay = t.title + " " + t.category + " " + t.help;
            std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (hay.find(want) == std::string::npos) continue;
            if (first && enter) picked = t.id;
            first = false;
            item(t);
        }
    } else {
        std::string category;
        bool open = false;
        for (const sg::NodeType& t : sg::node_types()) {
            if (t.id == "output") continue;
            if (t.category != category) {
                if (open) ImGui::EndMenu();
                category = t.category;
                open = ImGui::BeginMenu(category.c_str());
            }
            if (open) item(t);
        }
        if (open) ImGui::EndMenu();
    }
    if (!picked.empty()) {
        u32 id = graph_.add(picked, at);
        if (menu_link_) {
            const sg::NodeType* t = sg::node_type(picked);
            if (menu_link_from_output_ && !t->inputs.empty()) graph_.connect(menu_link_->first, menu_link_->second, id, 0);
            else if (!menu_link_from_output_ && !t->outputs.empty()) graph_.connect(id, 0, menu_link_->first, menu_link_->second);
        }
        selected_ = {id};
        graph_changed();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MaterialsWorkspace::graph_canvas(Engine& engine, Rect area) {
    ImGui::SetNextWindowPos({area.x, area.y});
    ImGui::SetNextWindowSize({area.w, area.h});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(20, 22, 28, 255));
    ImGui::Begin("##graph", nullptr, fixed | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    ImVec2 origin{area.x, area.y};
    f32 z = graph_zoom_;

    // Node sizes and pins, in screen coordinates.
    auto rows = [](const sg::NodeType& t) { return std::max(t.inputs.size(), t.outputs.size()); };
    auto node_rect = [&](const sg::Node& n) {
        const sg::NodeType& t = *sg::node_type(n.type);
        f32 h = header_h + (summary(graph_, n).empty() ? 0 : sub_h) + static_cast<f32>(rows(t)) * row_h + 8;
        ImVec2 a{origin.x + (n.position.x + graph_pan_.x) * z, origin.y + (n.position.y + graph_pan_.y) * z};
        return std::pair<ImVec2, ImVec2>{a, {a.x + node_w * z, a.y + h * z}};
    };
    auto pin_pos = [&](const sg::Node& n, bool output, u32 i) {
        auto [a, b] = node_rect(n);
        f32 top = header_h + (summary(graph_, n).empty() ? 0 : sub_h);
        return ImVec2{output ? b.x : a.x, a.y + (top + (static_cast<f32>(i) + 0.5f) * row_h) * z};
    };
    auto to_canvas = [&](ImVec2 s) { return Vec2{(s.x - origin.x) / z - graph_pan_.x, (s.y - origin.y) / z - graph_pan_.y}; };

    if (graph_fit_ && !graph_.nodes.empty()) { // everything in view
        graph_fit_ = false;
        Vec2 lo{1e9f, 1e9f}, hi{-1e9f, -1e9f};
        for (const sg::Node& n : graph_.nodes) {
            lo = {std::min(lo.x, n.position.x), std::min(lo.y, n.position.y)};
            hi = {std::max(hi.x, n.position.x + node_w), std::max(hi.y, n.position.y + 160)};
        }
        graph_zoom_ = z = std::clamp(std::min(area.w / (hi.x - lo.x + 120), area.h / (hi.y - lo.y + 120)), 0.35f, 1.25f);
        graph_pan_ = Vec2{area.w / z * 0.5f, area.h / z * 0.5f} - (lo + hi) * 0.5f;
    }

    // The grid
    {
        f32 step = 32 * z;
        f32 ox = std::fmod(graph_pan_.x * z, step), oy = std::fmod(graph_pan_.y * z, step);
        for (f32 x = area.x + ox; x < area.right(); x += step) dl->AddLine({x, area.y}, {x, area.bottom()}, IM_COL32(255, 255, 255, 9));
        for (f32 y = area.y + oy; y < area.bottom(); y += step) dl->AddLine({area.x, y}, {area.right(), y}, IM_COL32(255, 255, 255, 9));
    }

    // The mouse
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##canvas", {area.w, area.h},
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();
    ImVec2 mouse = io.MousePos;
    Vec2 cm = to_canvas(mouse);
    struct Hit {
        u32 node = 0;
        int pin = -1;
        bool output = false;
    };
    auto hit = [&]() {
        Hit h;
        for (auto it = graph_.nodes.rbegin(); it != graph_.nodes.rend(); ++it) {
            const sg::NodeType& t = *sg::node_type(it->type);
            for (u32 i = 0; i < t.outputs.size(); ++i) {
                ImVec2 p = pin_pos(*it, true, i);
                if (std::hypot(p.x - mouse.x, p.y - mouse.y) < std::max(9.0f, pin_r * z * 1.8f)) return Hit{it->id, static_cast<int>(i), true};
            }
            for (u32 i = 0; i < t.inputs.size(); ++i) {
                ImVec2 p = pin_pos(*it, false, i);
                if (std::hypot(p.x - mouse.x, p.y - mouse.y) < std::max(9.0f, pin_r * z * 1.8f)) return Hit{it->id, static_cast<int>(i), false};
            }
            auto [a, b] = node_rect(*it);
            if (mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y) {
                h.node = it->id;
                return h;
            }
        }
        return h;
    };
    bool shift = io.KeyShift;
    bool space = ImGui::IsKeyDown(ImGuiKey_Space) && !io.WantTextInput;
    if (hovered && io.MouseWheel != 0) {
        f32 nz = std::clamp(z * std::pow(1.12f, io.MouseWheel), 0.3f, 2.0f);
        graph_pan_ = Vec2{(mouse.x - origin.x) / nz, (mouse.y - origin.y) / nz} - cm;
        graph_zoom_ = z = nz;
        cm = to_canvas(mouse);
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && graph_drag_ == GraphDrag::None) {
        Hit h = hit();
        if (space) graph_drag_ = GraphDrag::Pan;
        else if (h.pin >= 0 && h.output) {
            graph_drag_ = GraphDrag::Link;
            drag_from_output_ = true;
            drag_node_ = h.node;
            drag_pin_ = static_cast<u32>(h.pin);
        } else if (h.pin >= 0) {
            // From an input: pick up what's joined there and move its end, or start a link backwards.
            if (const sg::Link* l = graph_.link_into(h.node, static_cast<u32>(h.pin))) {
                drag_from_output_ = true;
                drag_node_ = l->from;
                drag_pin_ = l->output;
                graph_.disconnect(h.node, static_cast<u32>(h.pin));
                graph_changed();
            } else {
                drag_from_output_ = false;
                drag_node_ = h.node;
                drag_pin_ = static_cast<u32>(h.pin);
            }
            graph_drag_ = GraphDrag::Link;
        } else if (h.node) {
            if (shift) {
                if (!selected_.erase(h.node)) selected_.insert(h.node);
            } else if (!selected_.contains(h.node)) {
                selected_ = {h.node};
            }
            graph_drag_ = GraphDrag::Nodes;
            drag_node_ = h.node;
            drag_start_ = cm;
            graph_moved_ = false;
            // What's clicked draws on top.
            auto it = std::find_if(graph_.nodes.begin(), graph_.nodes.end(), [&](const sg::Node& n) { return n.id == h.node; });
            if (it != graph_.nodes.end()) std::rotate(it, it + 1, graph_.nodes.end());
        } else {
            if (!shift) selected_.clear();
            graph_drag_ = GraphDrag::Box;
            drag_start_ = cm;
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) graph_drag_ = GraphDrag::Pan;
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right) && graph_drag_ == GraphDrag::None) graph_drag_ = GraphDrag::Pan;
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && graph_drag_ == GraphDrag::None) {
        Hit h = hit();
        if (h.node && h.pin < 0) {
            if (!selected_.contains(h.node)) selected_ = {h.node};
            ImGui::OpenPopup("node_menu");
        } else {
            menu_at_ = cm;
            menu_link_.reset();
            ImGui::OpenPopup("add_node");
        }
    }
    switch (graph_drag_) {
    case GraphDrag::Pan:
        graph_pan_ += Vec2{io.MouseDelta.x, io.MouseDelta.y} / z;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle) &&
            !ImGui::IsMouseDown(ImGuiMouseButton_Right))
            graph_drag_ = GraphDrag::None;
        break;
    case GraphDrag::Nodes:
        if (cm.x != drag_start_.x || cm.y != drag_start_.y) {
            for (u32 id : selected_)
                if (sg::Node* n = graph_.node(id)) n->position += cm - drag_start_;
            drag_start_ = cm;
            graph_moved_ = true;
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            graph_drag_ = GraphDrag::None;
            if (!graph_moved_ && !shift) selected_ = {drag_node_}; // a click picks just that one
        }
        break;
    case GraphDrag::Box: {
        Vec2 a = drag_start_;
        ImVec2 sa{origin.x + (a.x + graph_pan_.x) * z, origin.y + (a.y + graph_pan_.y) * z};
        dl->AddRectFilled(sa, mouse, IM_COL32(242, 196, 92, 25));
        dl->AddRect(sa, mouse, ui::color::accent);
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            ImVec2 lo{std::min(sa.x, mouse.x), std::min(sa.y, mouse.y)}, hi{std::max(sa.x, mouse.x), std::max(sa.y, mouse.y)};
            for (const sg::Node& n : graph_.nodes) {
                auto [p, q] = node_rect(n);
                if (p.x < hi.x && q.x > lo.x && p.y < hi.y && q.y > lo.y) selected_.insert(n.id);
            }
            graph_drag_ = GraphDrag::None;
        }
        break;
    }
    case GraphDrag::Link:
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            graph_drag_ = GraphDrag::None;
            Hit h = hit();
            if (h.pin >= 0 && h.output != drag_from_output_) {
                bool ok = drag_from_output_ ? graph_.connect(drag_node_, drag_pin_, h.node, static_cast<u32>(h.pin))
                                            : graph_.connect(h.node, static_cast<u32>(h.pin), drag_node_, drag_pin_);
                if (!ok) say_("That would make a loop");
                graph_changed();
            } else if (!h.node) { // into nothing: a new node, joined to it
                menu_at_ = cm;
                menu_link_ = std::pair{drag_node_, drag_pin_};
                menu_link_from_output_ = drag_from_output_;
                ImGui::OpenPopup("add_node");
            }
        }
        break;
    case GraphDrag::None: break;
    }

    // Links
    auto curve = [&](ImVec2 a, ImVec2 b, ImU32 c, f32 thick) {
        f32 d = std::max(40.0f * z, std::abs(b.x - a.x) * 0.5f);
        dl->AddBezierCubic(a, {a.x + d, a.y}, {b.x - d, b.y}, b, c, thick);
    };
    for (const sg::Link& l : graph_.links) {
        const sg::Node* a = graph_.node(l.from);
        const sg::Node* b = graph_.node(l.to);
        if (!a || !b) continue;
        u8 size = sg::node_type(a->type)->outputs[l.output].size;
        curve(pin_pos(*a, true, l.output), pin_pos(*b, false, l.input), pin_color(size), 2.5f * std::max(z, 0.6f));
    }
    if (graph_drag_ == GraphDrag::Link)
        if (const sg::Node* n = graph_.node(drag_node_)) {
            ImVec2 p = pin_pos(*n, drag_from_output_, drag_pin_);
            if (drag_from_output_) curve(p, mouse, ui::color::accent, 2.5f);
            else curve(mouse, p, ui::color::accent, 2.5f);
        }

    // Nodes
    ImFont* font = ImGui::GetFont();
    f32 fs = ImGui::GetFontSize() * z;
    f32 small = fs * 0.85f;
    Hit under = hovered && graph_drag_ == GraphDrag::None ? hit() : Hit{};
    for (const sg::Node& n : graph_.nodes) {
        const sg::NodeType& t = *sg::node_type(n.type);
        auto [a, b] = node_rect(n);
        if (b.x < area.x || a.x > area.right() || b.y < area.y || a.y > area.bottom()) continue;
        bool sel = selected_.contains(n.id);
        f32 r = 7 * z;
        dl->AddRectFilled({a.x + 3, a.y + 4}, {b.x + 3, b.y + 4}, IM_COL32(0, 0, 0, 70), r);
        dl->AddRectFilled(a, b, IM_COL32(36, 40, 50, 245), r);
        dl->AddRectFilled(a, {b.x, a.y + header_h * z}, category_color(t.category), r, ImDrawFlags_RoundCornersTop);
        dl->AddText(font, fs, {a.x + 9 * z, a.y + (header_h * z - fs) * 0.5f}, IM_COL32(245, 245, 250, 255), t.title.c_str());
        f32 y = a.y + header_h * z;
        if (std::string sub = summary(graph_, n); !sub.empty()) {
            dl->AddText(font, small, {a.x + 9 * z, y + 2 * z}, ui::color::accent, sub.c_str());
            y += sub_h * z;
        }
        for (u32 i = 0; i < t.inputs.size(); ++i) {
            const sg::Pin& p = t.inputs[i];
            ImVec2 c = pin_pos(n, false, i);
            bool joined = graph_.link_into(n.id, i) != nullptr;
            bool hot = under.node == n.id && under.pin == static_cast<int>(i) && !under.output;
            if (joined) dl->AddCircleFilled(c, pin_r * z, pin_color(p.size));
            else {
                dl->AddCircleFilled(c, pin_r * z, IM_COL32(30, 33, 41, 255));
                dl->AddCircle(c, pin_r * z, pin_color(p.size), 12, 1.5f);
            }
            if (hot) dl->AddCircle(c, pin_r * z + 3, ui::color::accent, 12, 2);
            std::string label = p.name;
            if (!joined) {
                if (p.uv) label += p.name == "UV" ? "  (the sprite's)" : "  (UV)";
                else if (p.color) {
                    sg::Values v = graph_.number(n, "in:" + p.name);
                    ImVec2 sw{c.x + 12 * z + ImGui::CalcTextSize(p.name.c_str()).x * z + 6 * z, c.y - 5 * z};
                    dl->AddRectFilled(sw, {sw.x + 16 * z, sw.y + 10 * z},
                                      ImGui::ColorConvertFloat4ToU32({std::min(v[0], 1.0f), std::min(v[1], 1.0f), std::min(v[2], 1.0f), 1}), 2);
                } else label += "  " + value_text(graph_.number(n, "in:" + p.name), p.size);
            }
            dl->AddText(font, small, {c.x + 12 * z, c.y - small * 0.5f}, joined ? ui::color::text : ui::color::muted, label.c_str());
            if (hot) ui::tip(std::format("{}: {}", p.name, size_name(p.size)), joined ? "Drag to move or remove its link" : "Drag to an output to join them");
        }
        for (u32 i = 0; i < t.outputs.size(); ++i) {
            const sg::Pin& p = t.outputs[i];
            ImVec2 c = pin_pos(n, true, i);
            bool hot = under.node == n.id && under.pin == static_cast<int>(i) && under.output;
            dl->AddCircleFilled(c, pin_r * z, pin_color(p.size));
            if (hot) dl->AddCircle(c, pin_r * z + 3, ui::color::accent, 12, 2);
            f32 w = ImGui::CalcTextSize(p.name.c_str()).x * z * 0.85f;
            dl->AddText(font, small, {c.x - 12 * z - w, c.y - small * 0.5f}, ui::color::text, p.name.c_str());
            if (hot) ui::tip(std::format("{}: {}", p.name, size_name(p.size)), "Drag to an input to join them");
        }
        dl->AddRect(a, b, sel ? ui::color::accent : n.type == "output" ? IM_COL32(242, 196, 92, 120) : IM_COL32(70, 78, 92, 255), r, 0,
                    sel ? 2.0f : 1.0f);
        (void)y;
    }
    if (under.node && under.pin < 0 && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
        if (const sg::Node* n = graph_.node(under.node)) ui::tip(sg::node_type(n->type)->help);
    if (graph_.nodes.size() <= 4 && graph_drag_ == GraphDrag::None)
        dl->AddText({area.x + 14, area.bottom() - 26}, ui::color::muted,
                    "Right-click to add a node. Drag from a pin to another to join them; drag into nothing to add one there.");

    // Menus
    add_node_menu(menu_at_);
    if (ImGui::BeginPopup("node_menu")) {
        bool output_only = selected_.size() == 1 && graph_.node(*selected_.begin()) &&
                           graph_.node(*selected_.begin())->type == "output";
        ImGui::BeginDisabled(output_only);
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
            std::set<u32> made;
            for (u32 id : selected_)
                if (const sg::Node* n = graph_.node(id); n && n->type != "output") {
                    sg::Node copy = *n;
                    copy.id = graph_.next_id++;
                    copy.position += Vec2{30, 30};
                    graph_.nodes.push_back(copy);
                    made.insert(copy.id);
                }
            selected_ = made;
            graph_changed();
        }
        if (ImGui::MenuItem("Delete", "Delete")) {
            for (u32 id : selected_) graph_.remove(id);
            selected_.clear();
            graph_changed();
        }
        ImGui::EndDisabled();
        if (ImGui::MenuItem("Disconnect")) {
            std::erase_if(graph_.links, [&](const sg::Link& l) { return selected_.contains(l.from) || selected_.contains(l.to); });
            graph_changed();
        }
        ImGui::EndPopup();
    }

    // Keys
    if ((hovered || ImGui::IsWindowFocused()) && !io.WantTextInput) {
        bool cmd = io.KeyCtrl || io.KeySuper;
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
            for (u32 id : selected_) graph_.remove(id);
            selected_.clear();
            graph_changed();
        }
        if (cmd && ImGui::IsKeyPressed(ImGuiKey_D)) {
            std::set<u32> made;
            for (u32 id : selected_)
                if (const sg::Node* n = graph_.node(id); n && n->type != "output") {
                    sg::Node copy = *n;
                    copy.id = graph_.next_id++;
                    copy.position += Vec2{30, 30};
                    graph_.nodes.push_back(copy);
                    made.insert(copy.id);
                }
            selected_ = made;
            graph_changed();
        }
        if (cmd && ImGui::IsKeyPressed(ImGuiKey_A))
            for (const sg::Node& n : graph_.nodes) selected_.insert(n.id);
        if (ImGui::IsKeyPressed(ImGuiKey_F) || ImGui::IsKeyPressed(ImGuiKey_Home)) graph_fit_ = true;
    }
    // The bar across the top: its name, saving, undo.
    ImGui::SetCursorScreenPos({area.x + 10, area.y + 8});
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::color::accent), "%s", fs::path(path_).filename().string().c_str());
    if (unsaved()) {
        ImGui::SameLine(0, 4);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::color::accent), "*");
    }
    ImGui::SetCursorScreenPos({area.right() - 100, area.y + 6});
    if (ui::tool_button("undo_graph", ui::Icon::Undo, false, "Undo|Ctrl+Z", 26, graph_cursor_ > 0)) undo(-1);
    ImGui::SameLine(0, 2);
    if (ui::tool_button("redo_graph", ui::Icon::Redo, false, "Redo|Ctrl+Shift+Z", 26, graph_cursor_ + 1 < graph_history_.size())) undo(1);
    ImGui::SameLine(0, 2);
    if (ui::tool_button("save_graph", ui::Icon::Save, false, "Save the graph and the shader it makes|Ctrl+S", 26)) save_graph();
    ImGui::End();
    (void)engine;
}

void MaterialsWorkspace::graph_node_panel(Engine&) {
    if (selected_.size() != 1) {
        if (ui::begin_card("Node", "node")) {
            ui::hint(selected_.empty() ? "Pick a node to change its settings and the values of inputs nothing's joined to."
                                       : "Several nodes are picked: Delete removes them, Ctrl+D copies them.");
            ui::end_card();
        }
        return;
    }
    sg::Node* n = graph_.node(*selected_.begin());
    if (!n) return;
    const sg::NodeType& t = *sg::node_type(n->type);
    if (!ui::begin_card(t.title.c_str(), "node")) return;
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(muted, "%s", t.help.c_str());
    ImGui::PopTextWrapPos();
    bool changed = false;
    for (const sg::Setting& s : t.settings) {
        ImGui::PushID(s.key.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(s.label.c_str());
        if (!s.help.empty()) ui::tip(s.help);
        ImGui::SameLine(124);
        ImGui::SetNextItemWidth(-1);
        switch (s.kind) {
        case sg::Setting::Kind::Number: {
            sg::Values v = graph_.number(*n, s.key);
            bool c = s.size == 1 && s.max > s.min ? ImGui::SliderFloat("##v", v.data(), s.min, s.max, "%.3f")
                                                  : ImGui::DragScalarN("##v", ImGuiDataType_Float, v.data(), s.size, 0.01f, nullptr, nullptr, "%.3f");
            if (c) n->numbers[s.key] = v, changed = true;
            break;
        }
        case sg::Setting::Kind::Color: {
            sg::Values v = graph_.number(*n, s.key);
            if (ImGui::ColorEdit4("##v", v.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_AlphaBar))
                n->numbers[s.key] = v, changed = true;
            break;
        }
        case sg::Setting::Kind::Text: {
            char buf[128];
            std::snprintf(buf, sizeof buf, "%s", graph_.text(*n, s.key).c_str());
            if (ImGui::InputText("##v", buf, sizeof buf)) n->texts[s.key] = buf, changed = true;
            break;
        }
        case sg::Setting::Kind::Choice: {
            std::string current = graph_.text(*n, s.key);
            if (ImGui::BeginCombo("##v", current.c_str())) {
                for (const std::string& o : s.options)
                    if (ImGui::Selectable(o.c_str(), o == current)) n->texts[s.key] = o, changed = true;
                ImGui::EndCombo();
            }
            break;
        }
        }
        ImGui::PopID();
    }
    // Inputs nothing's joined to take a value of their own.
    for (u32 i = 0; i < t.inputs.size(); ++i) {
        const sg::Pin& p = t.inputs[i];
        if (graph_.link_into(n->id, i)) continue;
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(p.name.c_str());
        ImGui::SameLine(124);
        if (p.uv) {
            ImGui::TextColored(muted, "the sprite's UV");
        } else {
            std::string key = "in:" + p.name;
            sg::Values v = graph_.number(*n, key);
            ImGui::SetNextItemWidth(-1);
            bool c = p.color ? (p.size == 4 ? ImGui::ColorEdit4("##in", v.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_AlphaBar)
                                            : ImGui::ColorEdit3("##in", v.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR))
                             : ImGui::DragScalarN("##in", ImGuiDataType_Float, v.data(), p.size, 0.01f, nullptr, nullptr, "%.3f");
            if (c) n->numbers[key] = v, changed = true;
        }
        ImGui::PopID();
    }
    if (changed) graph_changed();
    ui::end_card();
}

} // namespace bowl::editor
