#include "ph/gfx/text.hpp"

#include "ph/core/log.hpp"
#include "ph/core/utf8.hpp"

#include <algorithm>
#include <cmath>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace ph::gfx {

struct Font::Impl {
    std::vector<u8> data;
    stbtt_fontinfo info{};
};

Font::~Font() = default;

std::unique_ptr<Font> Font::from_memory(GpuDevice* device, std::vector<u8> ttf, std::string_view name) {
    auto font = std::unique_ptr<Font>(new Font());
    font->impl_ = std::make_unique<Impl>();
    font->impl_->data = std::move(ttf);
    const unsigned char* data = font->impl_->data.data();
    int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(&font->impl_->info, data, offset)) {
        error("font '{}': not a TrueType/OpenType font", name);
        return nullptr;
    }
    font->device_ = device;
    font->name_ = std::string(name);
    return font;
}

static u32 size_px(f32 pixel_size) { return std::max(1u, static_cast<u32>(std::lround(pixel_size))); }

Font::Metrics Font::metrics(f32 pixel_size) const {
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&impl_->info, &ascent, &descent, &gap);
    f32 scale = stbtt_ScaleForPixelHeight(&impl_->info, static_cast<f32>(size_px(pixel_size)));
    return {ascent * scale, descent * scale, gap * scale};
}

f32 Font::kerning(u32 left, u32 right, f32 pixel_size) const {
    f32 scale = stbtt_ScaleForPixelHeight(&impl_->info, static_cast<f32>(size_px(pixel_size)));
    return stbtt_GetCodepointKernAdvance(&impl_->info, static_cast<int>(left), static_cast<int>(right)) * scale;
}

bool Font::place(u32 w, u32 h, u16& page, u32& x, u32& y) {
    if (w + 2 > page_size || h + 2 > page_size) return false;
    if (pages_.empty()) pages_.push_back({std::vector<u8>(page_size * page_size * 4, 0)});
    Page* p = &pages_.back();
    if (p->shelf_x + w + 1 > page_size) { // next shelf
        p->shelf_y += p->shelf_h + 1;
        p->shelf_x = 1;
        p->shelf_h = 0;
    }
    if (p->shelf_y + h + 1 > page_size) { // next page
        pages_.push_back({std::vector<u8>(page_size * page_size * 4, 0)});
        p = &pages_.back();
    }
    page = static_cast<u16>(pages_.size() - 1);
    x = p->shelf_x;
    y = p->shelf_y;
    p->shelf_x += w + 1;
    p->shelf_h = std::max(p->shelf_h, h);
    p->dirty = true;
    return true;
}

const Font::Glyph& Font::glyph(u32 codepoint, f32 pixel_size) {
    u32 px = size_px(pixel_size);
    u64 key = (static_cast<u64>(px) << 32) | codepoint;
    if (auto it = glyphs_.find(key); it != glyphs_.end()) return it->second;

    const stbtt_fontinfo& info = impl_->info;
    int index = stbtt_FindGlyphIndex(&info, static_cast<int>(codepoint));
    if (index == 0 && codepoint != '?' && codepoint != ' ') {
        Glyph fallback = glyph('?', pixel_size);
        return glyphs_[key] = fallback;
    }
    f32 scale = stbtt_ScaleForPixelHeight(&info, static_cast<f32>(px));
    int advance = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&info, index, &advance, &lsb);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&info, index, scale, scale, &x0, &y0, &x1, &y1);

    Glyph g;
    g.advance = advance * scale;
    g.offset = {static_cast<f32>(x0), static_cast<f32>(y0)};
    u32 w = static_cast<u32>(std::max(0, x1 - x0)), h = static_cast<u32>(std::max(0, y1 - y0));
    u32 ax = 0, ay = 0;
    if (w > 0 && h > 0 && place(w, h, g.page, ax, ay)) {
        std::vector<u8> alpha(w * h);
        stbtt_MakeGlyphBitmap(&info, alpha.data(), static_cast<int>(w), static_cast<int>(h), static_cast<int>(w), scale,
                              scale, index);
        std::vector<u8>& pixels = pages_[g.page].pixels;
        for (u32 row = 0; row < h; ++row)
            for (u32 col = 0; col < w; ++col) {
                u8 a = alpha[row * w + col];
                u8* p = &pixels[((ay + row) * page_size + ax + col) * 4];
                p[0] = p[1] = p[2] = p[3] = a; // white, premultiplied
            }
        g.source = {static_cast<f32>(ax), static_cast<f32>(ay), static_cast<f32>(w), static_cast<f32>(h)};
        g.visible = true;
    }
    return glyphs_[key] = g;
}

