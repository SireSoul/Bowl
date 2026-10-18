// The Content workspace's maps, continued: drawing maps and the things standing on them, painting trees and rocks,
// exits with their way back, renaming and deleting maps with everything that names them, and the world: every map
// at once with the exits between them.

#include "content.hpp"
#include "content_util.hpp"

#include "ui.hpp"

#include "bowl/core/file.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <format>
#include <map>
#include <set>

namespace bowl::editor {

namespace fs = std::filesystem;
using json::Value;
using namespace content_util;

namespace {

const ImVec4 muted = ImGui::ColorConvertU32ToFloat4(ui::color::muted);
const ImVec4 danger = ImGui::ColorConvertU32ToFloat4(ui::color::danger);

// The index of an object layer by name, or -1.
i64 layer_named(const Value& map, std::string_view name) {
    const Value& layers = map["layers"];
    for (usize i = 0; i < layers.size(); ++i)
        if (layers[i]["name"].as_string() == name && layers[i]["type"].as_string() == "objectgroup") return static_cast<i64>(i);
    return -1;
}

// Which edge of the map a rectangle of cells is on: 0 west, 1 east, 2 north, 3 south, or -1.
int edge_of(Rect r, f32 w, f32 h) {
    if (r.x <= 0.5f) return 0;
    if (r.right() >= w - 0.5f) return 1;
    if (r.y <= 0.5f) return 2;
    if (r.bottom() >= h - 0.5f) return 3;
    return -1;
}

Value string_property(std::string_view name, std::string_view value) {
    return json::Object{{"name", name}, {"type", "string"}, {"value", value}};
}

} // namespace

// --- Drawing -------------------------------------------------------------------------------------------------------

void ContentWorkspace::draw_tiles(const gfx::Tilemap& m, const std::function<ImVec2(Vec2)>& to_screen, f32 alpha) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool iso = m.orientation == gfx::Tilemap::Orientation::Isometric;
    for (const gfx::MapLayer& layer : m.layers) {
        if (layer.kind != gfx::MapLayer::Kind::Tiles || !layer.visible) continue;
        ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(layer.opacity * alpha * 255));
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
                dl->AddImage(texture_id(*ts->image), to_screen(p0), to_screen(p0 + Vec2{src.w, src.h}), {src.x / iw, src.y / ih},
                             {src.right() / iw, src.bottom() / ih}, tint);
            }
    }
}

std::vector<ContentWorkspace::ObjectKind> ContentWorkspace::object_kinds() {
    std::vector<ObjectKind> out;
    if (!schema_.objects || schema_.objects->file.empty()) return out;
    const schema::MapObjects& mo = *schema_.objects;
    Doc* d = doc(mo.file);
    if (!d || !d->text.valid()) return out;
    for (const Value& k : d->text.value()[mo.list].array()) {
        ObjectKind kind{std::string(k[mo.name].as_string()), std::string(k[mo.picture].as_string())};
        if (k[mo.origin].is_array() && k[mo.origin].size() == 2) kind.origin = {k[mo.origin][0].as_float(), k[mo.origin][1].as_float()};
        if (!kind.name.empty()) out.push_back(std::move(kind));
    }
    return out;
}

