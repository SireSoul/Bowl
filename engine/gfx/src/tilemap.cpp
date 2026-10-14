#include "ph/gfx/tilemap.hpp"

#include "ph/core/log.hpp"

#define STBI_NO_STDIO
#include <stb_image.h> // declarations only: stbi_zlib_decode_* (implemented in loaders.cpp)

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <optional>
#include <span>

namespace ph::gfx {

// --- small helpers --------------------------------------------------------------------------------------------------

std::string normalize_path(std::string_view path) {
    std::vector<std::string_view> parts;
    usize start = 0;
    while (start <= path.size()) {
        usize slash = path.find('/', start);
        std::string_view part = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..") parts.pop_back();
            else parts.push_back(part);
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    std::string out;
    for (usize i = 0; i < parts.size(); ++i) {
        if (i) out += '/';
        out += parts[i];
    }
    return out;
}

namespace {

std::string dir_of(std::string_view path) {
    usize slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash + 1));
}

std::optional<std::vector<u8>> base64_decode(std::string_view s) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<u8> out;
    u32 buffer = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = value(c);
        if (v < 0) return std::nullopt;
        buffer = (buffer << 6) | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>(buffer >> bits));
        }
    }
    return out;
}

std::optional<std::vector<u8>> inflate(std::span<const u8> data, bool gzip) {
    int len = 0;
    char* raw = nullptr;
    if (gzip) {
        // gzip: 10-byte header (+ optional fields), raw deflate, 8-byte trailer.
        if (data.size() < 18 || data[0] != 0x1F || data[1] != 0x8B || data[2] != 8) return std::nullopt;
        u8 flags = data[3];
        usize at = 10;
        if (flags & 4) at += 2 + (data[at] | (data[at + 1] << 8)); // FEXTRA
        if (flags & 8) while (at < data.size() && data[at++]) {}  // FNAME
        if (flags & 16) while (at < data.size() && data[at++]) {} // FCOMMENT
        if (flags & 2) at += 2;                                   // FHCRC
        if (at + 8 > data.size()) return std::nullopt;
        raw = stbi_zlib_decode_noheader_malloc(reinterpret_cast<const char*>(data.data() + at),
                                               static_cast<int>(data.size() - at - 8), &len);
    } else {
        raw = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), &len);
    }
    if (!raw) return std::nullopt;
    std::vector<u8> out(raw, raw + len);
    std::free(raw);
    return out;
}

// Tile data: a JSON array of gids, or base64 (optionally zlib/gzip) little-endian u32s.
bool decode_cells(const json::Value& data, std::string_view encoding, std::string_view compression, usize count,
                  std::vector<u32>& out, std::string& error) {
    out.assign(count, 0);
    if (data.is_array()) {
        for (usize i = 0; i < count && i < data.size(); ++i) out[i] = static_cast<u32>(data[i].as_number());
        return true;
    }
    if (encoding != "base64") {
        error = std::format("unsupported layer encoding '{}'", encoding);
        return false;
    }
    auto bytes = base64_decode(data.as_string());
    if (!bytes) {
        error = "bad base64 layer data";
        return false;
    }
    if (compression == "zlib" || compression == "gzip") {
        bytes = inflate(*bytes, compression == "gzip");
        if (!bytes) {
            error = std::format("bad {} layer data", compression);
            return false;
        }
    } else if (!compression.empty()) {
        error = std::format("layer compression '{}' isn't supported; use zlib, gzip or none in Tiled", compression);
        return false;
    }
    if (bytes->size() < count * 4) {
        error = "layer data is shorter than the layer";
        return false;
    }
    for (usize i = 0; i < count; ++i)
        out[i] = u32((*bytes)[i * 4]) | u32((*bytes)[i * 4 + 1]) << 8 | u32((*bytes)[i * 4 + 2]) << 16 |
                 u32((*bytes)[i * 4 + 3]) << 24;
    return true;
}

