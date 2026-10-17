#include "bowl/gfx/tilemap.hpp"

#include "bowl/core/log.hpp"
#include "bowl/core/profiler.hpp"
#include "bowl/core/xml.hpp"

#define STBI_NO_STDIO
#include <stb_image.h> // declarations only: stbi_zlib_decode_* (implemented in stb_image.cpp)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <span>

namespace bowl::gfx {

// Small helpers

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

// A cell coordinate from a point, as an int. Points beyond any map (and the NaN a tile 0 pixels wide gives) clamp to a
// range no map reaches, where converting them as they are would be undefined; staggered maps then double it.
constexpr f32 cell_limit = 1 << 28;
i32 cell_floor(f32 v) { return std::isnan(v) ? 0 : static_cast<i32>(std::clamp(std::floor(v), -cell_limit, cell_limit)); }
i32 cell_ceil(f32 v) { return std::isnan(v) ? 0 : static_cast<i32>(std::clamp(std::ceil(v), -cell_limit, cell_limit)); }

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

// Exactly `size` bytes of zlib or gzip data: a stream that inflates to more fails, rather than to whatever it asks.
std::optional<std::vector<u8>> inflate(std::span<const u8> data, bool gzip, usize size) {
    std::vector<u8> out(size);
    auto into = reinterpret_cast<char*>(out.data());
    int len = -1;
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
        len = stbi_zlib_decode_noheader_buffer(into, static_cast<int>(size), reinterpret_cast<const char*>(data.data() + at),
                                               static_cast<int>(data.size() - at - 8));
    } else {
        len = stbi_zlib_decode_buffer(into, static_cast<int>(size), reinterpret_cast<const char*>(data.data()),
                                      static_cast<int>(data.size()));
    }
    if (len < 0 || static_cast<usize>(len) != size) return std::nullopt;
    return out;
}