void ContentWorkspace::draw_objects(const gfx::Tilemap& m, const Value& map, const std::function<ImVec2(Vec2)>& to_screen, f32 zoom) {
    if (!schema_.objects) return;
    i64 li = layer_named(map, schema_.objects->layer);
    if (li < 0) return;
    std::vector<ObjectKind> kinds = object_kinds();
    Vec2 per = per_cell(map);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Back to front, as the game sorts them.
    std::vector<const Value*> objects;
    for (const Value& o : map["layers"][static_cast<usize>(li)]["objects"].array()) objects.push_back(&o);
    std::sort(objects.begin(), objects.end(), [&](const Value* a, const Value* b) {
        return m.cell_to_world(cells_of(*a, per).x, cells_of(*a, per).y).y < m.cell_to_world(cells_of(*b, per).x, cells_of(*b, per).y).y;
    });
    for (const Value* o : objects) {
        Rect r = cells_of(*o, per);
        Vec2 at = m.cell_to_world(r.x, r.y);
        std::string type(o->find("type") ? (*o)["type"].as_string() : (*o)["class"].as_string());
        auto kind = std::find_if(kinds.begin(), kinds.end(), [&](const ObjectKind& k) { return k.name == type; });
        auto tex = kind != kinds.end() && !kind->picture.empty() ? assets_->load<gfx::Texture>(kind->picture) : Asset<gfx::Texture>{};
        if (!tex) {
            ImVec2 p = to_screen(at);
            dl->AddCircleFilled(p, std::max(2.0f, zoom * 2), IM_COL32(20, 60, 30, 200));
            continue;
        }
        Vec2 size{static_cast<f32>(tex->width()), static_cast<f32>(tex->height())};
        Vec2 p0 = at - Vec2{size.x * kind->origin.x, size.y * kind->origin.y};
        dl->AddImage(texture_id(*tex), to_screen(p0), to_screen(p0 + size));
    }
}

// --- Painting trees and rocks --------------------------------------------------------------------------------------

void ContentWorkspace::object_painting(const std::string& file, const Value& map, const MapView& view, const gfx::Tilemap& m) {
    if (!schema_.objects) return;
    i64 li = layer_named(map, schema_.objects->layer);
    if (li < 0) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && view.hovered) say_("The map has no \"" + schema_.objects->layer + "\" object layer");
        return;
    }
    Vec2 per = per_cell(map);
    const Value& objects = map["layers"][static_cast<usize>(li)]["objects"];
    Path objects_path{"layers", static_cast<usize>(li), "objects"};
    Vec2 cell{std::floor(view.mouse_cell.x), std::floor(view.mouse_cell.y)};
    bool inside = cell.x >= 0 && cell.y >= 0 && cell.x < static_cast<f32>(m.width) && cell.y < static_cast<f32>(m.height);
    // The object in a cell, if any.
    auto in_cell = [&](Vec2 c) -> i64 {
        for (usize i = 0; i < objects.size(); ++i) {
            Rect r = cells_of(objects[i], per);
            if (std::floor(r.x) == c.x && std::floor(r.y) == c.y) return static_cast<i64>(i);
        }
        return -1;
    };
    bool erase = ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::GetIO().KeyAlt;
    bool place = ImGui::IsMouseDown(ImGuiMouseButton_Left) && !erase;
    bool new_cell = !(cell.x == last_object_cell_.x && cell.y == last_object_cell_.y);
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) last_object_cell_ = {-1e9f, -1e9f};
    if (!view.hovered || !inside || !new_cell || (!place && !erase)) return;
    last_object_cell_ = cell;
    i64 there = in_cell(cell);
    if (erase && there >= 0) {
        Path p = objects_path;
        p.emplace_back(static_cast<usize>(there));
        edits_.push_back({Edit::Erase, file, p, {}, {}});
    } else if (place && there < 0 && !object_kind_.empty()) { // one a cell
        i64 id = map["nextobjectid"].as_int(1);
        json::Object o{{"id", id},       {"name", ""}, {"type", object_kind_}, {"point", true}, {"x", (cell.x + 0.5) * per.x},
                       {"y", (cell.y + 0.5) * per.y}, {"width", 0}, {"height", 0}, {"rotation", 0}, {"visible", true}};
        edits_.push_back({Edit::Insert, file, objects_path, Value(std::move(o)), {}});
        edits_.push_back({Edit::Set, file, {"nextobjectid"}, Value(id + 1), {}});
    }
}

// --- Exits and their way back --------------------------------------------------------------------------------------

