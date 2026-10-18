// The Content workspace's maps: drawing a map in a window (for picking a spot, and for the Maps view), placing and
// changing the markers on a map's "markers" layer, and starting the game.

#include "content.hpp"

#include "ui.hpp"

#include "bowl/core/file.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace bowl::editor {

namespace fs = std::filesystem;
using json::Value;

namespace {

ImTextureID texture_id(const gfx::Texture& t) { return static_cast<ImTextureID>(reinterpret_cast<intptr_t>(t.handle())); }

const ImVec4 danger = ImGui::ColorConvertU32ToFloat4(ui::color::danger);

// Map pixels per cell for object positions: isometric maps measure both axes in tile heights.
Vec2 per_cell(const Value& map) {
    f32 tw = map["tilewidth"].as_float(32), th = map["tileheight"].as_float(16);
    return {map["orientation"].as_string() == "isometric" ? th : tw, th};
}

// The index of the "markers" object layer in the map's layers, or -1.
i64 markers_layer(const Value& map) {
    const Value& layers = map["layers"];
    for (usize i = 0; i < layers.size(); ++i)
        if (layers[i]["name"].as_string() == "markers" && layers[i]["type"].as_string() == "objectgroup") return static_cast<i64>(i);
    return -1;
}

bool is_point(const Value& o) { return o["point"].as_bool() || (o["width"].as_number() == 0 && o["height"].as_number() == 0); }

// Where an object is, in cells: a point's position, or an area's rectangle.
Rect cells_of(const Value& o, Vec2 per) {
    return {o["x"].as_float() / per.x, o["y"].as_float() / per.y, o["width"].as_float() / per.x, o["height"].as_float() / per.y};
}

// Tiled's custom properties ([{name, type, value}]) as one object.
Value properties_of(const Value& o) {
    Value out;
    out.make_object();
    for (const Value& p : o["properties"].array()) out.set(p["name"].as_string(), p["value"]);
    return out;
}

f64 round2(f64 v) { return std::round(v * 100) / 100; }

} // namespace

// --- Drawing a map ---------------------------------------------------------------------------------------------------

void ContentWorkspace::cell_outline(const gfx::Tilemap& m, const MapView& view, Rect r, ImU32 color, f32 thick) {
    ImVec2 pts[4] = {view.to_screen(m.cell_to_world(r.x, r.y)), view.to_screen(m.cell_to_world(r.right(), r.y)),
                     view.to_screen(m.cell_to_world(r.right(), r.bottom())), view.to_screen(m.cell_to_world(r.x, r.bottom()))};
    ImGui::GetWindowDrawList()->AddPolyline(pts, 4, color, ImDrawFlags_Closed, thick);
}