// Tiled stores properties as [{name, type, value}]; turn them into a plain object: props["solid"].as_bool().
json::Value properties_of(const json::Value& v) {
    json::Value out;
    out.make_object();
    for (const json::Value& p : v["properties"].array()) out.set(p["name"].as_string(), p["value"]);
    return out;
}

Color color_of(std::string_view s, Color fallback) {
    if (s.empty() || s[0] != '#') return fallback;
    s.remove_prefix(1);
    u32 v = static_cast<u32>(std::strtoul(std::string(s).c_str(), nullptr, 16));
    if (s.size() == 8) return Color::rgba8(static_cast<u8>(v >> 16), static_cast<u8>(v >> 8), static_cast<u8>(v), static_cast<u8>(v >> 24));
    if (s.size() == 6) return Color::hex(v);
    return fallback;
}

bool parse_tileset(const json::Value& ts, std::string_view dir, Tileset& out, std::string& error) {
    out.name = std::string(ts["name"].as_string());
    out.tile_width = static_cast<u32>(ts["tilewidth"].as_int());
    out.tile_height = static_cast<u32>(ts["tileheight"].as_int());
    out.columns = static_cast<u32>(ts["columns"].as_int());
    out.tile_count = static_cast<u32>(ts["tilecount"].as_int());
    out.margin = static_cast<u32>(ts["margin"].as_int());
    out.spacing = static_cast<u32>(ts["spacing"].as_int());
    out.tile_offset = {ts["tileoffset"]["x"].as_float(), ts["tileoffset"]["y"].as_float()};
    if (ts.contains("image")) out.image_path = normalize_path(std::string(dir) + std::string(ts["image"].as_string()));
    out.properties = properties_of(ts);
    for (const json::Value& t : ts["tiles"].array()) {
        TileInfo info;
        info.id = static_cast<u32>(t["id"].as_int());
        info.properties = properties_of(t);
        info.type = std::string(t.contains("type") ? t["type"].as_string() : t["class"].as_string());
        for (const json::Value& f : t["animation"].array())
            info.animation.push_back({static_cast<u32>(f["tileid"].as_int()), f["duration"].as_float(100) / 1000.0f});
        if (t.contains("image")) {
            info.image_path = normalize_path(std::string(dir) + std::string(t["image"].as_string()));
            info.image_size = {t["imagewidth"].as_float(), t["imageheight"].as_float()};
        }
        out.tiles.push_back(std::move(info));
    }
    std::sort(out.tiles.begin(), out.tiles.end(), [](const TileInfo& a, const TileInfo& b) { return a.id < b.id; });
    if (out.tile_width == 0 || out.tile_height == 0) {
        error = std::format("tileset '{}' has no tile size", out.name);
        return false;
    }
    if (out.image_path.empty() && out.tiles.empty()) {
        error = std::format("tileset '{}' has no image", out.name);
        return false;
    }
    return true;
}

MapObject parse_object(const json::Value& o) {
    MapObject obj;
    obj.id = static_cast<u32>(o["id"].as_int());
    obj.name = std::string(o["name"].as_string());
    obj.type = std::string(o.contains("type") ? o["type"].as_string() : o["class"].as_string());
    obj.bounds = {o["x"].as_float(), o["y"].as_float(), o["width"].as_float(), o["height"].as_float()};
    obj.rotation = o["rotation"].as_float();
    obj.visible = o["visible"].as_bool(true);
    obj.properties = properties_of(o);
    if (o.contains("gid")) {
        obj.shape = MapObject::Shape::Tile;
        obj.gid = static_cast<u32>(o["gid"].as_number());
    } else if (o["point"].as_bool()) {
        obj.shape = MapObject::Shape::Point;
    } else if (o["ellipse"].as_bool()) {
        obj.shape = MapObject::Shape::Ellipse;
    } else if (o.contains("polygon") || o.contains("polyline")) {
        bool polygon = o.contains("polygon");
        obj.shape = polygon ? MapObject::Shape::Polygon : MapObject::Shape::Polyline;
        for (const json::Value& p : o[polygon ? "polygon" : "polyline"].array())
            obj.points.push_back({p["x"].as_float(), p["y"].as_float()});
    } else if (o.contains("text")) {
        obj.shape = MapObject::Shape::Text;
    }
    return obj;
}