// Tile data: a JSON array of gids, or base64 (optionally zlib/gzip) little-endian u32s.
// Nothing is allocated until the data is known to hold every cell, so a layer that claims to be huge can't ask for
// gigabytes.
bool decode_cells(const json::Value& data, std::string_view encoding, std::string_view compression, u64 count,
                  std::vector<u32>& out, std::string& error) {
    if (count > max_layer_cells) {
        error = std::format("a layer of {} cells is more than the {} a layer can have", count, max_layer_cells);
        return false;
    }
    if (data.is_array()) {
        if (data.size() < count) {
            error = "layer data is shorter than the layer";
            return false;
        }
        out.resize(count);
        for (usize i = 0; i < count; ++i) out[i] = static_cast<u32>(data[i].as_int());
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
        bytes = inflate(*bytes, compression == "gzip", count * 4);
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
    out.resize(count);
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
    for (const json::Value& ws : ts["wangsets"].array()) {
        WangSet set;
        set.name = std::string(ws["name"].as_string());
        std::string_view type = ws["type"].as_string("corner");
        set.type = type == "edge" ? WangSet::Type::Edge : type == "mixed" ? WangSet::Type::Mixed : WangSet::Type::Corner;
        set.properties = properties_of(ws);
        for (const json::Value& c : ws["colors"].array())
            set.terrains.push_back({std::string(c["name"].as_string()), color_of(c["color"].as_string(), Color::white()),
                                    c["probability"].as_float(1), properties_of(c)});
        for (const json::Value& wt : ws["wangtiles"].array()) {
            WangSet::Tile tile{static_cast<u32>(wt["tileid"].as_int()), {}};
            for (usize i = 0; i < 8 && i < wt["wangid"].size(); ++i) tile.wang[i] = static_cast<u8>(wt["wangid"][i].as_int());
            set.tiles.push_back(tile);
        }
        out.wang_sets.push_back(std::move(set));
    }
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
        obj.gid = static_cast<u32>(o["gid"].as_int());
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

// `expand` fills in object templates.
bool parse_layers(const json::Value& list, const LayerContext& parent, std::string_view dir, Tilemap& map,
                  const std::function<json::Value(const json::Value&)>& expand, std::string& error) {
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
            if (!parse_layers(l["layers"], group, dir, map, expand, error)) return false;
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
                // In 64 bits, and checked: chunks whose corners don't fit in 32 bits, or that cover more cells than a
                // layer holds, aren't a map Tiled made.
                i64 x0 = INT64_MAX, y0 = INT64_MAX, x1 = INT64_MIN, y1 = INT64_MIN;
                for (const json::Value& c : l["chunks"].array()) {
                    i64 cx = c["x"].as_int(), cy = c["y"].as_int(), cw = c["width"].as_int(), ch = c["height"].as_int();
                    if (cx < INT32_MIN || cy < INT32_MIN || cw < 0 || ch < 0 || cw > INT32_MAX || ch > INT32_MAX ||
                        cx > INT32_MAX - cw || cy > INT32_MAX - ch) { // (cx + cw could overflow even 64 bits)
                        error = std::format("layer '{}' has a chunk out of range", layer.name);
                        return false;
                    }
                    x0 = std::min(x0, cx);
                    y0 = std::min(y0, cy);
                    x1 = std::max(x1, cx + cw);
                    y1 = std::max(y1, cy + ch);
                }
                if (x0 <= x1 && y0 <= y1) {
                    if (static_cast<u64>(x1 - x0) * static_cast<u64>(y1 - y0) > max_layer_cells) {
                        error = std::format("layer '{}' spans {} x {} cells, more than a layer can have", layer.name,
                                            x1 - x0, y1 - y0);
                        return false;
                    }
                    layer.origin_x = static_cast<i32>(x0);
                    layer.origin_y = static_cast<i32>(y0);
                    layer.width = static_cast<u32>(x1 - x0);
                    layer.height = static_cast<u32>(y1 - y0);
                    layer.cells.assign(static_cast<usize>(layer.width) * layer.height, 0);
                    for (const json::Value& c : l["chunks"].array()) {
                        u32 cw = static_cast<u32>(c["width"].as_int()), ch = static_cast<u32>(c["height"].as_int());
                        std::vector<u32> cells;
                        if (!decode_cells(c["data"], encoding, compression, u64{cw} * ch, cells, error)) return false;
                        usize cx = static_cast<usize>(c["x"].as_int() - x0), cy = static_cast<usize>(c["y"].as_int() - y0);
                        for (usize y = 0; y < ch; ++y)
                            for (usize x = 0; x < cw; ++x)
                                layer.cells[(cy + y) * layer.width + cx + x] = cells[y * cw + x];
                    }
                }
            } else {
                layer.width = static_cast<u32>(l["width"].as_int());
                layer.height = static_cast<u32>(l["height"].as_int());
                if (!decode_cells(l["data"], encoding, compression, u64{layer.width} * layer.height, layer.cells, error))
                    return false;
            }
        } else if (type == "objectgroup") {
            layer.kind = MapLayer::Kind::Objects;
            for (const json::Value& o : l["objects"].array()) layer.objects.push_back(parse_object(expand(o)));
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

// .tmx and .tsx: Tiled's XML holds the same data as its JSON, so it's turned into the JSON shape and read by the same
// code.

f64 number_of(std::string_view s, f64 fallback = 0) {
    if (s.empty()) return fallback;
    char* end = nullptr;
    std::string copy(s);
    f64 v = std::strtod(copy.c_str(), &end);
    return end == copy.c_str() ? fallback : v;
}

void copy_numbers(const xml::Element& from, json::Value& to, std::initializer_list<std::string_view> keys) {
    for (std::string_view k : keys)
        if (from.has(k)) to.set(k, number_of(from.attribute(k)));
}

json::Value xml_properties(const xml::Element& e) {
    json::Value list;
    list.make_array();
    const xml::Element* props = e.child("properties");
    if (!props) return list;
    for (const xml::Element& p : props->children) {
        if (p.name != "property") continue;
        json::Value prop;
        std::string_view type = p.attribute("type", "string");
        std::string_view value = p.has("value") ? p.attribute("value") : std::string_view(p.text);
        prop.set("name", p.attribute("name"));
        prop.set("type", type);
        if (type == "bool") prop.set("value", value == "true");
        else if (type == "int" || type == "float" || type == "object") prop.set("value", number_of(value));
        else if (type == "class") {
            json::Value fields;
            fields.make_object();
            json::Value members = xml_properties(p); // kept alive: the loop below points into it
            for (const json::Value& f : members.array()) fields.set(f["name"].as_string(), f["value"]);
            prop.set("value", std::move(fields));
        } else prop.set("value", value);
        list.push(std::move(prop));
    }
    return list;
}

void copy_common(const xml::Element& e, json::Value& out) {
    if (e.has("id")) out.set("id", number_of(e.attribute("id")));
    out.set("name", e.attribute("name"));
    if (e.has("class")) out.set("class", e.attribute("class"));
    if (e.has("type")) out.set("type", e.attribute("type"));
    out.set("properties", xml_properties(e));
}

json::Value xml_tileset(const xml::Element& e) {
    json::Value ts;
    copy_common(e, ts);
    copy_numbers(e, ts, {"tilewidth", "tileheight", "tilecount", "columns", "spacing", "margin"});
    if (const xml::Element* off = e.child("tileoffset")) {
        json::Value o;
        o.set("x", number_of(off->attribute("x")));
        o.set("y", number_of(off->attribute("y")));
        ts.set("tileoffset", std::move(o));
    }
    if (const xml::Element* img = e.child("image")) {
        ts.set("image", img->attribute("source"));
        ts.set("imagewidth", number_of(img->attribute("width")));
        ts.set("imageheight", number_of(img->attribute("height")));
    }
    json::Value tiles;
    tiles.make_array();
    for (const xml::Element& t : e.children) {
        if (t.name != "tile") continue;
        json::Value tile;
        copy_common(t, tile);
        if (const xml::Element* img = t.child("image")) {
            tile.set("image", img->attribute("source"));
            tile.set("imagewidth", number_of(img->attribute("width")));
            tile.set("imageheight", number_of(img->attribute("height")));
        }
        if (const xml::Element* anim = t.child("animation")) {
            json::Value frames;
            frames.make_array();
            for (const xml::Element& f : anim->children) {
                if (f.name != "frame") continue;
                json::Value frame;
                frame.set("tileid", number_of(f.attribute("tileid")));
                frame.set("duration", number_of(f.attribute("duration"), 100));
                frames.push(std::move(frame));
            }
            tile.set("animation", std::move(frames));
        }
        tiles.push(std::move(tile));
    }
    ts.set("tiles", std::move(tiles));
    if (const xml::Element* sets = e.child("wangsets")) {
        json::Value list;
        list.make_array();
        for (const xml::Element& w : sets->children) {
            if (w.name != "wangset") continue;
            json::Value set;
            copy_common(w, set);
            set.set("type", w.attribute("type", "corner"));
            json::Value colors, wangtiles;
            colors.make_array();
            wangtiles.make_array();
            for (const xml::Element& c : w.children) {
                if (c.name == "wangcolor") {
                    json::Value color;
                    copy_common(c, color);
                    color.set("color", c.attribute("color"));
                    color.set("probability", number_of(c.attribute("probability"), 1));
                    colors.push(std::move(color));
                } else if (c.name == "wangtile") {
                    json::Value tile, ids;
                    tile.set("tileid", number_of(c.attribute("tileid")));
                    ids.make_array();
                    std::string_view list_text = c.attribute("wangid");
                    usize start = 0;
                    while (start <= list_text.size()) {
                        usize comma = list_text.find(',', start);
                        ids.push(number_of(list_text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start)));
                        if (comma == std::string_view::npos) break;
                        start = comma + 1;
                    }
                    tile.set("wangid", std::move(ids));
                    wangtiles.push(std::move(tile));
                }
            }
            set.set("colors", std::move(colors));
            set.set("wangtiles", std::move(wangtiles));
            list.push(std::move(set));
        }
        ts.set("wangsets", std::move(list));
    }
    return ts;
}

// Layer data: <tile gid=".."/> elements, CSV text, or base64 text (left for decode_cells).
json::Value xml_cells(const xml::Element& data, std::string_view encoding) {
    if (encoding == "base64") {
        std::string trimmed;
        for (char c : data.text)
            if (c != ' ' && c != '\n' && c != '\r' && c != '\t') trimmed += c;
        return json::Value(std::move(trimmed));
    }
    json::Value cells;
    cells.make_array();
    if (encoding == "csv") {
        usize start = 0;
        std::string_view text = data.text;
        while (start < text.size()) {
            usize comma = text.find(',', start);
            std::string_view part = text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
            usize a = part.find_first_not_of(" \t\r\n"), b = part.find_last_not_of(" \t\r\n");
            if (a != std::string_view::npos) cells.push(number_of(part.substr(a, b - a + 1)));
            if (comma == std::string_view::npos) break;
            start = comma + 1;
        }
    } else {
        for (const xml::Element& t : data.children)
            if (t.name == "tile") cells.push(number_of(t.attribute("gid")));
    }
    return cells;
}

json::Value xml_object(const xml::Element& e) {
    json::Value o;
    copy_common(e, o);
    copy_numbers(e, o, {"x", "y", "width", "height", "rotation", "gid"});
    o.set("visible", e.attribute("visible", "1") != "0");
    for (const xml::Element& c : e.children) {
        if (c.name == "point" || c.name == "ellipse") o.set(c.name, true);
        else if (c.name == "polygon" || c.name == "polyline") {
            json::Value points;
            points.make_array();
            std::string_view list = c.attribute("points");
            usize start = 0;
            while (start < list.size()) {
                usize space = list.find(' ', start);
                std::string_view pair = list.substr(start, space == std::string_view::npos ? std::string_view::npos : space - start);
                usize comma = pair.find(',');
                if (comma != std::string_view::npos) {
                    json::Value p;
                    p.set("x", number_of(pair.substr(0, comma)));
                    p.set("y", number_of(pair.substr(comma + 1)));
                    points.push(std::move(p));
                }
                if (space == std::string_view::npos) break;
                start = space + 1;
            }
            o.set(c.name, std::move(points));
        } else if (c.name == "text") {
            json::Value text;
            text.set("text", c.text);
            o.set("text", std::move(text));
        }
    }
    if (e.has("template")) {
        // A template instance: only what it sets itself, so the template fills in the rest.
        o.set("template", e.attribute("template"));
        if (!e.has("name")) o.erase("name");
        if (!e.has("visible")) o.erase("visible");
        if (!e.child("properties")) o.erase("properties");
    }
    return o;
}

// A .tx object template, as Tiled's JSON template (.tj).
json::Value xml_template(const xml::Element& e) {
    json::Value t;
    t.set("type", "template");
    if (const xml::Element* o = e.child("object")) t.set("object", xml_object(*o));
    if (const xml::Element* ts = e.child("tileset")) {
        json::Value tileset;
        tileset.set("firstgid", number_of(ts->attribute("firstgid"), 1));
        tileset.set("source", ts->attribute("source"));
        t.set("tileset", std::move(tileset));
    }
    return t;
}

json::Value xml_layers(const xml::Element& parent) {
    json::Value layers;
    layers.make_array();
    for (const xml::Element& e : parent.children) {
        json::Value l;
        if (e.name == "layer") l.set("type", "tilelayer");
        else if (e.name == "objectgroup") l.set("type", "objectgroup");
        else if (e.name == "imagelayer") l.set("type", "imagelayer");
        else if (e.name == "group") l.set("type", "group");
        else continue;
        copy_common(e, l);
        copy_numbers(e, l, {"opacity", "offsetx", "offsety", "width", "height"});
        l.set("visible", e.attribute("visible", "1") != "0");
        if (e.has("tintcolor")) l.set("tintcolor", e.attribute("tintcolor"));
        if (e.name == "layer") {
            if (const xml::Element* data = e.child("data")) {
                std::string_view encoding = data->attribute("encoding");
                l.set("encoding", encoding.empty() ? "csv" : encoding);
                if (data->has("compression")) l.set("compression", data->attribute("compression"));
                if (data->child("chunk")) {
                    json::Value chunks;
                    chunks.make_array();
                    for (const xml::Element& c : data->children) {
                        if (c.name != "chunk") continue;
                        json::Value chunk;
                        copy_numbers(c, chunk, {"x", "y", "width", "height"});
                        chunk.set("data", xml_cells(c, encoding));
                        chunks.push(std::move(chunk));
                    }
                    l.set("chunks", std::move(chunks));
                } else {
                    l.set("data", xml_cells(*data, encoding));
                }
            }
        } else if (e.name == "objectgroup") {
            json::Value objects;
            objects.make_array();
            for (const xml::Element& o : e.children)
                if (o.name == "object") objects.push(xml_object(o));
            l.set("objects", std::move(objects));
        } else if (e.name == "imagelayer") {
            if (const xml::Element* img = e.child("image")) l.set("image", img->attribute("source"));
        } else {
            l.set("layers", xml_layers(e));
        }
        layers.push(std::move(l));
    }
    return layers;
}

json::Value xml_map(const xml::Element& e) {
    json::Value m;
    m.set("orientation", e.attribute("orientation", "orthogonal"));
    copy_numbers(e, m, {"width", "height", "tilewidth", "tileheight", "hexsidelength"});
    m.set("infinite", e.attribute("infinite") == "1");
    for (std::string_view k : {"backgroundcolor", "staggeraxis", "staggerindex"})
        if (e.has(k)) m.set(k, e.attribute(k));
    m.set("properties", xml_properties(e));
    json::Value tilesets;
    tilesets.make_array();
    for (const xml::Element& t : e.children) {
        if (t.name != "tileset") continue;
        json::Value ts;
        if (t.has("source")) ts.set("source", t.attribute("source"));
        else ts = xml_tileset(t);
        ts.set("firstgid", number_of(t.attribute("firstgid"), 1));
        tilesets.push(std::move(ts));
    }
    m.set("tilesets", std::move(tilesets));
    m.set("layers", xml_layers(e));
    return m;
}

} // namespace

