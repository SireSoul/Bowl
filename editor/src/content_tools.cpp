// The Content workspace's tools: finding anything (Ctrl+K), where a record is used and renaming it everywhere, moving
// list items, and the bars between panels.

#include "content.hpp"

#include "ui.hpp"

#include <algorithm>
#include <cctype>
#include <format>

namespace bowl::editor {

namespace fs = std::filesystem;
using json::Value;

namespace {

const ImVec4 muted = ImGui::ColorConvertU32ToFloat4(ui::color::muted);

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// How well `query` matches `text`: its letters in order (not necessarily together), better at the start, together,
// and in shorter text. -1: no match.
int match_score(std::string_view text, std::string_view query) {
    if (query.empty()) return 1;
    std::string t = lower(text), q = lower(query);
    if (t.starts_with(q)) return 1000 - static_cast<int>(t.size());
    if (usize at = t.find(q); at != std::string::npos) return 800 - static_cast<int>(at) - static_cast<int>(t.size());
    int score = 500, last = -1;
    usize ti = 0;
    for (char c : q) {
        while (ti < t.size() && t[ti] != c) ++ti;
        if (ti == t.size()) return -1;
        score -= last >= 0 ? static_cast<int>(ti) - last - 1 : static_cast<int>(ti);
        last = static_cast<int>(ti++);
    }
    return score - static_cast<int>(t.size()) / 4;
}

} // namespace

std::string ContentWorkspace::path_key(const std::string& doc, const Path& path) {
    std::string key = doc;
    for (const auto& step : path) {
        key += '/';
        if (const auto* s = std::get_if<std::string>(&step)) key += *s;
        else key += std::to_string(std::get<usize>(step));
    }
    return key;
}

void ContentWorkspace::walk(const schema::Field& raw, const Value* v, const Path& path, const Value* parent, const Visit& fn) {
    schema::Field f = schema_.resolve(raw);
    fn(f, v, path, parent);
    if (!v) return;
    if (f.type == "list" && f.of && v->is_array())
        for (usize i = 0; i < v->size(); ++i) {
            Path p = path;
            p.emplace_back(i);
            walk(*f.of, &(*v)[i], p, v, fn);
        }
    if (const schema::Type* t = schema_.type(f.type); t && !t->base && v->is_object()) {
        auto fields = [&](const std::vector<schema::Field>& list) {
            for (const schema::Field& sub : list) {
                if (sub.type == "variant") continue;
                Path p = path;
                p.emplace_back(sub.key);
                walk(sub, v->find(sub.key), p, v, fn);
            }
        };
        fields(t->fields);
        if (!t->tag.empty())
            if (const schema::Variant* var = t->variant((*v)[t->tag].as_string())) fields(var->fields);
    }
}

std::vector<ContentWorkspace::Ref> ContentWorkspace::find_refs(std::string_view collection, std::string_view id) {
    std::vector<Ref> out;
    if (id.empty()) return out;
    for (const schema::Collection& c : schema_.collections) {
        if (c.id == "bowl.maps") continue; // below
        usize n = record_count(c);
        for (usize i = 0; i < n; ++i) {
            std::string doc_path;
            Path path;
            const Value* r = record(c, i, &doc_path, &path);
            if (!r) continue;
            std::string label;
            walk(schema::Field{.type = c.type}, r, path, nullptr, [&](const schema::Field& f, const Value* v, const Path& p, const Value*) {
                if (f.type != "ref" || f.to != collection || !v || v->as_string() != id) return;
                if (label.empty()) label = record_label(c, i);
                out.push_back({doc_path, p, c.id, i, std::format("{}: {} · {}", c.title, label, f.title())});
            });
        }
    }
    // Map markers' properties.
    if (const schema::Collection* maps = schema_.collection("bowl.maps")) {
        usize n = record_count(*maps);
        for (usize i = 0; i < n; ++i) {
            std::string file = record_id(*maps, i);
            Doc* d = doc(file);
            if (!d || !d->text.valid()) continue;
            const Value& layers = d->text.value()["layers"];
            for (usize li = 0; li < layers.size(); ++li) {
                if (layers[li]["name"].as_string() != "markers") continue;
                const Value& objects = layers[li]["objects"];
                for (usize oi = 0; oi < objects.size(); ++oi) {
                    const schema::MarkerType* t = schema_.marker(objects[oi]["type"].as_string());
                    if (!t) continue;
                    const Value& props = objects[oi]["properties"];
                    for (usize pi = 0; pi < props.size(); ++pi)
                        for (const schema::Field& raw : t->fields) {
                            schema::Field f = schema_.resolve(raw);
                            if (f.type != "ref" || f.to != collection || props[pi]["name"].as_string() != f.key ||
                                props[pi]["value"].as_string() != id)
                                continue;
                            out.push_back({file, Path{"layers", li, "objects", oi, "properties", pi, "value"}, maps->id, i,
                                           std::format("Map {}: {} marker", fs::path(file).stem().string(), t->label)});
                        }
                }
            }
        }
    }
    return out;
}

usize ContentWorkspace::rename_id(const schema::Collection& c, usize index, const std::string& from, const std::string& to) {
    std::string doc_path;
    Path path;
    if (!record(c, index, &doc_path, &path) || from == to) return 0;
    std::vector<Ref> refs = find_refs(c.id, from);
    for (const Ref& r : refs) edits_.push_back({Edit::Set, r.doc, r.path, Value(to), {}});
    path.emplace_back(c.id_key);
    edits_.push_back({Edit::Set, doc_path, path, Value(to), {}});
    return refs.size();
}

void ContentWorkspace::select(usize collection, usize record) {
    if (collection >= schema_.collections.size()) return;
    collection_ = collection;
    record_ = record;
    filter_[0] = 0;
}

void ContentWorkspace::used_by(const schema::Collection& c, usize index) {
    if (c.files() || c.list.empty()) return;
    std::vector<Ref> refs = find_refs(c.id, record_id(c, index));
    if (refs.empty()) {
        ImGui::TextColored(muted, "Not used anywhere yet");
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    bool open = ImGui::TreeNodeEx("##usedby", ImGuiTreeNodeFlags_SpanTextWidth, "Used in %zu place%s", refs.size(),
                                  refs.size() == 1 ? "" : "s");
    ImGui::PopStyleColor();
    ui::tip("Everything that names this. Changing its id changes them all.");
    if (!open) return;
    for (usize i = 0; i < refs.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(refs[i].where.c_str()))
            for (usize k = 0; k < schema_.collections.size(); ++k)
                if (schema_.collections[k].id == refs[i].collection) select(k, refs[i].record);
        ImGui::PopID();
    }
    ImGui::TreePop();
}

void ContentWorkspace::move_item(const std::string& doc_path, const Path& path, const Value& list, usize from, usize to) {
    if (from == to || from >= list.size() || to >= list.size()) return;
    auto at = [&](usize i) {
        Path p = path;
        p.emplace_back(i);
        return p;
    };
    if (from < to)
        for (usize k = from; k < to; ++k) edits_.push_back({Edit::Set, doc_path, at(k), list[k + 1], {}});
    else
        for (usize k = from; k > to; --k) edits_.push_back({Edit::Set, doc_path, at(k), list[k - 1], {}});
    edits_.push_back({Edit::Set, doc_path, at(to), list[from], {}});
}

void ContentWorkspace::collection_icon(const schema::Collection& c, ImVec2 center, f32 size, ImU32 color) {
    std::string_view k = c.icon;
    if (k.empty()) k = c.id == "bowl.maps" ? "map" : c.id == "bowl.sounds" ? "sound" : "";
    ui::Icon icon = k == "person"     ? ui::Icon::Person
                    : k == "item"     ? ui::Icon::Bag
                    : k == "shop"     ? ui::Icon::Shop
                    : k == "event"    ? ui::Icon::Bolt
                    : k == "map"      ? ui::Icon::Map
                    : k == "area"     ? ui::Icon::Region
                    : k == "sound"    ? ui::Icon::Note
                    : k == "settings" ? ui::Icon::Gear
                                      : ui::Icon::Folder;
    ui::draw_icon(ImGui::GetWindowDrawList(), icon, center, size, color);
}

void ContentWorkspace::splitter(const char* id, Rect bar, f32& value, f32 lo, f32 hi, f32 sign) {
    ImGui::SetNextWindowPos({bar.x, bar.y});
    ImGui::SetNextWindowSize({bar.w, bar.h});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    ImGui::InvisibleButton("##bar", {bar.w, bar.h});
    bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    f32 x = bar.x + bar.w * 0.5f;
    dl->AddLine({x, bar.y}, {x, bar.y + bar.h}, hot ? ui::color::accent : ui::color::line, hot ? 2.0f : 1.0f);
    if (ImGui::IsItemActive()) value = std::clamp(value + ImGui::GetIO().MouseDelta.x * sign, lo, hi);
    if (ImGui::IsItemDeactivated() && engine_) { // remembered for next time
        engine_->settings().set("editor.content.left_w", left_w_);
        engine_->settings().set("editor.content.side_w", side_w_);
    }
    if (ImGui::IsItemHovered()) ui::tip("Drag to resize");
    ImGui::End();
}

void ContentWorkspace::palette(Engine& engine) {
    if (!palette_open_) return;
    struct Entry {
        std::string label, detail;
        const schema::Collection* collection = nullptr; // for its icon
        std::function<void()> go;
        int score = 0;
    };
    std::vector<Entry> all;
    // Commands first, then every record of every collection.
    all.push_back({"Save all", "Ctrl+S", nullptr, [this] { save_all(); }});
    if (!schema_.play.empty()) all.push_back({"Play", "a new game", nullptr, [this] { save_all(); play_game({}); }});
    all.push_back({"Look for new files", "F5", nullptr, [this] { scan(); }});
    if (hooks.go) {
        all.push_back({"Paint", "workspace", nullptr, [this] { hooks.go(1); }});
        all.push_back({"Levels", "workspace", nullptr, [this] { hooks.go(2); }});
        all.push_back({"Files as text", "workspace: every data file as JSON", nullptr, [this] { hooks.go(3); }});
        all.push_back({"Materials", "workspace: materials and shaders", nullptr, [this] { hooks.go(5); }});
    }
    if (hooks.looks && hooks.open_look)
        for (const std::string& p : hooks.looks()) {
            std::string kind = p.ends_with(".material") ? "material: " : p.ends_with(".shadergraph") ? "shader graph: " : "shader: ";
            all.push_back({fs::path(p).filename().string(), kind + p, nullptr, [this, p] { hooks.open_look(p); }});
        }
    if (hooks.pictures && hooks.open_picture)
        for (const std::string& p : hooks.pictures()) all.push_back({fs::path(p).filename().string(), "picture: " + p, nullptr, [this, p] { hooks.open_picture(p); }});
    for (usize ci = 0; ci < schema_.collections.size(); ++ci) {
        const schema::Collection& c = schema_.collections[ci];
        all.push_back({c.title, "collection", &c, [this, ci] { select(ci, 0); }});
        if (!c.files() && !c.list.empty())
            all.push_back({"New: " + c.title, "add one", &c, [this, ci] {
                               select(ci, record_);
                               new_record(schema_.collections[ci]);
                           }});
        usize n = record_count(c);
        for (usize r = 0; r < n; ++r)
            all.push_back({record_label(c, r), c.title, &c, [this, ci, r] {
                               select(ci, r);
                               if (hooks.go) hooks.go(0);
                           }});
    }
    for (Entry& e : all) e.score = std::max(match_score(e.label, palette_query_), match_score(e.detail + " " + e.label, palette_query_) - 200);
    std::erase_if(all, [](const Entry& e) { return e.score < 0; });
    std::stable_sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) { return a.score > b.score; });
    if (all.size() > 14) all.resize(14);
    if (palette_pick_ >= all.size()) palette_pick_ = all.empty() ? 0 : all.size() - 1;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // Everything else dims while it's open.
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 110));
    ImGui::Begin("##palette_dim", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::SetNextWindowFocus();
    ImGui::SetNextWindowPos({vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + 70}, ImGuiCond_Always, {0.5f, 0});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(30, 34, 43, 255));
    ImGui::SetNextWindowSize({600, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
    ImGui::Begin("##palette", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImVec2 at = ImGui::GetCursorScreenPos();
    ui::draw_icon(ImGui::GetWindowDrawList(), ui::Icon::Search, {at.x + 12, at.y + ImGui::GetFrameHeight() * 0.5f}, 14, ui::color::muted);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 26);
    ImGui::SetNextItemWidth(-1);
    char buf[128];
    std::snprintf(buf, sizeof buf, "%s", palette_query_.c_str());
    if (ImGui::InputTextWithHint("##q", "Find anything: an item, a person, an event, a command", buf, sizeof buf)) {
        palette_query_ = buf;
        palette_pick_ = 0;
    }
    if (!ImGui::IsItemActive() && !ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1); // keep typing there
    bool run = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && palette_pick_ + 1 < all.size()) ++palette_pick_;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && palette_pick_ > 0) --palette_pick_;
    bool close = ImGui::IsKeyPressed(ImGuiKey_Escape);
    ImGui::Spacing();
    for (usize i = 0; i < all.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool chosen = ImGui::Selectable("##e", i == palette_pick_, 0, {0, 26});
        if (ImGui::IsItemHovered() && ImGui::GetIO().MouseDelta.x + ImGui::GetIO().MouseDelta.y != 0) palette_pick_ = i;
        if (all[i].collection) collection_icon(*all[i].collection, {p.x + 12, p.y + 13}, 15, ui::color::text);
        else ui::draw_icon(ImGui::GetWindowDrawList(), ui::Icon::Play, {p.x + 12, p.y + 13}, 10, ui::color::accent);
        ImGui::GetWindowDrawList()->AddText({p.x + 30, p.y + 4}, ui::color::text, all[i].label.c_str());
        ImVec2 dsz = ImGui::CalcTextSize(all[i].detail.c_str());
        ImGui::GetWindowDrawList()->AddText({p.x + ImGui::GetContentRegionAvail().x - dsz.x - 6, p.y + 4}, ui::color::muted,
                                            all[i].detail.c_str());
        if (chosen) {
            palette_pick_ = i;
            run = true;
        }
        ImGui::PopID();
    }
    if (all.empty()) ImGui::TextColored(muted, "Nothing matches.");
    ImGui::TextColored(muted, "Up and down pick, Enter goes, Esc closes");
    bool outside = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    ImGui::End();
    if (run && palette_pick_ < all.size()) {
        auto go = all[palette_pick_].go;
        palette_open_ = false;
        go();
    }
    if (close || outside) palette_open_ = false;
    (void)engine;
}

} // namespace bowl::editor