struct LayerContext {
    std::string prefix;
    Vec2 offset;
    f32 opacity = 1;
    bool visible = true;
};

bool parse_layers(const json::Value& list, const LayerContext& parent, std::string_view dir, Tilemap& map,
                  std::string& error) {
    for (const json::Value& l : list.array()) {
        std::string_view type = l["type"].as_string();
        LayerContext ctx;
        ctx.prefix = parent.prefix + std::string(l["name"].as_string());
        ctx.offset = parent.offset + Vec2{l["offsetx"].as_float(), l["offsety"].as_float()};
        ctx.opacity = parent.opacity * l["opacity"].as_float(1);
        ctx.visible = parent.visible && l["visible"].as_bool(true);
        if (type == "group") {
            LayerContext group = ctx;
            group.prefix += "/";
            if (!parse_layers(l["layers"], group, dir, map, error)) return false;
            continue;
        }
        MapLayer layer;
        layer.name = ctx.prefix;
        layer.offset = ctx.offset;
        layer.opacity = ctx.opacity;
        layer.visible = ctx.visible;
        layer.tint = color_of(l["tintcolor"].as_string(), Color::white());
        layer.properties = properties_of(l);
        if (type == "tilelayer") {
            layer.kind = MapLayer::Kind::Tiles;
            std::string_view encoding = l["encoding"].as_string("csv");
            std::string_view compression = l["compression"].as_string();
            if (l.contains("chunks")) { // infinite map: merge chunks into one rectangle
                i32 x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
                for (const json::Value& c : l["chunks"].array()) {
                    i32 cx = static_cast<i32>(c["x"].as_int()), cy = static_cast<i32>(c["y"].as_int());
                    x0 = std::min(x0, cx);
                    y0 = std::min(y0, cy);
                    x1 = std::max(x1, cx + static_cast<i32>(c["width"].as_int()));
                    y1 = std::max(y1, cy + static_cast<i32>(c["height"].as_int()));
                }
                if (x0 <= x1 && y0 <= y1) {
                    layer.origin_x = x0;
                    layer.origin_y = y0;
                    layer.width = static_cast<u32>(x1 - x0);
                    layer.height = static_cast<u32>(y1 - y0);
                    layer.cells.assign(static_cast<usize>(layer.width) * layer.height, 0);
                    for (const json::Value& c : l["chunks"].array()) {
                        u32 cw = static_cast<u32>(c["width"].as_int()), ch = static_cast<u32>(c["height"].as_int());
                        std::vector<u32> cells;
                        if (!decode_cells(c["data"], encoding, compression, static_cast<usize>(cw) * ch, cells, error)) return false;
                        i32 cx = static_cast<i32>(c["x"].as_int()) - x0, cy = static_cast<i32>(c["y"].as_int()) - y0;
                        for (u32 y = 0; y < ch; ++y)
                            for (u32 x = 0; x < cw; ++x)
                                layer.cells[static_cast<usize>(cy + static_cast<i32>(y)) * layer.width +
                                            static_cast<usize>(cx + static_cast<i32>(x))] = cells[y * cw + x];
                    }
                }
            } else {
                layer.width = static_cast<u32>(l["width"].as_int());
                layer.height = static_cast<u32>(l["height"].as_int());
                if (!decode_cells(l["data"], encoding, compression, static_cast<usize>(layer.width) * layer.height,
                                  layer.cells, error))
                    return false;
            }
        } else if (type == "objectgroup") {
            layer.kind = MapLayer::Kind::Objects;
            for (const json::Value& o : l["objects"].array()) layer.objects.push_back(parse_object(o));
        } else if (type == "imagelayer") {
            layer.kind = MapLayer::Kind::Image;
            layer.image_path = normalize_path(std::string(dir) + std::string(l["image"].as_string()));
        } else {
            warn("tilemap: skipping layer '{}' of unknown type '{}'", layer.name, type);
            continue;
        }
        map.layers.push_back(std::move(layer));
    }
    return true;
}

} // namespace

