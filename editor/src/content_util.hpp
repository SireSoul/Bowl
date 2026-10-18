#pragma once

// Small helpers the Content workspace's map files share: Tiled maps read as JSON.

#include "bowl/app/engine.hpp"
#include "bowl/core/json.hpp"

#include <imgui.h>

#include <cmath>

namespace bowl::editor::content_util {

using json::Value;

inline ImTextureID texture_id(const gfx::Texture& t) { return static_cast<ImTextureID>(reinterpret_cast<intptr_t>(t.handle())); }


// Map pixels per cell for object positions: isometric maps measure both axes in tile heights.
inline Vec2 per_cell(const Value& map) {
    f32 tw = map["tilewidth"].as_float(32), th = map["tileheight"].as_float(16);
    return {map["orientation"].as_string() == "isometric" ? th : tw, th};
}

// The index of the "markers" object layer in the map's layers, or -1.
inline i64 markers_layer(const Value& map) {
    const Value& layers = map["layers"];
    for (usize i = 0; i < layers.size(); ++i)
        if (layers[i]["name"].as_string() == "markers" && layers[i]["type"].as_string() == "objectgroup") return static_cast<i64>(i);
    return -1;
}

inline bool is_point(const Value& o) { return o["point"].as_bool() || (o["width"].as_number() == 0 && o["height"].as_number() == 0); }

// Where an object is, in cells: a point's position, or an area's rectangle.
inline Rect cells_of(const Value& o, Vec2 per) {
    return {o["x"].as_float() / per.x, o["y"].as_float() / per.y, o["width"].as_float() / per.x, o["height"].as_float() / per.y};
}

// Tiled's custom properties ([{name, type, value}]) as one object.
inline json::Value properties_of(const Value& o) {
    Value out;
    out.make_object();
    for (const Value& p : o["properties"].array()) out.set(p["name"].as_string(), p["value"]);
    return out;
}

inline f64 round2(f64 v) { return std::round(v * 100) / 100; }


} // namespace bowl::editor::content_util