std::string ContentWorkspace::make_way_back(const std::string& file, usize object) {
    Doc* here = doc(file);
    if (!here || !here->text.valid()) return "Can't read this map";
    const Value& map = here->text.value();
    i64 li = markers_layer(map);
    if (li < 0) return "This map has no markers layer";
    const Value& exit = map["layers"][static_cast<usize>(li)]["objects"][object];
    std::string this_name = fs::path(file).stem().string(), folder = fs::path(file).parent_path().generic_string();
    std::string to(properties_of(exit)["to_area"].as_string());
    if (to.empty()) return "Pick the map it leads to first";
    std::string there_file = map_path(to, folder);
    Doc* there = doc(there_file);
    if (!there || !there->text.valid()) return "Can't read " + there_file;
    const Value& other = there->text.value();
    i64 oli = markers_layer(other);
    if (oli < 0) return to + " has no markers layer: open it in Maps and add one";

    Vec2 per = per_cell(map), oper = per_cell(other);
    f32 w = map["width"].as_float(), h = map["height"].as_float(), ow = other["width"].as_float(), oh = other["height"].as_float();
    Rect r = cells_of(exit, per);
    int side = edge_of(r, w, h);
    if (side < 0) return "Put the exit on the map's edge first, so the way back can face it";
    // The matching place on the other map's opposite edge, the same distance along it.
    int opposite = side ^ 1;
    f32 along = side < 2 ? (r.y + r.h * 0.5f) / h : (r.x + r.w * 0.5f) / w;
    f32 span = side < 2 ? r.h : r.w;
    Rect back;
    if (opposite == 0) back = {0, along * oh - span * 0.5f, 1, span};
    if (opposite == 1) back = {ow - 1, along * oh - span * 0.5f, 1, span};
    if (opposite == 2) back = {along * ow - span * 0.5f, 0, span, 1};
    if (opposite == 3) back = {along * ow - span * 0.5f, oh - 1, span, 1};
    back.x = std::round(back.x), back.y = std::round(back.y);
    Vec2 inward[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}}; // from each edge, toward the middle
    Vec2 arrive_there = back.center() + inward[opposite] * 1.5f;
    Vec2 arrive_here = r.center() + inward[side] * 1.5f;
    std::string spawn_there = "from_" + this_name, spawn_here = "from_" + to;

    const Value& oobjects = other["layers"][static_cast<usize>(oli)]["objects"];
    Path opath{"layers", static_cast<usize>(oli), "objects"};
    i64 next = other["nextobjectid"].as_int(1);
    auto point = [&](Vec2 c, Vec2 p) {
        return json::Object{{"x", round2(c.x * p.x)}, {"y", round2(c.y * p.y)}};
    };
    // On the other map: the spawn point (moved, if it's there), and an exit home (made, if there isn't one).
    bool had_spawn = false, had_exit = false;
    for (usize i = 0; i < oobjects.size(); ++i) {
        const Value& o = oobjects[i];
        if (o["type"].as_string() == "spawn" && o["name"].as_string() == spawn_there) {
            had_spawn = true;
            for (auto& [k, v] : point(arrive_there, oper)) {
                Path p = opath;
                p.emplace_back(i), p.emplace_back(k);
                edits_.push_back({Edit::Set, there_file, p, v, {}});
            }
        }
        if (o["type"].as_string() == "exit" && properties_of(o)["to_area"].as_string() == this_name) had_exit = true;
    }
    if (!had_spawn) {
        json::Object s{{"id", next++}, {"name", spawn_there}, {"type", "spawn"}, {"point", true}};
        for (auto& [k, v] : point(arrive_there, oper)) s.emplace_back(k, v);
        s.emplace_back("width", 0), s.emplace_back("height", 0), s.emplace_back("rotation", 0), s.emplace_back("visible", true);
        edits_.push_back({Edit::Insert, there_file, opath, Value(std::move(s)), {}});
    }
    if (!had_exit) {
        json::Object e{{"id", next++}, {"name", "to " + this_name}, {"type", "exit"}, {"x", back.x * oper.x}, {"y", back.y * oper.y},
                       {"width", back.w * oper.x}, {"height", back.h * oper.y}, {"rotation", 0}, {"visible", true},
                       {"properties", json::Array{string_property("to_area", this_name), string_property("to_spawn", spawn_here)}}};
        edits_.push_back({Edit::Insert, there_file, opath, Value(std::move(e)), {}});
    }
    edits_.push_back({Edit::Set, there_file, {"nextobjectid"}, Value(next), {}});

    // On this map: the spawn point for coming back, and this exit arriving at the new spawn point there.
    const Value& objects = map["layers"][static_cast<usize>(li)]["objects"];
    Path path{"layers", static_cast<usize>(li), "objects"};
    i64 next_here = map["nextobjectid"].as_int(1);
    bool had_here = false;
    for (usize i = 0; i < objects.size(); ++i)
        if (objects[i]["type"].as_string() == "spawn" && objects[i]["name"].as_string() == spawn_here) had_here = true;
    if (!had_here) {
        json::Object s{{"id", next_here++}, {"name", spawn_here}, {"type", "spawn"}, {"point", true}};
        for (auto& [k, v] : point(arrive_here, per)) s.emplace_back(k, v);
        s.emplace_back("width", 0), s.emplace_back("height", 0), s.emplace_back("rotation", 0), s.emplace_back("visible", true);
        edits_.push_back({Edit::Insert, file, path, Value(std::move(s)), {}});
        edits_.push_back({Edit::Set, file, {"nextobjectid"}, Value(next_here), {}});
    }
    // This exit's to_spawn.
    const Value& props = exit["properties"];
    bool set = false;
    for (usize k = 0; k < props.size(); ++k)
        if (props[k]["name"].as_string() == "to_spawn") {
            Path p = path;
            p.emplace_back(object), p.emplace_back("properties"), p.emplace_back(k), p.emplace_back("value");
            edits_.push_back({Edit::Set, file, p, Value(spawn_there), {}});
            set = true;
        }
    if (!set) {
        Path p = path;
        p.emplace_back(object), p.emplace_back("properties");
        edits_.push_back({Edit::Insert, file, p, string_property("to_spawn", spawn_there), {}});
    }
    return std::format("The way back: {} now has {}{}, and this exit arrives there", to, had_exit ? "" : "an exit home and ",
                       had_spawn ? "the spawn point moved to match" : "a spawn point to arrive at");
}