// --- Tileset / MapLayer ---------------------------------------------------------------------------------------------

const TileInfo* Tileset::tile(u32 local_id) const {
    auto it = std::lower_bound(tiles.begin(), tiles.end(), local_id, [](const TileInfo& t, u32 id) { return t.id < id; });
    return it != tiles.end() && it->id == local_id ? &*it : nullptr;
}

Rect Tileset::source_rect(u32 local_id) const {
    u32 cols = std::max(columns, 1u);
    u32 cx = local_id % cols, cy = local_id / cols;
    return {static_cast<f32>(margin + cx * (tile_width + spacing)), static_cast<f32>(margin + cy * (tile_height + spacing)),
            static_cast<f32>(tile_width), static_cast<f32>(tile_height)};
}

u32 MapLayer::at(i32 x, i32 y) const {
    i32 lx = x - origin_x, ly = y - origin_y;
    if (lx < 0 || ly < 0 || lx >= static_cast<i32>(width) || ly >= static_cast<i32>(height)) return 0;
    return cells[static_cast<usize>(ly) * width + static_cast<usize>(lx)];
}

// --- Tilemap --------------------------------------------------------------------------------------------------------

bool Tilemap::parse(const json::Value& doc, std::string_view base_dir,
                    const std::function<std::optional<std::string>(const std::string&)>& read_file, Tilemap& out,
                    std::string& error) {
    std::string_view orientation = doc["orientation"].as_string("orthogonal");
    if (orientation == "orthogonal") out.orientation = Orientation::Orthogonal;
    else if (orientation == "isometric") out.orientation = Orientation::Isometric;
    else {
        error = std::format("'{}' maps aren't supported yet (orthogonal and isometric are)", orientation);
        return false;
    }
    out.width = static_cast<u32>(doc["width"].as_int());
    out.height = static_cast<u32>(doc["height"].as_int());
    out.tile_width = static_cast<u32>(doc["tilewidth"].as_int());
    out.tile_height = static_cast<u32>(doc["tileheight"].as_int());
    out.infinite = doc["infinite"].as_bool();
    out.background = color_of(doc["backgroundcolor"].as_string(), Color::transparent());
    out.properties = properties_of(doc);
    if (out.tile_width == 0 || out.tile_height == 0) {
        error = "map has no tile size (is this a Tiled .tmj file?)";
        return false;
    }

    for (const json::Value& ref : doc["tilesets"].array()) {
        Tileset ts;
        if (ref.contains("source")) {
            std::string source = std::string(ref["source"].as_string());
            if (source.ends_with(".tsx")) {
                error = std::format("tileset '{}' is XML; export it from Tiled as JSON (.tsj)", source);
                return false;
            }
            std::string path = normalize_path(std::string(base_dir) + source);
            auto text = read_file(path);
            if (!text) {
                error = std::format("can't read tileset '{}'", path);
                return false;
            }
            auto parsed = json::parse(*text);
            if (!parsed) {
                error = std::format("tileset '{}' {}:{}: {}", path, parsed.error->line, parsed.error->column,
                                    parsed.error->message);
                return false;
            }
            if (!parse_tileset(parsed.value, dir_of(path), ts, error)) return false;
        } else if (!parse_tileset(ref, base_dir, ts, error)) {
            return false;
        }
        ts.first_gid = static_cast<u32>(ref["firstgid"].as_int(1));
        out.tilesets.push_back(std::move(ts));
    }
    std::sort(out.tilesets.begin(), out.tilesets.end(), [](const Tileset& a, const Tileset& b) { return a.first_gid < b.first_gid; });
    return parse_layers(doc["layers"], {}, base_dir, out, error);
}