// Tileset / MapLayer

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

const WangSet* Tileset::wang_set(std::string_view set_name) const {
    for (const WangSet& s : wang_sets)
        if (s.name == set_name) return &s;
    return nullptr;
}

u8 WangSet::terrain(std::string_view terrain_name) const {
    for (usize i = 0; i < terrains.size(); ++i)
        if (terrains[i].name == terrain_name) return static_cast<u8>(i + 1);
    return 0;
}

std::vector<u32> WangSet::matching(const std::array<u8, 8>& want) const {
    std::vector<u32> out;
    for (const Tile& t : tiles) {
        bool fits = true;
        for (usize i = 0; i < 8 && fits; ++i) fits = want[i] == 0 || t.wang[i] == want[i];
        if (fits) out.push_back(t.tile);
    }
    return out;
}

std::optional<u32> WangSet::corner_tile(u8 top_left, u8 top_right, u8 bottom_right, u8 bottom_left, u32 pick) const {
    std::vector<u32> fits = matching({0, top_right, 0, bottom_right, 0, bottom_left, 0, top_left});
    if (fits.empty()) return std::nullopt;
    return fits[pick % fits.size()];
}

usize fill_from_corners(MapLayer& layer, const Tileset& tileset, const WangSet& set, std::span<const u8> corners,
                        u32 width, u32 height) {
    layer.kind = MapLayer::Kind::Tiles;
    layer.width = width;
    layer.height = height;
    layer.cells.assign(static_cast<usize>(width) * height, 0);
    if (corners.size() < static_cast<usize>(width + 1) * (height + 1)) return layer.cells.size();
    usize missing = 0;
    auto corner = [&](u32 x, u32 y) { return corners[static_cast<usize>(y) * (width + 1) + x]; };
    for (u32 y = 0; y < height; ++y)
        for (u32 x = 0; x < width; ++x) {
            u8 tl = corner(x, y), tr = corner(x + 1, y), br = corner(x + 1, y + 1), bl = corner(x, y + 1);
            if ((tl | tr | br | bl) == 0) continue; // no terrain at all: left empty
            u32 pick = (x * 73856093u) ^ (y * 19349663u); // same corners, same place: same variant
            auto tile = set.corner_tile(tl, tr, br, bl, pick);
            if (tile) layer.cells[static_cast<usize>(y) * width + x] = tileset.first_gid + *tile;
            else ++missing;
        }
    return missing;
}