// --- Renaming and deleting maps ------------------------------------------------------------------------------------

std::vector<ContentWorkspace::Ref> ContentWorkspace::map_refs(const std::string& name) {
    std::vector<Ref> out;
    // In content: every field that names a map.
    for (const schema::Collection& c : schema_.collections) {
        if (c.files()) continue;
        usize n = record_count(c);
        for (usize i = 0; i < n; ++i) {
            std::string doc_path;
            Path path;
            const Value* r = record(c, i, &doc_path, &path);
            if (!r) continue;
            walk(schema::Field{.type = c.type}, r, path, nullptr, [&](const schema::Field& f, const Value* v, const Path& p, const Value*) {
                if (f.type == "map" && v && v->as_string() == name)
                    out.push_back({doc_path, p, c.id, i, std::format("{}: {} · {}", c.title, record_label(c, i), f.title())});
            });
        }
    }
    // On maps: marker properties that name a map (exits).
    if (const schema::Collection* maps = schema_.collection("bowl.maps")) {
        usize n = record_count(*maps);
        for (usize i = 0; i < n; ++i) {
            std::string file = record_id(*maps, i);
            Doc* d = doc(file);
            if (!d || !d->text.valid()) continue;
            i64 li = markers_layer(d->text.value());
            if (li < 0) continue;
            const Value& objects = d->text.value()["layers"][static_cast<usize>(li)]["objects"];
            for (usize oi = 0; oi < objects.size(); ++oi) {
                const schema::MarkerType* t = schema_.marker(objects[oi]["type"].as_string());
                if (!t) continue;
                const Value& props = objects[oi]["properties"];
                for (usize pi = 0; pi < props.size(); ++pi)
                    for (const schema::Field& raw : t->fields)
                        if (schema_.resolve(raw).type == "map" && props[pi]["name"].as_string() == raw.key && props[pi]["value"].as_string() == name)
                            out.push_back({file, Path{"layers", static_cast<usize>(li), "objects", oi, "properties", pi, "value"}, maps->id, i,
                                           std::format("Map {}: {}", fs::path(file).stem().string(), t->label)});
            }
        }
    }
    return out;
}