const MapLayer* Tilemap::layer(std::string_view name) const {
    for (const MapLayer& l : layers)
        if (l.name == name) return &l;
    return nullptr;
}

const Tileset* Tilemap::tileset_for(u32 gid) const {
    gid &= tile_gid_mask;
    if (gid == 0) return nullptr;
    const Tileset* found = nullptr;
    for (const Tileset& ts : tilesets) {
        if (ts.first_gid > gid) break;
        found = &ts;
    }
    return found;
}

const TileInfo* Tilemap::tile_info(u32 gid) const {
    const Tileset* ts = tileset_for(gid);
    return ts ? ts->tile((gid & tile_gid_mask) - ts->first_gid) : nullptr;
}

const json::Value& Tilemap::tile_property(std::string_view layer_name, i32 x, i32 y, std::string_view key) const {
    static const json::Value none;
    const MapLayer* l = layer(layer_name);
    if (!l) return none;
    const TileInfo* info = tile_info(l->at(x, y));
    return info ? info->properties[key] : none;
}

Vec2 Tilemap::cell_to_world(f32 x, f32 y) const {
    f32 tw = static_cast<f32>(tile_width), th = static_cast<f32>(tile_height);
    if (orientation == Orientation::Isometric)
        return {(x - y) * tw * 0.5f + static_cast<f32>(height) * tw * 0.5f, (x + y) * th * 0.5f};
    return {x * tw, y * th};
}

Vec2 Tilemap::world_to_cell(Vec2 w) const {
    f32 tw = static_cast<f32>(tile_width), th = static_cast<f32>(tile_height);
    if (orientation == Orientation::Isometric) {
        f32 a = (w.x - static_cast<f32>(height) * tw * 0.5f) / (tw * 0.5f); // x - y
        f32 b = w.y / (th * 0.5f);                                          // x + y
        return {(a + b) * 0.5f, (b - a) * 0.5f};
    }
    return {w.x / tw, w.y / th};
}

// --- drawing --------------------------------------------------------------------------------------------------------

namespace {

struct TileImage {
    const Texture* texture = nullptr;
    Rect source;
};

TileImage image_for(const Tileset& ts, u32 local, f32 time) {
    if (const TileInfo* info = ts.tile(local); info && !info->animation.empty()) {
        f32 total = 0;
        for (const TileFrame& f : info->animation) total += f.seconds;
        if (total > 0) {
            f32 t = std::fmod(std::max(time, 0.0f), total);
            for (const TileFrame& f : info->animation) {
                if (t < f.seconds) {
                    local = f.tile;
                    break;
                }
                t -= f.seconds;
            }
        }
    }
    if (ts.image) return {ts.image.get(), ts.source_rect(local)};
    if (const TileInfo* info = ts.tile(local); info && info->image)
        return {info->image.get(), {0, 0, static_cast<f32>(info->image->width()), static_cast<f32>(info->image->height())}};
    return {};
}

// One tile image with Tiled's flip flags. `anchor` is where the image's bottom-left (orthogonal) or bottom-center
// (isometric) goes; pivot_x is 0 or 0.5 accordingly.
void draw_tile_image(SpriteBatch& batch, const TileImage& img, u32 gid, Vec2 anchor, f32 pivot_x, Vec2 size, Color color,
                     f32 depth, f32 rotation_deg = 0) {
    const Texture& tex = *img.texture;
    Vec2 inv{1.0f / static_cast<f32>(tex.width()), 1.0f / static_cast<f32>(tex.height())};
    Vec4 uv{img.source.x * inv.x, img.source.y * inv.y, img.source.right() * inv.x, img.source.bottom() * inv.y};
    bool h = gid & tile_flip_h, v = gid & tile_flip_v, d = gid & tile_flip_d;
    SpriteInstance s;
    s.color = color;
    s.depth = depth;
    s.size = size;
    if (d) {
        // Transpose = flip locally, then rotate -90 degrees; draw around the image center so it stays in place.
        bool local_h = !v, local_v = h;
        if (local_h) std::swap(uv.x, uv.z);
        if (local_v) std::swap(uv.y, uv.w);
        s.size = {size.y, size.x};
        s.rotation = -pi / 2;
        s.origin = {0.5f, 0.5f};
        s.position = anchor + Vec2{(0.5f - pivot_x) * size.x, -size.y * 0.5f};
    } else {
        if (h) std::swap(uv.x, uv.z);
        if (v) std::swap(uv.y, uv.w);
        s.origin = {pivot_x, 1.0f};
        s.position = anchor;
        s.rotation = radians(rotation_deg);
    }
    s.uv = uv;
    batch.draw(tex, s);
}

} // namespace