u32 MapLayer::at(i32 x, i32 y) const {
    i32 lx = x - origin_x, ly = y - origin_y;
    if (lx < 0 || ly < 0 || lx >= static_cast<i32>(width) || ly >= static_cast<i32>(height)) return 0;
    return cells[static_cast<usize>(ly) * width + static_cast<usize>(lx)];
}

// Tilemap

bool Tilemap::parse(const json::Value& doc, std::string_view base_dir,
                    const std::function<std::optional<std::string>(const std::string&)>& read_file, Tilemap& out,
                    std::string& error) {
    std::string_view orientation = doc["orientation"].as_string("orthogonal");
    if (orientation == "orthogonal") out.orientation = Orientation::Orthogonal;
    else if (orientation == "isometric") out.orientation = Orientation::Isometric;
    else if (orientation == "staggered") out.orientation = Orientation::Staggered;
    else if (orientation == "hexagonal") out.orientation = Orientation::Hexagonal;
    else {
        error = std::format("unknown map orientation '{}'", orientation);
        return false;
    }
    out.stagger_x = doc["staggeraxis"].as_string("y") == "x";
    out.stagger_even = doc["staggerindex"].as_string("odd") == "even";
    out.hex_side_length = out.orientation == Orientation::Hexagonal ? static_cast<u32>(doc["hexsidelength"].as_int()) : 0;
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
            std::string path = normalize_path(std::string(base_dir) + source);
            auto text = read_file(path);
            if (!text) {
                error = std::format("can't read tileset '{}'", path);
                return false;
            }
            json::Value tileset_doc;
            if (source.ends_with(".tsx")) {
                auto parsed = xml::parse(*text);
                if (!parsed || parsed.root.name != "tileset") {
                    error = parsed ? std::format("tileset '{}' has no <tileset>", path)
                                   : std::format("tileset '{}' {}:{}: {}", path, parsed.error->line,
                                                 parsed.error->column, parsed.error->message);
                    return false;
                }
                tileset_doc = xml_tileset(parsed.root);
            } else {
                auto parsed = json::parse(*text);
                if (!parsed) {
                    error = std::format("tileset '{}' {}:{}: {}", path, parsed.error->line, parsed.error->column,
                                        parsed.error->message);
                    return false;
                }
                tileset_doc = std::move(parsed.value);
            }
            if (!parse_tileset(tileset_doc, dir_of(path), ts, error)) return false;
            ts.source = path;
        } else if (!parse_tileset(ref, base_dir, ts, error)) {
            return false;
        }
        ts.first_gid = static_cast<u32>(ref["firstgid"].as_int(1));
        out.tilesets.push_back(std::move(ts));
    }
    std::sort(out.tilesets.begin(), out.tilesets.end(), [](const Tileset& a, const Tileset& b) { return a.first_gid < b.first_gid; });

    // Object templates (.tj, .tx): an object that names one gets the template's fields, with its own winning.
    std::unordered_map<std::string, json::Value> templates;
    auto expand = [&](const json::Value& o) -> json::Value {
        if (!o.contains("template")) return o;
        std::string path = normalize_path(std::string(base_dir) + std::string(o["template"].as_string()));
        auto found = templates.find(path);
        if (found == templates.end()) {
            json::Value t;
            if (auto text = read_file(path)) {
                if (path.ends_with(".tx")) {
                    if (auto parsed = xml::parse(*text); parsed && parsed.root.name == "template") t = xml_template(parsed.root);
                } else if (auto parsed = json::parse(*text)) {
                    t = std::move(parsed.value);
                }
            }
            if (t.is_null()) warn("tilemap: can't read object template '{}'", path);
            found = templates.emplace(path, std::move(t)).first;
        }
        const json::Value& t = found->second;
        json::Value merged = t["object"];
        merged.make_object();
        // A tile template's gid is in its own tileset's numbering: move it onto the map's copy of that tileset.
        if (merged.contains("gid") && t["tileset"].contains("source")) {
            std::string source = normalize_path(dir_of(path) + std::string(t["tileset"]["source"].as_string()));
            u32 gid = static_cast<u32>(merged["gid"].as_int());
            u32 local = (gid & tile_gid_mask) - static_cast<u32>(t["tileset"]["firstgid"].as_int(1));
            for (const Tileset& ts : out.tilesets)
                if (ts.source == source) merged.set("gid", static_cast<f64>((ts.first_gid + local) | (gid & ~tile_gid_mask)));
        }
        for (const auto& [key, value] : o.object()) {
            if (key == "template") continue;
            if (key != "properties") {
                merged.set(key, value);
                continue;
            }
            // Properties one by one: the instance's replace the template's of the same name.
            json::Value props = merged["properties"];
            props.make_array();
            for (const json::Value& p : value.array()) {
                bool replaced = false;
                for (json::Value& q : props.make_array())
                    if (q["name"].as_string() == p["name"].as_string()) {
                        q = p;
                        replaced = true;
                    }
                if (!replaced) props.push(p);
            }
            merged.set("properties", std::move(props));
        }
        return merged;
    };
    return parse_layers(doc["layers"], {}, base_dir, out, expand, error);
}

