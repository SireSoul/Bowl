#include "ph/atlas/atlas.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <map>
#include <numeric>

namespace ph::atlas {

namespace {

struct Box {
    u32 x = 0, y = 0, w = 0, h = 0; // the part of the image worth keeping
};

Box opaque_part(const Image& img, bool trim) {
    if (!trim) return {0, 0, img.width, img.height};
    u32 x0 = img.width, y0 = img.height, x1 = 0, y1 = 0;
    for (u32 y = 0; y < img.height; ++y)
        for (u32 x = 0; x < img.width; ++x)
            if (img.rgba[(static_cast<usize>(y) * img.width + x) * 4 + 3] != 0) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x + 1);
                y1 = std::max(y1, y + 1);
            }
    if (x1 <= x0) return {0, 0, 1, 1}; // fully transparent: keep one pixel so it still has a frame
    return {x0, y0, x1 - x0, y1 - y0};
}

// Shelf packing: tallest first, left to right, a new shelf when a row is full. Returns the height used, or 0 if
// something is wider than `width`.
u32 shelves(const std::vector<Box>& boxes, const std::vector<usize>& order, u32 width, u32 padding,
            std::vector<std::pair<u32, u32>>* positions) {
    u32 x = padding, y = padding, shelf = 0;
    for (usize i : order) {
        const Box& b = boxes[i];
        if (b.w + 2 * padding > width) return 0;
        if (x + b.w + padding > width) {
            x = padding;
            y += shelf + padding;
            shelf = 0;
        }
        if (positions) (*positions)[i] = {x, y};
        x += b.w + padding;
        shelf = std::max(shelf, b.h);
    }
    return y + shelf + padding;
}

u32 next_pow2(u32 v) {
    u32 p = 1;
    while (p < v) p <<= 1;
    return p;
}

} // namespace

std::optional<Atlas> pack(const std::vector<Image>& images, const Options& options, std::string& error) {
    if (images.empty()) {
        error = "no images to pack";
        return std::nullopt;
    }
    std::vector<Box> boxes;
    for (const Image& img : images) {
        if (img.rgba.size() < static_cast<usize>(img.width) * img.height * 4 || img.width == 0 || img.height == 0) {
            error = std::format("{} has no pixels", img.name);
            return std::nullopt;
        }
        boxes.push_back(opaque_part(img, options.trim));
    }
    std::vector<usize> order(images.size());
    std::iota(order.begin(), order.end(), usize{0});
    std::stable_sort(order.begin(), order.end(), [&](usize a, usize b) {
        return boxes[a].h != boxes[b].h ? boxes[a].h > boxes[b].h : boxes[a].w > boxes[b].w;
    });

    // Try each power-of-two width; keep the smallest area (then the squarer shape).
    u32 widest = 0;
    for (const Box& b : boxes) widest = std::max(widest, b.w + 2 * options.padding);
    u32 best_w = 0, best_h = 0;
    for (u32 w = next_pow2(std::max(widest, 16u)); w <= options.max_size; w <<= 1) {
        u32 h = shelves(boxes, order, w, options.padding, nullptr);
        if (h == 0 || h > options.max_size) continue;
        u64 area = static_cast<u64>(w) * h, best_area = static_cast<u64>(best_w) * best_h;
        if (!best_w || area < best_area || (area == best_area && std::max(w, h) < std::max(best_w, best_h))) {
            best_w = w;
            best_h = h;
        }
    }
    if (!best_w) {
        error = std::format("the pictures don't fit in {} x {}; raise the maximum size or split them", options.max_size,
                            options.max_size);
        return std::nullopt;
    }

    std::vector<std::pair<u32, u32>> positions(images.size());
    shelves(boxes, order, best_w, options.padding, &positions);
    Atlas atlas;
    atlas.width = best_w;
    atlas.height = best_h;
    atlas.rgba.assign(static_cast<usize>(best_w) * best_h * 4, 0);
    for (usize i = 0; i < images.size(); ++i) {
        const Image& img = images[i];
        const Box& b = boxes[i];
        auto [px, py] = positions[i];
        for (u32 row = 0; row < b.h; ++row)
            std::memcpy(&atlas.rgba[(static_cast<usize>(py + row) * best_w + px) * 4],
                        &img.rgba[(static_cast<usize>(b.y + row) * img.width + b.x) * 4], static_cast<usize>(b.w) * 4);
        atlas.placed.push_back({i, Rect{static_cast<f32>(px), static_cast<f32>(py), static_cast<f32>(b.w), static_cast<f32>(b.h)},
                                Vec2{static_cast<f32>(b.x), static_cast<f32>(b.y)},
                                Vec2{static_cast<f32>(img.width), static_cast<f32>(img.height)}});
    }
    return atlas;
}