void ContentWorkspace::map_canvas(const char* id, const gfx::Tilemap& m, MapView& view, f32 bottom) {
    view.origin = ImGui::GetCursorScreenPos();
    view.size = ImGui::GetContentRegionAvail();
    view.size.y = std::max(50.0f, view.size.y - bottom);
    ImGui::InvisibleButton(id, view.size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    view.hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();

    // The map's extent in map pixels: its cells' corners.
    f32 W = static_cast<f32>(m.width), H = static_cast<f32>(m.height);
    Vec2 corners[] = {m.cell_to_world(0, 0), m.cell_to_world(W, 0), m.cell_to_world(0, H), m.cell_to_world(W, H)};
    Vec2 lo = corners[0], hi = corners[0];
    for (Vec2 c : corners) {
        lo = {std::min(lo.x, c.x), std::min(lo.y, c.y)};
        hi = {std::max(hi.x, c.x), std::max(hi.y, c.y)};
    }
    if (view.zoom <= 0) { // first time: all of it
        view.zoom = std::min(view.size.x / (hi.x - lo.x + 64), view.size.y / (hi.y - lo.y + 64));
        view.pan = (lo + hi) * 0.5f;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (view.hovered && io.MouseWheel != 0) {
        Vec2 before = view.to_world(io.MousePos);
        view.zoom = std::clamp(view.zoom * std::pow(1.15f, io.MouseWheel), 0.1f, 20.0f);
        view.pan += before - view.to_world(io.MousePos);
    }
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
        view.pan -= Vec2{io.MouseDelta.x, io.MouseDelta.y} / view.zoom;
    view.mouse_cell = m.world_to_cell(view.to_world(io.MousePos));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 end{view.origin.x + view.size.x, view.origin.y + view.size.y};
    dl->PushClipRect(view.origin, end, true);
    dl->AddRectFilled(view.origin, end, IM_COL32(16, 18, 24, 255));
    bool iso = m.orientation == gfx::Tilemap::Orientation::Isometric;
    // Tile layers in order, each tile with its bottom on its cell, the way Tiled draws them.
    for (const gfx::MapLayer& layer : m.layers) {
        if (layer.kind != gfx::MapLayer::Kind::Tiles || !layer.visible) continue;
        ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(layer.opacity * 255));
        for (i32 y = 0; y < static_cast<i32>(m.height); ++y)
            for (i32 x = 0; x < static_cast<i32>(m.width); ++x) {
                u32 gid = layer.at(x, y) & gfx::tile_gid_mask;
                if (!gid) continue;
                const gfx::Tileset* ts = m.tileset_for(gid);
                if (!ts || !ts->image) continue;
                Rect src = ts->source_rect(gid - ts->first_gid);
                Vec2 a = m.cell_to_world(static_cast<f32>(x), static_cast<f32>(y));
                f32 left = iso ? a.x - m.tile_width * 0.5f : a.x, base = a.y + m.tile_height;
                Vec2 p0{left + ts->tile_offset.x, base - src.h + ts->tile_offset.y};
                f32 iw = static_cast<f32>(ts->image->width()), ih = static_cast<f32>(ts->image->height());
                dl->AddImage(texture_id(*ts->image), view.to_screen(p0), view.to_screen(p0 + Vec2{src.w, src.h}),
                             {src.x / iw, src.y / ih}, {src.right() / iw, src.bottom() / ih}, tint);
            }
    }
    // Things on the other object layers (trees, rocks): dots, so there's something to place markers by.
    f32 per_x = static_cast<f32>(iso ? m.tile_height : m.tile_width), per_y = static_cast<f32>(m.tile_height);
    for (const gfx::MapLayer& layer : m.layers) {
        if (layer.kind != gfx::MapLayer::Kind::Objects || layer.name == "markers") continue;
        for (const gfx::MapObject& o : layer.objects) {
            ImVec2 p = view.to_screen(m.cell_to_world(o.bounds.x / per_x, o.bounds.y / per_y));
            dl->AddCircleFilled(p, std::max(2.0f, view.zoom * 2), IM_COL32(20, 60, 30, 200));
        }
    }
    dl->PopClipRect();
}

void ContentWorkspace::map_spot_window(Engine& engine) {
    if (!spot_) return;
    bool open = true;
    ImGui::SetNextWindowSize({760, 600}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    std::string title = std::format("{}: click a spot###spot", spot_->map);
    if (!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                                ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        if (!open) spot_.reset();
        return;
    }
    MapInfo* info = map_info(spot_->map, spot_->folder);
    if (!info || !info->map) {
        ImGui::TextColored(danger, "Can't open %s", map_path(spot_->map, spot_->folder).c_str());
        ImGui::End();
        if (!open) spot_.reset();
        return;
    }
    const gfx::Tilemap& m = *info->map;
    ui::hint("Click to put it in the middle of a cell, Alt-click for exactly there. The wheel zooms, right-drag moves.");
    MapView view{spot_->zoom, spot_->pan};
    map_canvas("canvas", m, view, 30);
    spot_->zoom = view.zoom;
    spot_->pan = view.pan;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(view.origin, {view.origin.x + view.size.x, view.origin.y + view.size.y}, true);
    for (const auto& [name, r] : info->markers) {
        cell_outline(m, view, {r.x, r.y, std::max(r.w, 0.2f), std::max(r.h, 0.2f)}, IM_COL32(90, 200, 200, 200), 1.5f);
        dl->AddText(view.to_screen(m.cell_to_world(r.x, r.y)), IM_COL32(150, 230, 230, 255), name.c_str());
    }
    for (const auto& [name, at] : info->spawns) {
        ImVec2 p = view.to_screen(m.cell_to_world(at.x, at.y));
        dl->AddCircleFilled(p, 4, IM_COL32(120, 200, 255, 255));
        dl->AddText({p.x + 6, p.y - 7}, IM_COL32(200, 230, 255, 255), name.c_str());
    }
    Vec2 under = view.mouse_cell;
    if (view.hovered) {
        cell_outline(m, view, {std::floor(under.x), std::floor(under.y), 1, 1}, ui::color::accent, 2);
        ImGui::SetTooltip("%.2f, %.2f", under.x, under.y);
    }
    ImVec2 now = view.to_screen(m.cell_to_world(spot_->cell.x, spot_->cell.y));
    dl->AddCircleFilled(now, 6, ui::color::accent);
    dl->AddCircle(now, 6, IM_COL32(0, 0, 0, 255), 0, 2);
    dl->PopClipRect();

    if (view.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        Vec2 cell = ImGui::GetIO().KeyAlt ? Vec2{std::round(under.x * 100) / 100, std::round(under.y * 100) / 100}
                                          : Vec2{std::floor(under.x) + 0.5f, std::floor(under.y) + 0.5f};
        spot_->cell = cell;
        if (Doc* d = doc(spot_->doc)) {
            Value v = json::Array{Value(cell.x), Value(cell.y)};
            if (spot_->exists) {
                d->text.set(spot_->path, v);
            } else {
                Path parent = spot_->path;
                std::string key = std::get<std::string>(parent.back());
                parent.pop_back();
                d->text.insert(parent, v, key);
                spot_->exists = true;
            }
        }
    }
    ImGui::Text("Spot: %.2f, %.2f", spot_->cell.x, spot_->cell.y);
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 60);
    if (ImGui::Button("Done")) open = false;
    ImGui::End();
    if (!open) spot_.reset();
    (void)engine;
}

// --- The Maps view: markers ------------------------------------------------------------------------------------------

std::vector<ContentWorkspace::Problem> ContentWorkspace::marker_problems(const std::string& file, usize record) {
    std::vector<Problem> out;
    Doc* d = doc(file);
    const schema::Collection* maps = schema_.collection("bowl.maps");
    if (!d || !d->text.valid() || !maps) return out;
    const Value& map = d->text.value();
    i64 li = markers_layer(map);
    if (li < 0) return out;
    std::string name = fs::path(file).stem().string();
    std::set<std::string> spawns;
    for (const Value& o : map["layers"][static_cast<usize>(li)]["objects"].array()) {
        std::string type(o["type"].as_string()), label(o["name"].as_string());
        std::string where = std::format("{}: {} {}", name, type.empty() ? "marker" : type,
                                        label.empty() ? std::format("#{}", o["id"].as_int()) : "\"" + label + "\"");
        if (type == "spawn" && !spawns.insert(label).second)
            out.push_back({maps->id, record, where + ": another spawn point has this name"});
        const schema::MarkerType* t = schema_.marker(type);
        if (!t) {
            if (!type.empty()) out.push_back({maps->id, record, where + ": the schema has no kind of marker called that"});
            continue;
        }
        Value props = properties_of(o);
        for (const schema::Field& f : t->fields) check(f, props.find(f.key), where, *maps, record, out, &props);
    }
    return out;
}

void ContentWorkspace::maps_view(Engine& engine, Rect area, const std::string& file) {
    constexpr ImGuiWindowFlags fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar;
    ImGui::SetNextWindowPos({area.x, area.y});
    ImGui::SetNextWindowSize({area.w, area.h});
    ImGui::Begin("##content_maps", nullptr, fixed);
    if (markers_.file != file) markers_ = {file};
    std::string name = fs::path(file).stem().string(), folder = fs::path(file).parent_path().generic_string();
    Doc* d = file.empty() ? nullptr : doc(file);
    MapInfo* info = d ? map_info(name, folder) : nullptr;
    if (!d || !info || !info->map || !d->text.valid() || !d->text.value().is_object()) {
        ui::hint(file.empty() ? "No maps in the assets folder." : "Can't read " + file + ".");
        ImGui::End();
        return;
    }
    history_doc_ = file;
    const Value& map = d->text.value();
    const gfx::Tilemap& m = *info->map;
    Vec2 per = per_cell(map);
    i64 li = markers_layer(map);

    // The toolbar: what's open, saving, playing, adding a marker.
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted(name.c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::BeginDisabled(!unsaved());
    if (ImGui::SmallButton("Save")) save_all();
    ImGui::EndDisabled();
    ui::tip("Saves the map (and any other changed content); a running game picks it up", "Ctrl+S");
    ImGui::SameLine();
    ImGui::BeginDisabled(schema_.play.empty());
    if (ImGui::SmallButton("Play here")) {
        save_all();
        play_game(name);
    }
    ImGui::EndDisabled();
    ui::tip(schema_.play.empty() ? "Add \"play\" to the game's schema to start the game from here"
                                 : "Saves, then starts the game in this map");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", file.c_str());

    if (li < 0) {
        ui::hint("This map has no \"markers\" object layer yet.");
        if (ImGui::Button("Add one")) {
            i64 id = map["nextlayerid"].as_int(static_cast<i64>(map["layers"].size()) + 1);
            Value layer = json::Object{{"id", id},         {"name", "markers"},      {"type", "objectgroup"},
                                       {"objects", json::Array{}}, {"visible", true}, {"opacity", 1},
                                       {"x", 0},           {"y", 0},                 {"draworder", "topdown"}};
            edits_.push_back({Edit::Insert, file, {"layers"}, layer, {}});
            edits_.push_back({map.contains("nextlayerid") ? Edit::Set : Edit::Insert, file,
                              map.contains("nextlayerid") ? Path{"nextlayerid"} : Path{}, Value(id + 1), "nextlayerid"});
        }
        ImGui::End();
        return;
    }
    const Value& objects = map["layers"][static_cast<usize>(li)]["objects"];
    Path objects_path{"layers", static_cast<usize>(li), "objects"};
    if (markers_.selected >= static_cast<i64>(objects.size())) markers_.selected = -1;

    ImGui::SameLine(0, 24);
    if (ImGui::Button(markers_.placing ? "Click the map..." : "+ Add a marker")) ImGui::OpenPopup("add");
    ui::tip("Pick a kind, then click where it goes");
    if (ImGui::BeginPopup("add")) {
        for (const schema::MarkerType& t : schema_.markers)
            if (ImGui::Selectable(t.label.c_str())) {
                markers_.adding = t.name;
                markers_.placing = true;
            }
        if (!schema_.markers.empty()) ImGui::Separator();
        if (ImGui::Selectable("Named area (for events)")) {
            markers_.adding.clear();
            markers_.placing = true;
        }
        ui::tip("An area with only a name: events start on stepping into it (\"Walking into a marker\") or using it");
        ImGui::EndPopup();
    }
    if (markers_.placing && ImGui::IsKeyPressed(ImGuiKey_Escape)) markers_.placing = false;

    // The map on the left, the markers and the one picked on the right.
    constexpr float side = 340;
    ImGui::BeginChild("canvas", {ImGui::GetContentRegionAvail().x - side - 8, 0});
    ui::hint(markers_.placing ? "Click where it goes. Esc: never mind."
                              : "Click a marker to pick it, drag it to move it (Alt: off the grid). Wheel zooms, right-drag moves.");
    map_canvas("map", m, markers_.view, 0);
    MapView& view = markers_.view;
    ImGuiIO& io = ImGui::GetIO();
    Vec2 mouse = view.mouse_cell;
    auto snapped = [&](Vec2 p, bool area_marker) {
        if (io.KeyAlt) return Vec2{static_cast<f32>(round2(p.x)), static_cast<f32>(round2(p.y))};
        return area_marker ? Vec2{std::round(p.x), std::round(p.y)} : Vec2{std::floor(p.x) + 0.5f, std::floor(p.y) + 0.5f};
    };
    auto area_kind = [&](std::string_view type) {
        const schema::MarkerType* t = schema_.marker(type);
        return t ? t->area : type.empty();
    };

    // Picking, dragging and placing.
    if (view.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (markers_.placing) {
            bool is_area = area_kind(markers_.adding);
            Vec2 at = snapped(is_area ? mouse - Vec2{0.5f, 0.5f} : mouse, is_area);
            i64 id = map["nextobjectid"].as_int(1);
            json::Object o{{"id", id}, {"name", ""}, {"type", markers_.adding}, {"x", at.x * per.x}, {"y", at.y * per.y},
                           {"width", is_area ? per.x : 0}, {"height", is_area ? per.y : 0}, {"rotation", 0}, {"visible", true}};
            if (!is_area) o.emplace_back("point", true);
            // The properties its kind has, at their defaults, so they show in Tiled too.
            if (const schema::MarkerType* t = schema_.marker(markers_.adding); t && !t->fields.empty()) {
                json::Array props;
                for (const schema::Field& f : t->fields) {
                    schema::Field r = schema_.resolve(f);
                    Value v = schema::default_value(schema_, f);
                    std::string type = r.type == "int" ? "int" : r.type == "number" ? "float" : r.type == "bool" ? "bool" : "string";
                    props.push_back(json::Object{{"name", f.key}, {"type", type}, {"value", v}});
                }
                o.emplace_back("properties", std::move(props));
            }
            edits_.push_back({Edit::Insert, file, objects_path, Value(std::move(o)), {}});
            edits_.push_back({Edit::Set, file, {"nextobjectid"}, Value(id + 1), {}});
            markers_.selected = static_cast<i64>(objects.size());
            markers_.placing = false;
        } else {
            markers_.selected = -1;
            for (usize i = objects.size(); i-- > 0;) {
                const Value& o = objects[i];
                Rect r = cells_of(o, per);
                bool hit = is_point(o) ? (mouse - r.position()).length() < 0.45f : r.contains(mouse);
                if (!hit) continue;
                markers_.selected = static_cast<i64>(i);
                markers_.dragging = true;
                markers_.grab = mouse - r.position();
                markers_.ghost = r.position();
                break;
            }
        }
    }
    if (markers_.dragging && markers_.selected >= 0) {
        const Value& o = objects[static_cast<usize>(markers_.selected)];
        bool point = is_point(o);
        markers_.ghost = snapped(point ? mouse : mouse - markers_.grab, !point);
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            markers_.dragging = false;
            Rect r = cells_of(o, per);
            if ((markers_.ghost - r.position()).length() > 1e-3f) {
                Path p = objects_path;
                p.emplace_back(static_cast<usize>(markers_.selected));
                Path px = p, py = p;
                px.emplace_back("x");
                py.emplace_back("y");
                edits_.push_back({Edit::Set, file, px, Value(round2(markers_.ghost.x * per.x)), {}});
                edits_.push_back({Edit::Set, file, py, Value(round2(markers_.ghost.y * per.y)), {}});
            }
        }
    }
    bool typing = io.WantTextInput;
    if (markers_.selected >= 0 && !typing && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
        Path p = objects_path;
        p.emplace_back(static_cast<usize>(markers_.selected));
        edits_.push_back({Edit::Erase, file, p, {}, {}});
        markers_.selected = -1;
    }

    // The markers, over the map.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(view.origin, {view.origin.x + view.size.x, view.origin.y + view.size.y}, true);
    for (usize i = 0; i < objects.size(); ++i) {
        const Value& o = objects[i];
        bool picked = static_cast<i64>(i) == markers_.selected;
        Rect r = cells_of(o, per);
        if (picked && markers_.dragging) r = {markers_.ghost.x, markers_.ghost.y, r.w, r.h};
        std::string type(o["type"].as_string()), label(o["name"].as_string());
        std::string text = label.empty() ? type : type.empty() ? label : type + " " + label;
        ImU32 color = picked ? ui::color::accent : type == "spawn" ? IM_COL32(120, 200, 255, 255) : IM_COL32(90, 210, 200, 230);
        if (is_point(o)) {
            ImVec2 p = view.to_screen(m.cell_to_world(r.x, r.y));
            dl->AddCircleFilled(p, picked ? 6.0f : 4.5f, color);
            dl->AddCircle(p, picked ? 6.0f : 4.5f, IM_COL32(0, 0, 0, 200), 0, 1.5f);
            dl->AddText({p.x + 7, p.y - 7}, color, text.c_str());
        } else {
            cell_outline(m, view, r, color, picked ? 2.5f : 1.5f);
            dl->AddText(view.to_screen(m.cell_to_world(r.x, r.y)), color, text.c_str());
        }
    }
    if (view.hovered) {
        cell_outline(m, view, {std::floor(mouse.x), std::floor(mouse.y), 1, 1}, IM_COL32(255, 255, 255, 90), 1);
        ImGui::SetTooltip("%.2f, %.2f", mouse.x, mouse.y);
    }
    dl->PopClipRect();
    ImGui::EndChild();

    // The right: problems, every marker, and the one picked.
    ImGui::SameLine();
    ImGui::BeginChild("markers", {side, 0});
    usize record = 0;
    if (const schema::Collection* maps = schema_.collection("bowl.maps")) {
        usize n = record_count(*maps);
        for (usize i = 0; i < n; ++i)
            if (record_id(*maps, i) == file) record = i;
    }
    for (const Problem& p : marker_problems(file, record)) ImGui::TextColored(danger, "%s", p.text.c_str());
    ImGui::BeginChild("list", {0, std::min(220.0f, 8 + ImGui::GetFrameHeightWithSpacing() * static_cast<f32>(objects.size() + 1))},
                      ImGuiChildFlags_Borders);
    for (usize i = 0; i < objects.size(); ++i) {
        const Value& o = objects[i];
        std::string type(o["type"].as_string()), label(o["name"].as_string());
        std::string row = std::format("{}  {}##{}", type.empty() ? "(named)" : type, label, i);
        if (ImGui::Selectable(row.c_str(), static_cast<i64>(i) == markers_.selected)) markers_.selected = static_cast<i64>(i);
    }
    if (objects.size() == 0) ui::hint("No markers yet: + Add a marker.");
    ImGui::EndChild();

    if (markers_.selected >= 0) {
        usize i = static_cast<usize>(markers_.selected);
        const Value& o = objects[i];
        Path op = objects_path;
        op.emplace_back(i);
        auto key_slot = [&](const char* key) {
            Path p = op;
            p.emplace_back(key);
            return Slot{file, p, o.find(key), &o, folder};
        };
        ui::rule();
        // Its kind.
        std::string type(o["type"].as_string());
        const schema::MarkerType* t = schema_.marker(type);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Kind");
        ImGui::SameLine(150);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##kind", t ? t->label.c_str() : type.empty() ? "(named area)" : type.c_str())) {
            for (const schema::MarkerType& k : schema_.markers)
                if (ImGui::Selectable(k.label.c_str(), &k == t)) set(key_slot("type"), k.name);
            if (ImGui::Selectable("(named area)", type.empty())) set(key_slot("type"), std::string());
            ImGui::EndCombo();
        }
        if (t && !t->help.empty()) ui::hint(t->help);
        // Its name.
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Name");
        ui::tip("Spawn points are found by name; events start from markers by name");
        ImGui::SameLine(150);
        ImGui::PushID("name");
        value_widget(engine, schema::Field{.key = "name", .type = "text"}, key_slot("name"));
        ImGui::PopID();
        // Where it is (and how big, for an area).
        Rect r = cells_of(o, per);
        f32 xy[2] = {r.x, r.y};
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(is_point(o) ? "At" : "Corner");
        ImGui::SameLine(150);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat2("##at", xy, 0.05f, -1000, 1000, "%.2f")) {
            set(key_slot("x"), round2(xy[0] * per.x));
            set(key_slot("y"), round2(xy[1] * per.y));
        }
        if (!is_point(o)) {
            f32 wh[2] = {r.w, r.h};
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Size");
            ImGui::SameLine(150);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat2("##size", wh, 0.05f, 0.05f, 1000, "%.2f cells")) {
                set(key_slot("width"), round2(wh[0] * per.x));
                set(key_slot("height"), round2(wh[1] * per.y));
            }
        }
        // Its properties, as its kind says.
        if (t) {
            Value props = properties_of(o);
            const Value* list = o.find("properties");
            for (const schema::Field& raw : t->fields) {
                schema::Field f = schema_.resolve(raw);
                std::string tiled_type = f.type == "int" ? "int" : f.type == "number" ? "float" : f.type == "bool" ? "bool" : "string";
                Slot s{file, op, props.find(f.key), &props, folder};
                // Written as Tiled writes properties: [{name, type, value}].
                s.setter = [this, file, op, list, key = f.key, tiled_type](Value v) {
                    Path props_path = op;
                    props_path.emplace_back("properties");
                    if (list && list->is_array()) {
                        for (usize k = 0; k < list->size(); ++k)
                            if ((*list)[k]["name"].as_string() == key) {
                                Path vp = props_path;
                                vp.emplace_back(k);
                                vp.emplace_back("value");
                                edits_.push_back({Edit::Set, file, vp, std::move(v), {}});
                                return;
                            }
                        edits_.push_back({Edit::Insert, file, props_path,
                                          Value(json::Object{{"name", key}, {"type", tiled_type}, {"value", std::move(v)}}), {}});
                        return;
                    }
                    edits_.push_back({Edit::Insert, file, op,
                                      Value(json::Array{Value(json::Object{{"name", key}, {"type", tiled_type}, {"value", std::move(v)}})}),
                                      "properties"});
                };
                ImGui::PushID(f.key.c_str());
                field(engine, raw, s);
                ImGui::PopID();
            }
        }
        ImGui::Spacing();
        if (ImGui::Button("Remove it")) {
            edits_.push_back({Edit::Erase, file, op, {}, {}});
            markers_.selected = -1;
        }
        ui::tip("", "Delete");
    } else {
        ui::rule();
        ui::hint("Pick a marker on the map or in the list to change it. Tiles are painted in Tiled; everything on the "
                 "markers layer can be done here.");
    }
    ImGui::EndChild();
    ImGui::End();
}

// --- Playing ---------------------------------------------------------------------------------------------------------

void ContentWorkspace::play_game(const std::string& area) {
    if (schema_.play.empty()) return;
    fs::path exe = schema_.play_from / schema_.play;
    std::error_code ec;
    if (!fs::exists(exe, ec) && fs::exists(exe.string() + ".exe", ec)) exe += ".exe";
    if (!fs::exists(exe, ec)) {
        say_("Can't find the game at " + exe.lexically_normal().string() + ": build it first");
        return;
    }
    if (game_) { // one at a time: the last one started goes
        SDL_KillProcess(game_, false);
        SDL_DestroyProcess(game_);
        game_ = nullptr;
    }
    std::vector<std::string> args{exe.lexically_normal().string()};
    for (const std::string& a : schema_.play_args) args.push_back(a);
    if (!area.empty()) {
        args.push_back("--area");
        args.push_back(area);
    }
    std::vector<const char*> argv;
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    game_ = SDL_CreateProcess(argv.data(), false);
    say_(game_ ? "Started the game" : std::string("Couldn't start the game: ") + SDL_GetError());
}

} // namespace bowl::editor