void draw_tilemap(SpriteBatch& batch, const Tilemap& map, const TilemapDrawOptions& o) {
    bool iso = map.orientation == Tilemap::Orientation::Isometric;
    f32 tw = static_cast<f32>(map.tile_width), th = static_cast<f32>(map.tile_height);
    f32 pivot_x = iso ? 0.5f : 0.0f;
    Rect view = batch.camera().visible_bounds();
    view.x -= o.position.x;
    view.y -= o.position.y;

    // Tall tiles reach above their cell: widen the culling window by the tallest tile.
    f32 overhang = 0;
    for (const Tileset& ts : map.tilesets) {
        overhang = std::max(overhang, static_cast<f32>(ts.tile_height) + std::abs(ts.tile_offset.y));
        for (const TileInfo& t : ts.tiles) overhang = std::max(overhang, t.image_size.y);
    }

    bool any_ysort_property = std::any_of(map.layers.begin(), map.layers.end(),
                                          [](const MapLayer& l) { return l.properties["ysort"].is_bool(); });
    constexpr f32 underneath = -1e7f; // flat layers under everything y-sorted

    for (usize li = 0; li < map.layers.size(); ++li) {
        const MapLayer& layer = map.layers[li];
        if (!layer.visible || layer.opacity <= 0) continue;
        if (!o.only_layer.empty() && layer.name != o.only_layer) continue;
        Color color{layer.tint.r, layer.tint.g, layer.tint.b, layer.tint.a * layer.opacity};
        bool sorts = o.y_sort && (any_ysort_property ? layer.properties["ysort"].as_bool() : li > 0);
        f32 layer_depth = o.depth + static_cast<f32>(li) * o.layer_depth_step + (o.y_sort && !sorts ? underneath : 0.0f);
        Vec2 base = o.position + layer.offset;

        if (layer.kind == MapLayer::Kind::Image) {
            if (!layer.image) continue;
            SpriteInstance s;
            s.position = base;
            s.size = layer.image->size();
            s.origin = {0, 0};
            s.color = color;
            s.depth = layer_depth;
            batch.draw(*layer.image, s);
            continue;
        }

        if (layer.kind == MapLayer::Kind::Objects) { // tile objects are drawn; other shapes are data
            for (const MapObject& obj : layer.objects) {
                if (!obj.visible || obj.shape != MapObject::Shape::Tile) continue;
                const Tileset* ts = map.tileset_for(obj.gid);
                if (!ts) continue;
                TileImage img = image_for(*ts, (obj.gid & tile_gid_mask) - ts->first_gid, o.time);
                if (!img.texture) continue;
                // Tiled object coordinates in isometric maps are in "tile height" units along both grid axes.
                Vec2 at = iso ? map.cell_to_world(obj.bounds.x / th, obj.bounds.y / th) : obj.bounds.position();
                at += base + ts->tile_offset;
                Vec2 size = obj.bounds.w > 0 ? obj.bounds.size() : img.source.size();
                f32 depth = sorts ? o.depth + at.y : layer_depth;
                draw_tile_image(batch, img, obj.gid, at, pivot_x, size, color, depth, obj.rotation);
            }
            continue;
        }

        // Tile layer: only the cells whose images can touch the view.
        Rect padded{view.x - tw, view.y - th, view.w + 2 * tw, view.h + 2 * th + overhang};
        Vec2 corners[4] = {map.world_to_cell(padded.position()), map.world_to_cell({padded.right(), padded.y}),
                           map.world_to_cell({padded.x, padded.bottom()}), map.world_to_cell({padded.right(), padded.bottom()})};
        f32 min_x = corners[0].x, max_x = corners[0].x, min_y = corners[0].y, max_y = corners[0].y;
        for (Vec2 c : corners) {
            min_x = std::min(min_x, c.x);
            max_x = std::max(max_x, c.x);
            min_y = std::min(min_y, c.y);
            max_y = std::max(max_y, c.y);
        }
        i32 x0 = std::max(layer.origin_x, static_cast<i32>(std::floor(min_x)));
        i32 y0 = std::max(layer.origin_y, static_cast<i32>(std::floor(min_y)));
        i32 x1 = std::min(layer.origin_x + static_cast<i32>(layer.width) - 1, static_cast<i32>(std::ceil(max_x)));
        i32 y1 = std::min(layer.origin_y + static_cast<i32>(layer.height) - 1, static_cast<i32>(std::ceil(max_y)));
        for (i32 y = y0; y <= y1; ++y)
            for (i32 x = x0; x <= x1; ++x) {
                u32 gid = layer.at(x, y);
                if (!(gid & tile_gid_mask)) continue;
                const Tileset* ts = map.tileset_for(gid);
                if (!ts) continue;
                TileImage img = image_for(*ts, (gid & tile_gid_mask) - ts->first_gid, o.time);
                if (!img.texture) continue;
                Vec2 cell = map.cell_to_world(static_cast<f32>(x), static_cast<f32>(y));
                // Bottom of the cell: bottom-left (orthogonal) or the diamond's bottom corner (isometric).
                Vec2 anchor = cell + Vec2{0, th} + base + ts->tile_offset;
                f32 depth = sorts ? o.depth + anchor.y : layer_depth;
                draw_tile_image(batch, img, gid, anchor, pivot_x, img.source.size(), color, depth);
            }
    }
}

