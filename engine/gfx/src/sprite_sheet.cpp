#include "ph/gfx/sprite_sheet.hpp"

#include "ph/core/log.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace ph::gfx {

const SpriteAnimation* SpriteSheet::find(std::string_view name) const {
    i32 i = index_of(name);
    return i < 0 ? nullptr : &animations[static_cast<usize>(i)];
}

i32 SpriteSheet::index_of(std::string_view name) const {
    for (usize i = 0; i < animations.size(); ++i)
        if (animations[i].name == name) return static_cast<i32>(i);
    return -1;
}

namespace {

Rect rect_of(const json::Value& v) {
    return {v["x"].as_float(), v["y"].as_float(), v["w"].as_float(), v["h"].as_float()};
}

u32 repeat_of(const json::Value& v) {
    // Aseprite writes "repeat" as a string ("3"); accept numbers too.
    if (v.is_number()) return static_cast<u32>(std::max<i64>(0, v.as_int()));
    std::string_view s = v.as_string();
    u32 n = 0;
    std::from_chars(s.data(), s.data() + s.size(), n);
    return n;
}

SpriteFrame frame_of(const json::Value& f) {
    SpriteFrame frame;
    frame.source = rect_of(f["frame"]);
    const json::Value& sss = f["spriteSourceSize"];
    frame.offset = {sss["x"].as_float(), sss["y"].as_float()};
    const json::Value& src = f["sourceSize"];
    frame.size = {src["w"].as_float(frame.source.w), src["h"].as_float(frame.source.h)};
    frame.duration = static_cast<f32>(f["duration"].as_number(100)) / 1000.0f;
    return frame;
}

} // namespace

bool SpriteSheet::parse_aseprite(const json::Value& doc, SpriteSheet& out, std::string& image, std::string& error) {
    const json::Value& frames = doc["frames"];
    if (frames.is_array()) {
        for (const json::Value& f : frames.array()) out.frames.push_back(frame_of(f));
    } else if (frames.is_object()) { // "hash" layout: keys are file names, in frame order
        for (const auto& [name, f] : frames.object()) out.frames.push_back(frame_of(f));
    }
    if (out.frames.empty()) {
        error = "no frames (expected Aseprite sprite-sheet JSON)";
        return false;
    }
    for (const SpriteFrame& f : out.frames)
        if (f.source.w <= 0 || f.source.h <= 0) {
            error = "a frame has no size";
            return false;
        }

    const json::Value& meta = doc["meta"];
    image = std::string(meta["image"].as_string());
    u32 last = static_cast<u32>(out.frames.size() - 1);
    for (const json::Value& tag : meta["frameTags"].array()) {
        SpriteAnimation a;
        a.name = std::string(tag["name"].as_string());
        a.from = std::min(static_cast<u32>(tag["from"].as_int()), last);
        a.to = std::clamp(static_cast<u32>(tag["to"].as_int()), a.from, last);
        std::string_view dir = tag["direction"].as_string("forward");
        if (dir == "reverse") a.direction = SpriteAnimation::Direction::Reverse;
        else if (dir == "pingpong") a.direction = SpriteAnimation::Direction::PingPong;
        else if (dir == "pingpong_reverse") a.direction = SpriteAnimation::Direction::PingPongReverse;
        a.repeat = repeat_of(tag["repeat"]);
        out.animations.push_back(std::move(a));
    }
    // Without tags, the whole sheet is one looping animation.
    if (out.animations.empty()) out.animations.push_back({"default", 0, last});
    return true;
}

SpriteSheet SpriteSheet::grid(Asset<Texture> texture, u32 cell_width, u32 cell_height, u32 count, f32 frame_seconds) {
    SpriteSheet sheet;
    u32 columns = texture ? std::max(1u, texture->width() / cell_width) : 1;
    for (u32 i = 0; i < count; ++i) {
        SpriteFrame f;
        f.source = {static_cast<f32>((i % columns) * cell_width), static_cast<f32>((i / columns) * cell_height),
                    static_cast<f32>(cell_width), static_cast<f32>(cell_height)};
        f.size = f.source.size();
        f.duration = frame_seconds;
        sheet.frames.push_back(f);
    }
    sheet.animations.push_back({"default", 0, count ? count - 1 : 0});
    sheet.texture = std::move(texture);
    return sheet;
}

// SpriteAnimator

bool SpriteAnimator::play(std::string_view name, bool restart) {
    if (!sheet) return false;
    i32 index = sheet->index_of(name);
    if (index < 0) return false;
    if (!restart && static_cast<u32>(index) == animation && playing && !finished) return true;
    const SpriteAnimation& a = sheet->animations[static_cast<usize>(index)];
    animation = static_cast<u32>(index);
    bool backwards = a.direction == SpriteAnimation::Direction::Reverse ||
                     a.direction == SpriteAnimation::Direction::PingPongReverse;
    frame = backwards ? a.to : a.from;
    step = backwards ? -1 : 1;
    time = 0;
    loops_done = 0;
    playing = true;
    finished = false;
    frame_changed = true;
    return true;
}