bool Tilemap::parse_tmx(std::string_view text, std::string_view base_dir,
                        const std::function<std::optional<std::string>(const std::string&)>& read_file, Tilemap& out,
                        std::string& error) {
    auto parsed = xml::parse(text);
    if (!parsed) {
        error = std::format("XML error at {}:{}: {}", parsed.error->line, parsed.error->column, parsed.error->message);
        return false;
    }
    if (parsed.root.name != "map") {
        error = "not a Tiled map (no <map> element)";
        return false;
    }
    return parse(xml_map(parsed.root), base_dir, read_file, out, error);
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

namespace {

// The measurements of a staggered or hexagonal grid, as Tiled works them out. Staggered maps are hexagonal maps whose
// hexagons have no flat side (diamonds).
struct HexGrid {
    i32 tw, th, side_x, side_y, offset_x, offset_y, column_w, row_h;
    bool stagger_x, even;

    explicit HexGrid(const Tilemap& m) {
        tw = static_cast<i32>(m.tile_width & ~1u);
        th = static_cast<i32>(m.tile_height & ~1u);
        stagger_x = m.stagger_x;
        even = m.stagger_even;
        i32 side = static_cast<i32>(m.hex_side_length);
        side_x = stagger_x ? side : 0;
        side_y = stagger_x ? 0 : side;
        offset_x = (tw - side_x) / 2;
        offset_y = (th - side_y) / 2;
        column_w = offset_x + side_x;
        row_h = offset_y + side_y;
    }
    // Whether a column (stagger_x) or row is the shifted one.
    bool shifted(i32 index) const { return ((index & 1) != 0) != even; }

    Vec2 to_world(f32 x, f32 y) const {
        if (stagger_x)
            return {x * static_cast<f32>(column_w),
                    y * static_cast<f32>(th + side_y) + (shifted(cell_floor(x)) ? row_h : 0)};
        return {x * static_cast<f32>(tw + side_x) + (shifted(cell_floor(y)) ? column_w : 0),
                y * static_cast<f32>(row_h)};
    }

    // Tiled's neighbours of a cell in a staggered layout.
    std::pair<i32, i32> step(i32 x, i32 y, int dx, int dy) const {
        if (!stagger_x) {
            bool s = shifted(y);
            return {x + (dx < 0 ? (s ? 0 : -1) : (s ? 1 : 0)), y + dy};
        }
        bool s = shifted(x);
        return {x + dx, y + (dy < 0 ? (s ? 0 : -1) : (s ? 1 : 0))};
    }

    std::pair<i32, i32> diamond_at(Vec2 p) const {
        f32 x = p.x, y = p.y;
        if (stagger_x) x -= even ? static_cast<f32>(offset_x) : 0.0f;
        else y -= even ? static_cast<f32>(offset_y) : 0.0f;
        i32 rx = cell_floor(x / static_cast<f32>(tw));
        i32 ry = cell_floor(y / static_cast<f32>(th));
        f32 rel_x = x - static_cast<f32>(rx) * static_cast<f32>(tw), rel_y = y - static_cast<f32>(ry) * static_cast<f32>(th);
        i32& axis = stagger_x ? rx : ry;
        axis *= 2;
        if (even) ++axis;
        f32 y_pos = rel_x * static_cast<f32>(th) / static_cast<f32>(tw);
        f32 oy = static_cast<f32>(offset_y);
        // Inside the reference square the diamond is in the middle; the corners belong to its neighbours.
        if (oy - y_pos > rel_y) return step(rx, ry, -1, -1);
        if (-oy + y_pos > rel_y) return step(rx, ry, +1, -1);
        if (oy + y_pos < rel_y) return step(rx, ry, -1, +1);
        if (oy * 3 - y_pos < rel_y) return step(rx, ry, +1, +1);
        return {rx, ry};
    }

    std::pair<i32, i32> hexagon_at(Vec2 p) const {
        f32 x = p.x, y = p.y;
        if (stagger_x) x -= static_cast<f32>(even ? tw : offset_x);
        else y -= static_cast<f32>(even ? th : offset_y);
        f32 cw2 = static_cast<f32>(column_w * 2), rh2 = static_cast<f32>(row_h * 2);
        i32 rx = cell_floor(x / cw2), ry = cell_floor(y / rh2);
        Vec2 rel{x - static_cast<f32>(rx) * cw2, y - static_cast<f32>(ry) * rh2};
        i32& axis = stagger_x ? rx : ry;
        axis *= 2;
        if (even) ++axis;
        Vec2 centers[4];
        if (stagger_x) {
            f32 left = static_cast<f32>(side_x / 2), cx = left + static_cast<f32>(column_w), cy = static_cast<f32>(th / 2);
            centers[0] = {left, cy};
            centers[1] = {cx, cy - static_cast<f32>(row_h)};
            centers[2] = {cx, cy + static_cast<f32>(row_h)};
            centers[3] = {cx + static_cast<f32>(column_w), cy};
        } else {
            f32 top = static_cast<f32>(side_y / 2), cx = static_cast<f32>(tw / 2), cy = top + static_cast<f32>(row_h);
            centers[0] = {cx, top};
            centers[1] = {cx - static_cast<f32>(column_w), cy};
            centers[2] = {cx + static_cast<f32>(column_w), cy};
            centers[3] = {cx, cy + static_cast<f32>(row_h)};
        }
        int nearest = 0;
        f32 best = 1e30f;
        for (int i = 0; i < 4; ++i) {
            Vec2 d = centers[i] - rel;
            f32 dist = d.x * d.x + d.y * d.y;
            if (dist < best) {
                best = dist;
                nearest = i;
            }
        }
        static constexpr int stagger_x_offsets[4][2] = {{0, 0}, {1, -1}, {1, 0}, {2, 0}};
        static constexpr int stagger_y_offsets[4][2] = {{0, 0}, {-1, 1}, {0, 1}, {0, 2}};
        const int* o = stagger_x ? stagger_x_offsets[nearest] : stagger_y_offsets[nearest];
        return {rx + o[0], ry + o[1]};
    }
};

bool staggered_layout(const Tilemap& m) {
    return m.orientation == Tilemap::Orientation::Staggered || m.orientation == Tilemap::Orientation::Hexagonal;
}

} // namespace

Vec2 Tilemap::cell_to_world(f32 x, f32 y) const {
    f32 tw = static_cast<f32>(tile_width), th = static_cast<f32>(tile_height);
    if (orientation == Orientation::Isometric)
        return {(x - y) * tw * 0.5f + static_cast<f32>(height) * tw * 0.5f, (x + y) * th * 0.5f};
    if (staggered_layout(*this)) return HexGrid(*this).to_world(x, y);
    return {x * tw, y * th};
}

f32 Tilemap::sort_y(i32 x, i32 y) const {
    return cell_to_world(static_cast<f32>(x), static_cast<f32>(y)).y + static_cast<f32>(tile_height) * 0.5f;
}

Vec2 Tilemap::world_to_cell(Vec2 w) const {
    f32 tw = static_cast<f32>(tile_width), th = static_cast<f32>(tile_height);
    if (orientation == Orientation::Isometric) {
        f32 a = (w.x - static_cast<f32>(height) * tw * 0.5f) / (tw * 0.5f); // x - y
        f32 b = w.y / (th * 0.5f);                                          // x + y
        return {(a + b) * 0.5f, (b - a) * 0.5f};
    }
    if (staggered_layout(*this)) {
        auto [x, y] = cell_at(w);
        return {static_cast<f32>(x), static_cast<f32>(y)};
    }
    return {w.x / tw, w.y / th};
}

std::pair<i32, i32> Tilemap::cell_at(Vec2 w) const {
    if (orientation == Orientation::Staggered) return HexGrid(*this).diamond_at(w);
    if (orientation == Orientation::Hexagonal) return HexGrid(*this).hexagon_at(w);
    Vec2 c = world_to_cell(w);
    return {cell_floor(c.x), cell_floor(c.y)};
}

Vec2 Tilemap::pixel_size() const {
    f32 w = static_cast<f32>(width), h = static_cast<f32>(height);
    f32 tw = static_cast<f32>(tile_width), th = static_cast<f32>(tile_height);
    switch (orientation) {
    case Orientation::Orthogonal: return {w * tw, h * th};
    case Orientation::Isometric: return {(w + h) * tw * 0.5f, (w + h) * th * 0.5f};
    case Orientation::Staggered:
    case Orientation::Hexagonal: {
        HexGrid g(*this);
        if (g.stagger_x) {
            Vec2 size{w * static_cast<f32>(g.column_w) + static_cast<f32>(g.offset_x), h * static_cast<f32>(g.th + g.side_y)};
            if (width > 1) size.y += static_cast<f32>(g.row_h);
            return size;
        }
        Vec2 size{w * static_cast<f32>(g.tw + g.side_x), h * static_cast<f32>(g.row_h) + static_cast<f32>(g.offset_y)};
        if (height > 1) size.x += static_cast<f32>(g.column_w);
        return size;
    }
    }
    return {};
}

// Drawing

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

// What draw_tilemap and TilemapCache share: which layers to draw, at what depth, and how to draw a cell.
struct MapDraw {
    const Tilemap& map;
    const TilemapDrawOptions& o;
    bool iso;
    f32 tw, th, pivot_x, overhang = 0;
    bool any_ysort_property;
    static constexpr f32 underneath = -1e7f; // flat layers under everything y-sorted
    static constexpr f32 overhead = 1e7f;    // "above" layers over it

    MapDraw(const Tilemap& m, const TilemapDrawOptions& options) : map(m), o(options) {
        iso = map.orientation == Tilemap::Orientation::Isometric;
        tw = static_cast<f32>(map.tile_width);
        th = static_cast<f32>(map.tile_height);
        pivot_x = iso ? 0.5f : 0.0f;
        // Tall tiles reach above their cell: widen culling by the tallest tile.
        for (const Tileset& ts : map.tilesets) {
            overhang = std::max(overhang, static_cast<f32>(ts.tile_height) + std::abs(ts.tile_offset.y));
            for (const TileInfo& t : ts.tiles) overhang = std::max(overhang, t.image_size.y);
        }
        any_ysort_property = std::any_of(map.layers.begin(), map.layers.end(),
                                         [](const MapLayer& l) { return l.properties["ysort"].is_bool(); });
    }

    // A layer with the bool property "above" draws over everything y-sorted (roofs, treetops, arches); it never sorts.
    bool above(usize li) const { return o.y_sort && map.layers[li].properties["above"].as_bool(); }
    bool sorts(usize li) const {
        if (above(li)) return false;
        return o.y_sort && (any_ysort_property ? map.layers[li].properties["ysort"].as_bool() : li > 0);
    }
    // Drawn with the sprites every frame (y-sorted or above them), rather than cached under them.
    bool with_sprites(usize li) const { return sorts(li) || above(li); }
    bool wanted(usize li) const {
        const MapLayer& layer = map.layers[li];
        if (!layer.visible || layer.opacity <= 0) return false;
        if (!o.only_layer.empty() && layer.name != o.only_layer) return false;
        if (o.layers == TilemapDrawOptions::Layers::Flat && with_sprites(li)) return false;
        if (o.layers == TilemapDrawOptions::Layers::Sorted && !with_sprites(li)) return false;
        return true;
    }
    f32 layer_depth(usize li) const {
        f32 band = !o.y_sort || sorts(li) ? 0.0f : above(li) ? overhead : underneath;
        return o.depth + static_cast<f32>(li) * o.layer_depth_step + band;
    }
    Color color(usize li) const {
        const MapLayer& l = map.layers[li];
        return {l.tint.r, l.tint.g, l.tint.b, l.tint.a * l.opacity};
    }
    Vec2 base(usize li) const { return o.position + map.layers[li].offset; }
    bool animated(u32 gid) const {
        const TileInfo* info = map.tile_info(gid);
        return info && !info->animation.empty();
    }

    void cell(SpriteBatch& batch, usize li, i32 x, i32 y, f32 time) const {
        u32 gid = map.layers[li].at(x, y);
        if (!(gid & tile_gid_mask)) return;
        const Tileset* ts = map.tileset_for(gid);
        if (!ts) return;
        TileImage img = image_for(*ts, (gid & tile_gid_mask) - ts->first_gid, time);
        if (!img.texture) return;
        // Bottom of the cell: the diamond's bottom corner (isometric) or the bottom-left of the cell's box.
        Vec2 anchor = map.cell_to_world(static_cast<f32>(x), static_cast<f32>(y)) + Vec2{0, th} + base(li) + ts->tile_offset;
        // Sorted by the middle of the cell, not the bottom of the picture (see Tilemap::sort_y).
        f32 depth = sorts(li) ? o.depth + map.sort_y(x, y) + base(li).y + ts->tile_offset.y : layer_depth(li);
        draw_tile_image(batch, img, gid, anchor, pivot_x, img.source.size(), color(li), depth);
    }

    // Image layers and tile objects (other object shapes are data).
    void other(SpriteBatch& batch, usize li) const {
        const MapLayer& layer = map.layers[li];
        if (layer.kind == MapLayer::Kind::Image) {
            if (!layer.image) return;
            SpriteInstance s;
            s.position = base(li);
            s.size = layer.image->size();
            s.origin = {0, 0};
            s.color = color(li);
            s.depth = layer_depth(li);
            batch.draw(*layer.image, s);
            return;
        }
        for (const MapObject& obj : layer.objects) {
            if (!obj.visible || obj.shape != MapObject::Shape::Tile) continue;
            const Tileset* ts = map.tileset_for(obj.gid);
            if (!ts) continue;
            TileImage img = image_for(*ts, (obj.gid & tile_gid_mask) - ts->first_gid, o.time);
            if (!img.texture) continue;
            // Tiled object coordinates in isometric maps are in "tile height" units along both grid axes.
            Vec2 at = iso ? map.cell_to_world(obj.bounds.x / th, obj.bounds.y / th) : obj.bounds.position();
            at += base(li) + ts->tile_offset;
            Vec2 size = obj.bounds.w > 0 ? obj.bounds.size() : img.source.size();
            f32 depth = sorts(li) ? o.depth + at.y : layer_depth(li);
            draw_tile_image(batch, img, obj.gid, at, pivot_x, size, color(li), depth, obj.rotation);
        }
    }

    // Cells of a tile layer whose images can touch `view` (world coordinates): x0, y0, x1, y1 inclusive.
    std::array<i32, 4> cells_in(usize li, Rect view) const {
        const MapLayer& layer = map.layers[li];
        Vec2 b = base(li);
        Rect padded{view.x - b.x - tw, view.y - b.y - th, view.w + 2 * tw, view.h + 2 * th + overhang};
        Vec2 corners[4] = {map.world_to_cell(padded.position()), map.world_to_cell({padded.right(), padded.y}),
                           map.world_to_cell({padded.x, padded.bottom()}), map.world_to_cell({padded.right(), padded.bottom()})};
        f32 min_x = corners[0].x, max_x = corners[0].x, min_y = corners[0].y, max_y = corners[0].y;
        for (Vec2 c : corners) {
            min_x = std::min(min_x, c.x);
            max_x = std::max(max_x, c.x);
            min_y = std::min(min_y, c.y);
            max_y = std::max(max_y, c.y);
        }
        i32 margin = staggered_layout(map) ? 1 : 0; // whole cells, so one more around the edge
        return {std::max(layer.origin_x, cell_floor(min_x) - margin),
                std::max(layer.origin_y, cell_floor(min_y) - margin),
                std::min(layer.origin_x + static_cast<i32>(layer.width) - 1, cell_ceil(max_x) + margin),
                std::min(layer.origin_y + static_cast<i32>(layer.height) - 1, cell_ceil(max_y) + margin)};
    }

    // Cells in the order Tiled draws them: row by row, and in maps staggered along x, the raised columns of each row
    // before the lowered ones so they overlap correctly.
    template <class F> void each_cell(std::array<i32, 4> r, F&& f) const {
        bool two_passes = staggered_layout(map) && map.stagger_x;
        HexGrid grid(map);
        for (i32 y = r[1]; y <= r[3]; ++y)
            for (int pass = 0; pass < (two_passes ? 2 : 1); ++pass)
                for (i32 x = r[0]; x <= r[2]; ++x) {
                    if (two_passes && grid.shifted(x) != (pass == 1)) continue;
                    f(x, y);
                }
    }
};

} // namespace