bool ContentWorkspace::rename_map(const std::string& file, const std::string& to) {
    std::string from = fs::path(file).stem().string();
    fs::path target = fs::path(file).parent_path() / (to + fs::path(file).extension().string());
    std::error_code ec;
    if (to.empty() || to == from || fs::exists(assets_dir_ / target, ec)) return false;
    for (const Ref& r : map_refs(from)) edits_.push_back({Edit::Set, r.doc, r.path, Value(to), {}});
    apply_edits();
    save_all(); // the references, before the file moves
    fs::rename(assets_dir_ / file, assets_dir_ / target, ec);
    if (ec) {
        say_("Couldn't rename " + file + ": " + ec.message());
        return false;
    }
    docs_.erase(file);
    maps_.erase(from);
    scan();
    say_(std::format("Renamed {} to {}, and everything that leads there", from, to));
    return true;
}

bool ContentWorkspace::delete_map(const std::string& file) {
    std::error_code ec;
    // Into the Trash where there is one, so a mistake can be undone.
    fs::path trash;
    if (const char* home = std::getenv("HOME")) trash = fs::path(home) / ".Trash";
    fs::path from = assets_dir_ / file;
    if (!trash.empty() && fs::exists(trash, ec)) {
        auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
        fs::rename(from, trash / std::format("{} {}{}", fs::path(file).stem().string(), stamp, fs::path(file).extension().string()), ec);
    } else {
        fs::remove(from, ec);
    }
    if (ec) {
        say_("Couldn't delete " + file + ": " + ec.message());
        return false;
    }
    docs_.erase(file);
    maps_.erase(fs::path(file).stem().string());
    scan();
    say_(trash.empty() ? "Deleted " + file : "Moved " + file + " to the Trash");
    return true;
}

// --- The world -----------------------------------------------------------------------------------------------------