void SpriteAnimator::update(f32 dt) {
    frame_changed = false;
    if (!sheet || !playing || finished || sheet->animations.empty()) return;
    const SpriteAnimation& a = sheet->animations[std::min<usize>(animation, sheet->animations.size() - 1)];
    frame = std::clamp(frame, a.from, a.to);
    time += dt * speed;
    // Advance as many frames as the elapsed time covers (low frame rates or fast speeds skip correctly).
    for (int guard = 0; guard < 1024; ++guard) {
        f32 duration = std::max(sheet->frames[frame].duration, 1e-4f);
        if (time < duration) break;
        time -= duration;
        bool ping = a.direction == SpriteAnimation::Direction::PingPong ||
                    a.direction == SpriteAnimation::Direction::PingPongReverse;
        u32 first = step > 0 ? a.from : a.to, last = step > 0 ? a.to : a.from;
        if (frame != last) {
            frame = static_cast<u32>(static_cast<i64>(frame) + step);
        } else if (ping && a.from != a.to) {
            // Turning around; a full loop is there and back.
            bool back_at_start = (a.direction == SpriteAnimation::Direction::PingPong) == (step < 0);
            step = static_cast<i8>(-step);
            if (back_at_start && a.repeat && ++loops_done >= a.repeat) {
                finished = true;
                break;
            }
            frame = static_cast<u32>(static_cast<i64>(frame) + step);
        } else {
            if (a.repeat && ++loops_done >= a.repeat) {
                finished = true;
                break;
            }
            frame = first;
        }
        frame_changed = true;
    }
}

// Drawing

void draw_frame(SpriteBatch& batch, const SpriteSheet& sheet, u32 frame, Vec2 position, Vec2 scale, Color color,
                f32 depth, f32 rotation) {
    if (!sheet.texture || frame >= sheet.frames.size()) return;
    const Texture& tex = *sheet.texture;
    const SpriteFrame& f = sheet.frames[frame];
    Vec2 inv{1.0f / static_cast<f32>(tex.width()), 1.0f / static_cast<f32>(tex.height())};
    SpriteInstance s;
    s.position = position;
    s.size = {f.source.w * std::abs(scale.x), f.source.h * std::abs(scale.y)};
    // The pivot is defined on the full frame; express it relative to the trimmed piece so rotation and scale
    // happen around it.
    Vec2 pivot_px = f.size * sheet.pivot;
    Vec2 origin{(pivot_px.x - f.offset.x) / f.source.w, (pivot_px.y - f.offset.y) / f.source.h};
    Vec4 uv{f.source.x * inv.x, f.source.y * inv.y, f.source.right() * inv.x, f.source.bottom() * inv.y};
    if (scale.x < 0) {
        std::swap(uv.x, uv.z);
        origin.x = 1.0f - origin.x;
    }
    if (scale.y < 0) {
        std::swap(uv.y, uv.w);
        origin.y = 1.0f - origin.y;
    }
    s.origin = origin;
    s.uv = uv;
    s.rotation = rotation;
    s.depth = depth;
    s.color = color;
    batch.draw(tex, s);
}

void register_sprite_sheet_loader(AssetManager& assets) {
    assets.register_loader<SpriteSheet>([](AssetLoadContext& ctx) -> std::unique_ptr<SpriteSheet> {
        auto text = ctx.read_text(ctx.path());
        if (!text) {
            log_asset_error(ctx.path(), "SpriteSheet", "file not found in any mount");
            return nullptr;
        }
        auto parsed = json::parse(*text);
        if (!parsed) {
            log_asset_error(ctx.path(), "SpriteSheet",
                            std::format("JSON error at {}:{}: {}", parsed.error->line, parsed.error->column,
                                        parsed.error->message));
            return nullptr;
        }
        auto sheet = std::make_unique<SpriteSheet>();
        std::string image, err;
        if (!SpriteSheet::parse_aseprite(parsed.value, *sheet, image, err)) {
            log_asset_error(ctx.path(), "SpriteSheet", err);
            return nullptr;
        }
        std::string_view path = ctx.path();
        usize slash = path.rfind('/');
        std::string dir = slash == std::string_view::npos ? "" : std::string(path.substr(0, slash + 1));
        sheet->texture = ctx.assets().load<Texture>(dir + image);
        if (!sheet->texture) log_asset_error(ctx.path(), "SpriteSheet", "its image '" + dir + image + "' failed to load");
        return sheet;
    });
}

} // namespace ph::gfx