const Texture* Font::page(u16 index) {
    if (index >= pages_.size()) return nullptr;
    Page& p = pages_[index];
    if (p.dirty && device_) {
        if (!p.texture) {
            TextureDesc desc;
            desc.width = desc.height = page_size;
            desc.rgba8 = p.pixels.data();
            desc.premultiplied = true;
            desc.filter = Filter::Linear;
            desc.mipmaps = false; // glyphs are rasterized at the size they're drawn
            desc.name = name_;
            p.texture = device_->create_texture(desc);
        } else {
            device_->update_texture(*p.texture, p.pixels.data(), true);
        }
        p.dirty = false;
    }
    return p.texture.get();
}

// --- layout ---------------------------------------------------------------------------------------------------------

namespace {

struct Line {
    usize begin, end; // byte range
    f32 width;        // raster pixels
};

f32 measure_range(Font& font, std::string_view text, usize begin, usize end, f32 px) {
    f32 width = 0;
    u32 prev = 0;
    for (usize i = begin; i < end;) {
        u32 cp = utf8::next(text, i);
        if (prev) width += font.kerning(prev, cp, px);
        width += font.glyph(cp, px).advance;
        prev = cp;
    }
    return width;
}

std::vector<Line> break_lines(Font& font, std::string_view text, f32 px, f32 max_width) {
    std::vector<Line> lines;
    usize line_start = 0, last_space = std::string_view::npos;
    f32 width = 0, width_at_space = 0;
    u32 prev = 0;
    for (usize i = 0; i < text.size();) {
        usize at = i;
        u32 cp = utf8::next(text, i);
        if (cp == '\n') {
            lines.push_back({line_start, at, width});
            line_start = i;
            width = 0;
            last_space = std::string_view::npos;
            prev = 0;
            continue;
        }
        f32 adv = font.glyph(cp, px).advance + (prev ? font.kerning(prev, cp, px) : 0.0f);
        if (max_width > 0 && cp != ' ' && width + adv > max_width && at > line_start) {
            if (last_space != std::string_view::npos) {
                lines.push_back({line_start, last_space, width_at_space});
                line_start = last_space + 1;
                width = measure_range(font, text, line_start, at, px);
            } else { // one long word: break inside it
                lines.push_back({line_start, at, width});
                line_start = at;
                width = 0;
            }
            last_space = std::string_view::npos;
            adv = font.glyph(cp, px).advance;
        }
        if (cp == ' ') {
            last_space = at;
            width_at_space = width;
        }
        width += adv;
        prev = cp;
    }
    lines.push_back({line_start, text.size(), width});
    return lines;
}

} // namespace

Vec2 measure_text(Font& font, std::string_view text, const TextStyle& style) {
    f32 scale = std::max(style.raster_scale, 1e-3f);
    f32 px = style.size * scale;
    auto lines = break_lines(font, text, px, style.max_width * scale);
    f32 widest = 0;
    for (const Line& l : lines) widest = std::max(widest, l.width);
    f32 line_h = font.metrics(px).line_height() * style.line_spacing;
    return Vec2{widest, line_h * static_cast<f32>(lines.size())} / scale;
}

Vec2 draw_text(SpriteBatch& batch, Font& font, std::string_view text, Vec2 position, const TextStyle& style, f32 depth) {
    f32 scale = std::max(style.raster_scale, 1e-3f);
    f32 unit = 1.0f / scale;
    f32 px = style.size * scale;
    auto lines = break_lines(font, text, px, style.max_width * scale); // also rasterizes every glyph used
    Font::Metrics m = font.metrics(px);
    f32 line_h = m.line_height() * style.line_spacing;

    f32 widest = 0;
    for (usize li = 0; li < lines.size(); ++li) {
        const Line& line = lines[li];
        widest = std::max(widest, line.width);
        f32 x = 0;
        if (style.align == TextAlign::Center) x = -line.width * 0.5f;
        else if (style.align == TextAlign::Right) x = -line.width;
        f32 baseline = static_cast<f32>(li) * line_h + m.ascent;
        u32 prev = 0;
        for (usize i = line.begin; i < line.end;) {
            u32 cp = utf8::next(text, i);
            if (prev) x += font.kerning(prev, cp, px);
            const Font::Glyph& g = font.glyph(cp, px);
            if (g.visible) {
                if (const Texture* page = font.page(g.page)) {
                    // Snap to whole raster pixels so glyphs stay crisp.
                    Vec2 at{std::round(x + g.offset.x), std::round(baseline + g.offset.y)};
                    batch.draw_region(*page, g.source, position + at * unit, g.source.size() * unit, style.color,
                                      depth, 0, {0, 0});
                }
            }
            x += g.advance;
            prev = cp;
        }
    }
    return Vec2{widest, line_h * static_cast<f32>(lines.size())} * unit;
}

} // namespace ph::gfx