void ContentWorkspace::world_view(Engine& engine, Rect area) {
    const schema::Collection* maps = schema_.collection("bowl.maps");
    if (!maps) return;
    usize n = record_count(*maps);
    struct Node {
        std::string file, name;
        const gfx::Tilemap* map = nullptr;
        const Value* json = nullptr;
        Vec2 slot;   // place in the layout grid
        Vec2 origin; // world position of the box's corner
        Vec2 size;   // map pixel size
    };
    std::vector<Node> nodes;
    std::map<std::string, usize> by_name;
    for (usize i = 0; i < n; ++i) {
        std::string file = record_id(*maps, i);
        std::string name = fs::path(file).stem().string();
        MapInfo* info = map_info(name, fs::path(file).parent_path().generic_string());
        Doc* d = doc(file);
        if (!info || !info->map || !d || !d->text.valid()) continue;
        by_name[name] = nodes.size();
        nodes.push_back({file, name, info->map.get(), &d->text.value()});
    }
    // Lay the maps out by their exits: a map through an east exit goes to the right, and so on; the first unplaced map
    // starts a new row.
    std::set<std::pair<i32, i32>> taken;
    std::vector<bool> placed(nodes.size(), false);
    Vec2 step_dirs[] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    i32 row = 0;
    for (usize start = 0; start < nodes.size(); ++start) {
        if (placed[start]) continue;
        std::deque<usize> queue{start};
        nodes[start].slot = {0, static_cast<f32>(row)};
        while (taken.contains({0, row})) ++row, nodes[start].slot = {0, static_cast<f32>(row)};
        taken.insert({0, row});
        placed[start] = true;
        while (!queue.empty()) {
            usize i = queue.front();
            queue.pop_front();
            const Value& m = *nodes[i].json;
            i64 li = markers_layer(m);
            if (li < 0) continue;
            Vec2 per = per_cell(m);
            for (const Value& o : m["layers"][static_cast<usize>(li)]["objects"].array()) {
                if (o["type"].as_string() != "exit") continue;
                auto it = by_name.find(std::string(properties_of(o)["to_area"].as_string()));
                if (it == by_name.end() || placed[it->second]) continue;
                int side = edge_of(cells_of(o, per), m["width"].as_float(), m["height"].as_float());
                Vec2 slot = nodes[i].slot + (side >= 0 ? step_dirs[side] : Vec2{1, 0});
                while (taken.contains({static_cast<i32>(slot.x), static_cast<i32>(slot.y)})) slot.y += 1;
                nodes[it->second].slot = slot;
                taken.insert({static_cast<i32>(slot.x), static_cast<i32>(slot.y)});
                placed[it->second] = true;
                queue.push_back(it->second);
            }
        }
        row = 0;
        for (auto [x, y] : taken) row = std::max(row, y + 1);
    }
    // Boxes in world space, a cell of the layout as big as the biggest map plus a gap.
    Vec2 biggest{1, 1};
    for (Node& node : nodes) {
        node.size = node.map->pixel_size();
        biggest = {std::max(biggest.x, node.size.x), std::max(biggest.y, node.size.y)};
    }
    for (Node& node : nodes) node.origin = Vec2{node.slot.x * biggest.x * 1.25f, node.slot.y * biggest.y * 1.3f};

    // The canvas.
    ImGui::SetNextWindowPos({area.x, area.y});
    ImGui::SetNextWindowSize({area.w, area.h});
    ImGui::Begin("##world", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                         ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted("The world");
    ImGui::PopFont();
    ImGui::SameLine();
    ui::hint("Every map, and the exits between them. Click a map to open it; the wheel zooms, right-drag moves.");
    ImVec2 origin = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    Vec2 lo{1e9f, 1e9f}, hi{-1e9f, -1e9f};
    for (const Node& node : nodes) { // the corners of each map's cells (isometric maps reach left of their corner)
        f32 W = static_cast<f32>(node.map->width), H = static_cast<f32>(node.map->height);
        for (Vec2 c : {node.map->cell_to_world(0, 0), node.map->cell_to_world(W, 0), node.map->cell_to_world(0, H), node.map->cell_to_world(W, H)}) {
            Vec2 a = node.origin + c;
            lo = {std::min(lo.x, a.x), std::min(lo.y, a.y - 48)}; // room above for tall things
            hi = {std::max(hi.x, a.x), std::max(hi.y, a.y)};
        }
    }
    WorldView& v = world_view_;
    if (v.zoom <= 0 && !nodes.empty()) {
        v.zoom = std::min(size.x / (hi.x - lo.x + 200), size.y / (hi.y - lo.y + 200));
        v.pan = (lo + hi) * 0.5f;
    }
    Vec2 center{origin.x + size.x * 0.5f, origin.y + size.y * 0.5f};
    auto to_screen = [&](Vec2 w) {
        Vec2 s = center + (w - v.pan) * v.zoom;
        return ImVec2{s.x, s.y};
    };
    ImGuiIO& io = ImGui::GetIO();
    if (hovered && io.MouseWheel != 0) {
        Vec2 before = v.pan + (Vec2{io.MousePos.x, io.MousePos.y} - center) / v.zoom;
        v.zoom = std::clamp(v.zoom * std::pow(1.15f, io.MouseWheel), 0.02f, 8.0f);
        Vec2 after = v.pan + (Vec2{io.MousePos.x, io.MousePos.y} - center) / v.zoom;
        v.pan += before - after;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) v.pan -= Vec2{io.MouseDelta.x, io.MouseDelta.y} / v.zoom;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
    dl->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32(16, 18, 24, 255));
    std::optional<usize> under;
    for (usize i = 0; i < nodes.size(); ++i) {
        const Node& node = nodes[i];
        auto place = [&](Vec2 p) { return to_screen(node.origin + p); };
        draw_tiles(*node.map, place);
        draw_objects(*node.map, *node.json, place, v.zoom);
        // Its outline (the diamond's corners) and name.
        const gfx::Tilemap& m = *node.map;
        f32 W = static_cast<f32>(m.width), H = static_cast<f32>(m.height);
        ImVec2 pts[4] = {place(m.cell_to_world(0, 0)), place(m.cell_to_world(W, 0)), place(m.cell_to_world(W, H)), place(m.cell_to_world(0, H))};
        ImVec2 mouse = io.MousePos;
        bool inside = false;
        for (int k = 0, j = 3; k < 4; j = k++) // a point in a convex quad
            if (((pts[k].y > mouse.y) != (pts[j].y > mouse.y)) &&
                (mouse.x < (pts[j].x - pts[k].x) * (mouse.y - pts[k].y) / (pts[j].y - pts[k].y) + pts[k].x))
                inside = !inside;
        if (inside && hovered) under = i;
        dl->AddPolyline(pts, 4, inside && hovered ? ui::color::accent : ui::color::line, ImDrawFlags_Closed, inside && hovered ? 2.5f : 1.5f);
        ImVec2 label = place(m.cell_to_world(0, 0));
        std::string title = node.name;
        ImVec2 ts = ImGui::CalcTextSize(title.c_str());
        dl->AddRectFilled({label.x - ts.x * 0.5f - 6, label.y - ts.y - 10}, {label.x + ts.x * 0.5f + 6, label.y - 4}, IM_COL32(24, 27, 34, 230), 4);
        dl->AddText({label.x - ts.x * 0.5f, label.y - ts.y - 7}, ui::color::text, title.c_str());
    }
    // Exits: an arrow from each exit to the spawn point it arrives at.
    for (const Node& node : nodes) {
        const Value& m = *node.json;
        i64 li = markers_layer(m);
        if (li < 0) continue;
        Vec2 per = per_cell(m);
        for (const Value& o : m["layers"][static_cast<usize>(li)]["objects"].array()) {
            if (o["type"].as_string() != "exit") continue;
            Value props = properties_of(o);
            auto it = by_name.find(std::string(props["to_area"].as_string()));
            Rect r = cells_of(o, per);
            ImVec2 a = to_screen(node.origin + node.map->cell_to_world(r.center().x, r.center().y));
            if (it == by_name.end()) {
                dl->AddCircleFilled(a, 5, ui::color::danger);
                continue;
            }
            const Node& to = nodes[it->second];
            ImVec2 b = to_screen(to.origin + to.map->cell_to_world(to.map->width * 0.5f, to.map->height * 0.5f));
            if (MapInfo* info = map_info(to.name, fs::path(to.file).parent_path().generic_string()))
                for (const auto& [name, at] : info->spawns)
                    if (name == props["to_spawn"].as_string()) b = to_screen(to.origin + to.map->cell_to_world(at.x, at.y));
            dl->AddLine(a, b, IM_COL32(90, 210, 200, 220), 2);
            Vec2 d{b.x - a.x, b.y - a.y};
            f32 len = std::max(1.0f, d.length());
            Vec2 u = d / len, nrm{-u.y, u.x};
            ImVec2 tip = b, l{b.x - u.x * 10 + nrm.x * 5, b.y - u.y * 10 + nrm.y * 5}, rr{b.x - u.x * 10 - nrm.x * 5, b.y - u.y * 10 - nrm.y * 5};
            dl->AddTriangleFilled(tip, l, rr, IM_COL32(90, 210, 200, 255));
            dl->AddCircleFilled(a, 4, IM_COL32(90, 210, 200, 255));
        }
    }
    dl->PopClipRect();
    if (under) {
        ImGui::SetTooltip("%s: click to open it", nodes[*under].name.c_str());
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            for (usize i = 0; i < schema_.collections.size(); ++i)
                if (schema_.collections[i].id == "bowl.maps") {
                    usize count = record_count(schema_.collections[i]);
                    for (usize r = 0; r < count; ++r)
                        if (record_id(schema_.collections[i], r) == nodes[*under].file) select(i, r);
                }
            world_ = false;
        }
    }
    ImGui::End();
    (void)engine;
}

} // namespace bowl::editor