void draw_maps(ecs::World& world, SpriteBatch& batch, f32 time, const std::function<bool(ecs::Entity)>& skip) {
    world.each<const Map, const ecs::GlobalTransform2D>([&](ecs::Entity e, const Map& m, const ecs::GlobalTransform2D& g) {
        if (!m.visible || !m.map || (skip && skip(e))) return;
        TilemapDrawOptions options;
        options.position = g.position;
        options.time = time;
        options.y_sort = m.y_sort;
        options.depth = m.depth + (m.y_sort ? 0.0f : -1e7f); // not sorted: all of it under the sprites, like ground
        draw_tilemap(batch, *m.map, options);
    });
}

void draw_tilemap(SpriteBatch& batch, const Tilemap& map, const TilemapDrawOptions& o) {
    MapDraw draw(map, o);
    Rect view = batch.camera().visible_bounds();
    for (usize li = 0; li < map.layers.size(); ++li) {
        if (!draw.wanted(li)) continue;
        if (map.layers[li].kind != MapLayer::Kind::Tiles) {
            draw.other(batch, li);
            continue;
        }
        draw.each_cell(draw.cells_in(li, view), [&](i32 x, i32 y) { draw.cell(batch, li, x, y, o.time); });
    }
}

// TilemapCache

u64 TilemapCache::signature(const Tilemap& map, const TilemapDrawOptions& o) const {
    u64 h = 1469598103934665603ull;
    auto mix = [&](u64 v) { h = (h ^ v) * 1099511628211ull; };
    auto mix_f = [&](f32 v) {
        u32 bits;
        std::memcpy(&bits, &v, 4);
        mix(bits);
    };
    mix(reinterpret_cast<uintptr_t>(&map));
    mix(map.layers.size());
    for (const Tileset& ts : map.tilesets) {
        mix(reinterpret_cast<uintptr_t>(ts.image.get()));
        mix(ts.image.version());
        for (const TileInfo& t : ts.tiles) mix(reinterpret_cast<uintptr_t>(t.image.get()) ^ t.image.version());
    }
    mix_f(o.position.x);
    mix_f(o.position.y);
    mix_f(o.depth);
    mix_f(o.layer_depth_step);
    mix(o.y_sort);
    mix(std::hash<std::string_view>{}(o.only_layer));
    return h;
}