// --- loader ---------------------------------------------------------------------------------------------------------

void register_tilemap_loader(AssetManager& assets) {
    assets.register_loader<Tilemap>([](AssetLoadContext& ctx) -> std::unique_ptr<Tilemap> {
        auto text = ctx.read_text(ctx.path());
        if (!text) {
            log_asset_error(ctx.path(), "Tilemap", "file not found in any mount");
            return nullptr;
        }
        auto parsed = json::parse(*text);
        if (!parsed) {
            log_asset_error(ctx.path(), "Tilemap",
                            std::format("JSON error at {}:{}: {}", parsed.error->line, parsed.error->column, parsed.error->message));
            return nullptr;
        }
        auto map = std::make_unique<Tilemap>();
        std::string error;
        auto read = [&](const std::string& path) { return ctx.read_text(path); }; // tracked: edits hot-reload
        if (!Tilemap::parse(parsed.value, dir_of(ctx.path()), read, *map, error)) {
            log_asset_error(ctx.path(), "Tilemap", error);
            return nullptr;
        }
        AssetManager& assets = ctx.assets();
        for (Tileset& ts : map->tilesets) {
            if (!ts.image_path.empty()) ts.image = assets.load<Texture>(ts.image_path);
            for (TileInfo& t : ts.tiles)
                if (!t.image_path.empty()) t.image = assets.load<Texture>(t.image_path);
        }
        for (MapLayer& l : map->layers)
            if (!l.image_path.empty()) l.image = assets.load<Texture>(l.image_path);
        return map;
    });
}

} // namespace ph::gfx