json::Value sheet_json(const Atlas& atlas, const std::vector<Image>& images, std::string_view image_file, u32 frame_ms) {
    json::Value frames;
    frames.make_object();
    auto rect = [](f32 x, f32 y, f32 w, f32 h) {
        json::Value r;
        r.set("x", x);
        r.set("y", y);
        r.set("w", w);
        r.set("h", h);
        return r;
    };
    for (const Placed& p : atlas.placed) {
        json::Value f;
        f.set("frame", rect(p.source.x, p.source.y, p.source.w, p.source.h));
        f.set("rotated", false);
        f.set("trimmed", p.source.w != p.size.x || p.source.h != p.size.y);
        f.set("spriteSourceSize", rect(p.offset.x, p.offset.y, p.source.w, p.source.h));
        json::Value size;
        size.set("w", p.size.x);
        size.set("h", p.size.y);
        f.set("sourceSize", std::move(size));
        f.set("duration", static_cast<i64>(frame_ms));
        frames.set(images[p.image].name, std::move(f));
    }

    // "walk_0", "walk_1", ... in the order frames were written (input order) become the "walk" tag.
    std::map<std::string, std::vector<std::pair<u32, u32>>> groups; // name -> (number, frame index)
    for (usize i = 0; i < atlas.placed.size(); ++i) {
        std::string_view name = images[atlas.placed[i].image].name;
        usize split = name.find_last_of("_ ");
        if (split == std::string_view::npos || split == 0 || split + 1 >= name.size()) continue;
        u32 number = 0;
        auto [end, ec] = std::from_chars(name.data() + split + 1, name.data() + name.size(), number);
        if (ec != std::errc{} || end != name.data() + name.size()) continue;
        groups[std::string(name.substr(0, split))].push_back({number, static_cast<u32>(i)});
    }
    json::Value tags;
    tags.make_array();
    for (auto& [name, list] : groups) {
        std::sort(list.begin(), list.end());
        // A tag needs consecutive frames; they are when the input lists them in order (ph-atlas sorts by name).
        bool consecutive = true;
        for (usize k = 1; k < list.size(); ++k) consecutive &= list[k].second == list[k - 1].second + 1;
        if (!consecutive || list.size() < 2) continue;
        json::Value tag;
        tag.set("name", name);
        tag.set("from", static_cast<i64>(list.front().second));
        tag.set("to", static_cast<i64>(list.back().second));
        tag.set("direction", "forward");
        tags.push(std::move(tag));
    }

    json::Value meta;
    meta.set("app", "ph-atlas");
    meta.set("image", std::string(image_file));
    meta.set("format", "RGBA8888");
    json::Value size;
    size.set("w", static_cast<i64>(atlas.width));
    size.set("h", static_cast<i64>(atlas.height));
    meta.set("size", std::move(size));
    meta.set("frameTags", std::move(tags));

    json::Value doc;
    doc.set("frames", std::move(frames));
    doc.set("meta", std::move(meta));
    return doc;
}

} // namespace ph::atlas