void TilemapCache::invalidate() {
    built_for_ = 0;
    chunks_.clear();
    animated_.clear();
}

void TilemapCache::build(GpuDevice& device, const Tilemap& map, const TilemapDrawOptions& o) {
    BOWL_PROFILE_SCOPE("TilemapCache::build");
    chunks_.clear();
    animated_.clear();
    MapDraw draw(map, o);
    for (usize li = 0; li < map.layers.size(); ++li) {
        const MapLayer& layer = map.layers[li];
        if (!draw.wanted(li) || layer.kind != MapLayer::Kind::Tiles) continue;
        i32 x_end = layer.origin_x + static_cast<i32>(layer.width), y_end = layer.origin_y + static_cast<i32>(layer.height);
        for (i32 cy = layer.origin_y; cy < y_end; cy += chunk_cells)
            for (i32 cx = layer.origin_x; cx < x_end; cx += chunk_cells) {
                scratch_.begin({}, SpriteSort::Depth);
                std::array<i32, 4> range{cx, cy, std::min(cx + chunk_cells, x_end) - 1, std::min(cy + chunk_cells, y_end) - 1};
                draw.each_cell(range, [&](i32 x, i32 y) {
                    u32 gid = layer.at(x, y);
                    if (!(gid & tile_gid_mask)) return;
                    if (draw.animated(gid)) animated_.push_back({li, x, y});
                    else draw.cell(scratch_, li, x, y, 0);
                });
                if (scratch_.size() == 0) continue;
                auto chunk = std::make_unique<Chunk>();
                chunk->layer = li;
                if (chunk->sprites.build(device, scratch_)) chunks_.push_back(std::move(chunk));
            }
    }
}

void TilemapCache::submit(Renderer& renderer, GpuDevice& device, const Camera2D& camera, const Tilemap& map,
                          const TilemapDrawOptions& options) {
    TilemapDrawOptions o = options;
    o.layers = TilemapDrawOptions::Layers::Flat;
    if (u64 sig = signature(map, o); sig != built_for_ || built_for_ == 0) {
        build(device, map, o);
        built_for_ = sig;
    }
    MapDraw draw(map, o);
    Rect view = camera.visible_bounds();
    usize next_chunk = 0, next_animated = 0;
    for (usize li = 0; li < map.layers.size(); ++li) {
        if (!draw.wanted(li)) continue;
        for (; next_chunk < chunks_.size() && chunks_[next_chunk]->layer == li; ++next_chunk)
            if (chunks_[next_chunk]->sprites.bounds().overlaps(view)) renderer.submit(chunks_[next_chunk]->sprites, camera);
        // What changes every frame goes in a batch drawn right after the layer's chunks.
        moving_.begin(camera, SpriteSort::Depth);
        if (map.layers[li].kind != MapLayer::Kind::Tiles) draw.other(moving_, li);
        for (; next_animated < animated_.size() && animated_[next_animated].layer == li; ++next_animated) {
            const Animated& a = animated_[next_animated];
            Vec2 at = map.cell_to_world(static_cast<f32>(a.x), static_cast<f32>(a.y)) + draw.base(li);
            Rect reach{at.x - draw.tw, at.y - draw.overhang, draw.tw * 3, draw.overhang + draw.th * 2};
            if (reach.overlaps(view)) draw.cell(moving_, li, a.x, a.y, o.time);
        }
        if (moving_.size() > 0) renderer.submit(moving_);
    }
}

// Loader

void register_tilemap_loader(AssetManager& assets) {
    assets.register_loader<Tilemap>([](AssetLoadContext& ctx) -> std::unique_ptr<Tilemap> {
        auto text = ctx.read_text(ctx.path());
        if (!text) {
            log_asset_error(ctx.path(), "Tilemap", "file not found in any mount");
            return nullptr;
        }
        auto map = std::make_unique<Tilemap>();
        std::string error;
        auto read = [&](const std::string& path) { return ctx.read_text(path); }; // tracked: edits hot-reload
        bool ok = false;
        if (ctx.path().ends_with(".tmx")) {
            ok = Tilemap::parse_tmx(*text, dir_of(ctx.path()), read, *map, error);
        } else {
            auto parsed = json::parse(*text);
            if (!parsed) {
                log_asset_error(ctx.path(), "Tilemap",
                                std::format("JSON error at {}:{}: {}", parsed.error->line, parsed.error->column, parsed.error->message));
                return nullptr;
            }
            ok = Tilemap::parse(parsed.value, dir_of(ctx.path()), read, *map, error);
        }
        if (!ok) {
            log_asset_error(ctx.path(), "Tilemap", error);
            return nullptr;
        }
        AssetManager& manager = ctx.assets();
        for (Tileset& ts : map->tilesets) {
            if (!ts.image_path.empty()) ts.image = manager.load<Texture>(ts.image_path);
            for (TileInfo& t : ts.tiles)
                if (!t.image_path.empty()) t.image = manager.load<Texture>(t.image_path);
        }
        for (MapLayer& l : map->layers)
            if (!l.image_path.empty()) l.image = manager.load<Texture>(l.image_path);
        return map;
    });
}

} // namespace bowl::gfx
