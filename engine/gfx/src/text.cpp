#include "ph/gfx/text.hpp"

#include "color_glyphs.hpp"
#include "ph/core/log.hpp"
#if PH_HARFBUZZ
#include <hb.h>
#endif
#include "ph/core/utf8.hpp"

#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <optional>
#include <span>

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
    color::Tables colors; // emoji
#if PH_HARFBUZZ
    hb_font_t* hb = nullptr; // shaping, in font units
    ~Impl() {
        if (hb) hb_font_destroy(hb);
    }
#endif
};

bool text_shaping() { return PH_HARFBUZZ != 0; }

void* Font::hb_font(int slot) const {
#if PH_HARFBUZZ
    const Impl* impl = slot <= 0 ? impl_.get() : fallbacks_[static_cast<usize>(slot - 1)]->impl_.get();
    return impl->hb;
#else
    (void)slot;
    return nullptr;
#endif
}

struct Font::Face {
    const stbtt_fontinfo* info = nullptr;
    int index = 0; // 0: the font's "missing" glyph
    const Impl* impl = nullptr;
};

namespace {

// A color glyph's picture at `scale` (emoji): a PNG from sbix or CBDT, or COLR layers drawn in their palette colors.
// Nothing for a plain glyph.
std::optional<color::Bitmap> color_picture(const stbtt_fontinfo* info, std::span<const u8> data, const color::Tables& tables,
                                           int index, f32 scale) {
    if (!tables.any()) return std::nullopt;
    if (auto b = color::bitmap_glyph(data, tables, static_cast<u32>(index), scale * static_cast<f32>(tables.units_per_em)))
        return b;
    auto layers = color::layers(data, tables, static_cast<u32>(index));
    if (layers.empty()) return std::nullopt;
    int ux0 = INT_MAX, uy0 = INT_MAX, ux1 = INT_MIN, uy1 = INT_MIN;
    for (const auto& [glyph, c] : layers) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBox(info, static_cast<int>(glyph), scale, scale, &x0, &y0, &x1, &y1);
        if (x1 <= x0 || y1 <= y0) continue;
        ux0 = std::min(ux0, x0), uy0 = std::min(uy0, y0), ux1 = std::max(ux1, x1), uy1 = std::max(uy1, y1);
    }
    if (ux1 <= ux0 || uy1 <= uy0) return std::nullopt;
    color::Bitmap b;
    b.width = static_cast<u32>(ux1 - ux0);
    b.height = static_cast<u32>(uy1 - uy0);
    b.rgba.assign(static_cast<usize>(b.width) * b.height * 4, 0);
    b.offset = {static_cast<f32>(ux0), static_cast<f32>(uy0)};
    for (const auto& [glyph, c] : layers) { // bottom layer first, each painted over the last
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBox(info, static_cast<int>(glyph), scale, scale, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0;
        if (w <= 0 || h <= 0) continue;
        std::vector<u8> alpha(static_cast<usize>(w) * static_cast<usize>(h));
        stbtt_MakeGlyphBitmap(info, alpha.data(), w, h, w, scale, scale, static_cast<int>(glyph));
        Color paint = c.value_or(Color::white()); // the text color layer: white, tinted when drawn
        for (int row = 0; row < h; ++row)
            for (int col = 0; col < w; ++col) {
                f32 a = alpha[static_cast<usize>(row * w + col)] / 255.0f * paint.a;
                u8* p = &b.rgba[(static_cast<usize>(y0 - uy0 + row) * b.width + static_cast<usize>(x0 - ux0 + col)) * 4];
                const f32 src[4] = {paint.r * a, paint.g * a, paint.b * a, a};
                for (int k = 0; k < 4; ++k) p[k] = static_cast<u8>(std::lround(src[k] * 255.0f + p[k] * (1.0f - a)));
            }
    }
    return b;
}

} // namespace

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
    font->impl_->colors = color::find_tables(font->impl_->data, static_cast<u32>(offset));
#if PH_HARFBUZZ
    // HarfBuzz reads the same bytes (which the Impl keeps alive), the first font of a collection as stb does.
    hb_blob_t* blob = hb_blob_create(reinterpret_cast<const char*>(data), static_cast<unsigned>(font->impl_->data.size()),
                                     HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    hb_face_t* face = hb_face_create(blob, 0);
    font->impl_->hb = hb_font_create(face);
    unsigned upem = hb_face_get_upem(face);
    hb_font_set_scale(font->impl_->hb, static_cast<int>(upem), static_cast<int>(upem));
    hb_face_destroy(face);
    hb_blob_destroy(blob);
#endif
    font->device_ = device;
    font->name_ = std::string(name);
    return font;
}

static u32 size_px(f32 pixel_size) { return std::max(1u, static_cast<u32>(std::lround(pixel_size))); }

void Font::set_fallbacks(std::vector<Asset<Font>> fonts) {
    fallbacks_ = std::move(fonts);
    // Characters drawn as '?' so far may have a real glyph now.
    glyphs_.clear();
    index_glyphs_.clear();
    sdf_glyphs_.clear();
    pages_.clear();
    sdf_pages_.clear();
}

Font::Face Font::face_for(u32 codepoint) const {
    if (int i = stbtt_FindGlyphIndex(&impl_->info, static_cast<int>(codepoint)); i != 0) return {&impl_->info, i, impl_.get()};
    for (const Asset<Font>& f : fallbacks_)
        if (f && f.get() != this)
            if (int i = stbtt_FindGlyphIndex(&f->impl_->info, static_cast<int>(codepoint)); i != 0)
                return {&f->impl_->info, i, f->impl_.get()};
    return {&impl_->info, 0, impl_.get()};
}

bool Font::has_glyph(u32 codepoint) const { return face_for(codepoint).index != 0; }

Font::Metrics Font::metrics(f32 pixel_size) const {
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&impl_->info, &ascent, &descent, &gap);
    f32 scale = stbtt_ScaleForPixelHeight(&impl_->info, static_cast<f32>(size_px(pixel_size)));
    return {ascent * scale, descent * scale, gap * scale};
}

f32 Font::advance(u32 codepoint, f32 pixel_size) const {
    Face face = face_for(codepoint);
    if (face.index == 0 && codepoint != ' ') face = face_for('?');
    int advance = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(face.info, face.index, &advance, &lsb);
    return advance * stbtt_ScaleForPixelHeight(face.info, static_cast<f32>(size_px(pixel_size)));
}

f32 Font::kerning(u32 left, u32 right, f32 pixel_size) const {
    Face a = face_for(left), b = face_for(right);
    if (a.info != b.info) return 0; // kerning only means something within one font
    f32 scale = stbtt_ScaleForPixelHeight(a.info, static_cast<f32>(size_px(pixel_size)));
    return stbtt_GetGlyphKernAdvance(a.info, a.index, b.index) * scale;
}

bool Font::place(std::vector<Page>& pages, u32 w, u32 h, u16& page, u32& x, u32& y) {
    if (w + 2 > page_size || h + 2 > page_size) return false;
    if (pages.empty()) pages.push_back({std::vector<u8>(page_size * page_size * 4, 0)});
    Page* p = &pages.back();
    if (p->shelf_x + w + 1 > page_size) { // next shelf
        p->shelf_y += p->shelf_h + 1;
        p->shelf_x = 1;
        p->shelf_h = 0;
    }
    if (p->shelf_y + h + 1 > page_size) { // next page
        pages.push_back({std::vector<u8>(page_size * page_size * 4, 0)});
        p = &pages.back();
    }
    page = static_cast<u16>(pages.size() - 1);
    x = p->shelf_x;
    y = p->shelf_y;
    p->shelf_x += w + 1;
    p->shelf_h = std::max(p->shelf_h, h);
    p->mark(x, y, w, h);
    return true;
}

const Font::Glyph& Font::glyph(u32 codepoint, f32 pixel_size) {
    u32 px = size_px(pixel_size);
    u64 key = (static_cast<u64>(px) << 32) | codepoint;
    if (auto it = glyphs_.find(key); it != glyphs_.end()) return it->second;

    Face face = face_for(codepoint);
    if (face.index == 0 && codepoint != '?' && codepoint != ' ') {
        Glyph missing = glyph('?', pixel_size);
        return glyphs_[key] = missing;
    }
    Glyph g = rasterize(face, px);
    return glyphs_[key] = g;
}

const Font::Glyph& Font::glyph_at(int slot, u32 index, f32 pixel_size) {
    u32 px = size_px(pixel_size);
    u64 key = (static_cast<u64>(px) << 40) | (static_cast<u64>(slot & 0xFF) << 32) | index;
    if (auto it = index_glyphs_.find(key); it != index_glyphs_.end()) return it->second;
    const Impl* impl = slot == 0 ? impl_.get() : fallbacks_[static_cast<usize>(slot - 1)]->impl_.get();
    Glyph g = rasterize({&impl->info, static_cast<int>(index), impl}, px);
    return index_glyphs_[key] = g;
}

int Font::slot_for(u32 codepoint) const {
    if (stbtt_FindGlyphIndex(&impl_->info, static_cast<int>(codepoint)) != 0) return 0;
    for (usize i = 0; i < fallbacks_.size(); ++i)
        if (fallbacks_[i] && fallbacks_[i].get() != this && stbtt_FindGlyphIndex(&fallbacks_[i]->impl_->info, static_cast<int>(codepoint)) != 0)
            return static_cast<int>(i + 1);
    return -1;
}

f32 Font::slot_scale(int slot, f32 pixel_size) const {
    const Impl* impl = slot <= 0 ? impl_.get() : fallbacks_[static_cast<usize>(slot - 1)]->impl_.get();
    return stbtt_ScaleForPixelHeight(&impl->info, static_cast<f32>(size_px(pixel_size)));
}

Font::Glyph Font::rasterize(const Face& face, u32 px) {
    f32 scale = stbtt_ScaleForPixelHeight(face.info, static_cast<f32>(px));
    int advance = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(face.info, face.index, &advance, &lsb);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(face.info, face.index, scale, scale, &x0, &y0, &x1, &y1);

    Glyph g;
    g.advance = advance * scale;
    g.offset = {static_cast<f32>(x0), static_cast<f32>(y0)};
    u32 ax = 0, ay = 0;
    if (auto picture = color_picture(face.info, face.impl->data, face.impl->colors, face.index, scale)) {
        if (place(pages_, picture->width, picture->height, g.page, ax, ay)) {
            std::vector<u8>& pixels = pages_[g.page].pixels;
            for (u32 row = 0; row < picture->height; ++row)
                std::copy_n(&picture->rgba[static_cast<usize>(row) * picture->width * 4], picture->width * 4,
                            &pixels[((ay + row) * page_size + ax) * 4]);
            g.source = {static_cast<f32>(ax), static_cast<f32>(ay), static_cast<f32>(picture->width),
                        static_cast<f32>(picture->height)};
            g.offset = picture->offset;
            g.visible = true;
            g.color = true;
        }
        return g;
    }
    u32 w = static_cast<u32>(std::max(0, x1 - x0)), h = static_cast<u32>(std::max(0, y1 - y0));
    if (w > 0 && h > 0 && place(pages_, w, h, g.page, ax, ay)) {
        std::vector<u8> alpha(w * h);
        stbtt_MakeGlyphBitmap(face.info, alpha.data(), static_cast<int>(w), static_cast<int>(h), static_cast<int>(w),
                              scale, scale, face.index);
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
    return g;
}

const Font::Glyph& Font::sdf_glyph(u32 codepoint) {
    if (auto it = sdf_glyphs_.find(codepoint); it != sdf_glyphs_.end()) return it->second;
    Face face = face_for(codepoint);
    if (face.index == 0 && codepoint != '?' && codepoint != ' ') {
        Glyph missing = sdf_glyph('?');
        return sdf_glyphs_[codepoint] = missing;
    }
    f32 scale = stbtt_ScaleForPixelHeight(face.info, sdf_size);
    int advance = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(face.info, face.index, &advance, &lsb);
    Glyph g;
    g.advance = advance * scale;
    int w = 0, h = 0, xoff = 0, yoff = 0;
    // Distances: 128 on the outline, falling to 0 sdf_spread pixels outside and rising to 255 inside.
    unsigned char* field = stbtt_GetGlyphSDF(face.info, scale, face.index, sdf_spread, 128,
                                             128.0f / static_cast<f32>(sdf_spread), &w, &h, &xoff, &yoff);
    u32 ax = 0, ay = 0;
    if (field && w > 0 && h > 0 && place(sdf_pages_, static_cast<u32>(w), static_cast<u32>(h), g.page, ax, ay)) {
        std::vector<u8>& pixels = sdf_pages_[g.page].pixels;
        for (int row = 0; row < h; ++row)
            for (int col = 0; col < w; ++col) {
                u8* p = &pixels[((ay + static_cast<u32>(row)) * page_size + ax + static_cast<u32>(col)) * 4];
                p[0] = p[1] = p[2] = 255;
                p[3] = field[row * w + col];
            }
        g.source = {static_cast<f32>(ax), static_cast<f32>(ay), static_cast<f32>(w), static_cast<f32>(h)};
        g.offset = {static_cast<f32>(xoff), static_cast<f32>(yoff)};
        g.visible = true;
    }
    if (field) stbtt_FreeSDF(field, nullptr);
    return sdf_glyphs_[codepoint] = g;
}

const Texture* Font::upload(std::vector<Page>& pages, u16 index, std::string_view label) {
    if (index >= pages.size()) return nullptr;
    Page& p = pages[index];
    if (device_ && (!p.texture || p.dirty())) {
        if (!p.texture) {
            TextureDesc desc;
            desc.width = desc.height = page_size;
            desc.rgba8 = p.pixels.data();
            desc.premultiplied = true; // glyph pages are written ready to use (SDF pages hold distances, not colors)
            desc.filter = Filter::Linear;
            desc.mipmaps = false; // glyphs are rasterized at the size they're drawn
            desc.name = label;
            p.texture = device_->create_texture(desc);
        } else {
            // Only the glyphs added since the last upload: usually a few hundred bytes, not the 4 MB page.
            PixelRect rect{p.dirty_x0, p.dirty_y0, p.dirty_x1 - p.dirty_x0, p.dirty_y1 - p.dirty_y0};
            device_->update_texture_region(*p.texture, rect, &p.pixels[(rect.y * page_size + rect.x) * 4], page_size,
                                           true);
        }
        p.dirty_x0 = p.dirty_y0 = page_size;
        p.dirty_x1 = p.dirty_y1 = 0;
    }
    return p.texture.get();
}

const Texture* Font::page(u16 index) { return upload(pages_, index, name_); }
const Texture* Font::sdf_page(u16 index) { return upload(sdf_pages_, index, name_ + " (sdf)"); }

// Markup

namespace {

enum class Effect : u8 { None, Wave, Shake };

// Style changes in the text, by byte offset into the text with the tags taken out.
struct Run {
    usize begin;
    Color color;
    bool has_color;
    Effect effect;
};

std::optional<Color> parse_color(std::string_view v) {
    if (v.starts_with('#')) v.remove_prefix(1);
    u32 rgb = 0;
    if ((v.size() != 6 && v.size() != 8 && v.size() != 3) ||
        std::from_chars(v.data(), v.data() + v.size(), rgb, 16).ec != std::errc{})
        return std::nullopt;
    if (v.size() == 3) // #rgb
        return Color::rgba8(static_cast<u8>(((rgb >> 8) & 0xF) * 17), static_cast<u8>(((rgb >> 4) & 0xF) * 17),
                            static_cast<u8>((rgb & 0xF) * 17));
    if (v.size() == 8) return Color::hex(rgb >> 8, static_cast<f32>(rgb & 0xFF) / 255.0f);
    return Color::hex(rgb);
}

// Takes the tags out of `text` into `plain`, recording where the color and effect change.
void parse_markup(std::string_view text, std::string& plain, std::vector<Run>& runs) {
    struct State {
        Color color;
        bool has_color = false;
        Effect effect = Effect::None;
    };
    std::vector<State> stack{State{}};
    auto change = [&] {
        const State& s = stack.back();
        if (!runs.empty() && runs.back().begin == plain.size()) runs.pop_back();
        runs.push_back({plain.size(), s.color, s.has_color, s.effect});
    };
    change();
    for (usize i = 0; i < text.size();) {
        if (text[i] == '[') {
            if (i + 1 < text.size() && text[i + 1] == '[') { // "[[" is a literal "["
                plain += '[';
                i += 2;
                continue;
            }
            usize close = text.find(']', i);
            if (close != std::string_view::npos) {
                std::string_view tag = text.substr(i + 1, close - i - 1);
                bool known = true;
                if (tag.starts_with("color=")) {
                    if (auto c = parse_color(tag.substr(6))) {
                        State s = stack.back();
                        s.color = *c;
                        s.has_color = true;
                        stack.push_back(s);
                    } else known = false;
                } else if (tag == "wave" || tag == "shake") {
                    State s = stack.back();
                    s.effect = tag == "wave" ? Effect::Wave : Effect::Shake;
                    stack.push_back(s);
                } else if (tag == "/color" || tag == "/wave" || tag == "/shake") {
                    if (stack.size() > 1) stack.pop_back();
                } else {
                    known = false; // not a tag: keep the text as it is
                }
                if (known) {
                    change();
                    i = close + 1;
                    continue;
                }
            }
        }
        plain += text[i++];
    }
}

const Run& run_at(const std::vector<Run>& runs, usize byte) {
    const Run* found = &runs.front();
    for (const Run& r : runs)
        if (r.begin <= byte) found = &r;
        else break;
    return *found;
}

// Layout

struct Line {
    usize begin, end; // byte range
    f32 width;        // raster pixels
};

} // namespace

// One glyph of a laid-out line, in drawing order (left to right on screen). Not rasterized yet.
struct LineGlyph {
    int slot;       // shaped: the font slot `id` is a glyph index in; -1: `id` is a character, drawn with Font::glyph
    u32 id;
    usize cluster;  // byte offset of the character it draws
    f32 x, y;       // from the line's start on the baseline, pixels (y down)
    bool rtl = false; // shaped right to left (so a bracket was drawn mirrored)
};
struct ShapedLine {
    std::vector<LineGlyph> glyphs;
    f32 width = 0;
};

// Lays out lines of one text: its characters' bidi levels, and shaping (HarfBuzz builds) or one glyph per character.
struct TextShaper {
    Font& font;
    std::string_view text;
    f32 px;
    bool shape_runs; // HarfBuzz, for bitmap text
    std::vector<u32> cps;
    std::vector<usize> offsets; // each character's byte offset
    std::vector<u8> levels, paragraph_levels;
    bool any_rtl = false;
#if PH_HARFBUZZ
    hb_buffer_t* buffer = hb_buffer_create();
    ~TextShaper() { hb_buffer_destroy(buffer); }
#endif

    TextShaper(Font& f, std::string_view t, f32 pixels, bool sdf, bidi::Direction direction)
        : font(f), text(t), px(pixels), shape_runs(PH_HARFBUZZ && !sdf) {
        for (usize i = 0; i < text.size();) {
            offsets.push_back(i);
            cps.push_back(utf8::next(text, i));
        }
        levels.resize(cps.size());
        paragraph_levels.resize(cps.size());
        for (usize begin = 0; begin < cps.size();) { // paragraph by paragraph
            usize end = begin;
            while (end < cps.size() && cps[end] != '\n') ++end;
            bool rtl = direction == bidi::Direction::RightToLeft;
            for (usize i = begin; i < end && !rtl; ++i) rtl = bidi::type_of(cps[i]) != bidi::Type::L || cps[i] >= 0x0590;
            if (rtl) { // only text with right-to-left or neutral characters needs the full algorithm
                bidi::Paragraph p = bidi::resolve(std::span(cps).subspan(begin, end - begin), direction);
                std::copy(p.levels.begin(), p.levels.end(), levels.begin() + static_cast<std::ptrdiff_t>(begin));
                std::fill_n(paragraph_levels.begin() + static_cast<std::ptrdiff_t>(begin), end - begin, p.level);
                for (u8 l : p.levels) any_rtl = any_rtl || l > 0;
            }
            begin = end + 1;
        }
    }

    std::pair<usize, usize> chars(usize begin, usize end) const {
        auto a = std::lower_bound(offsets.begin(), offsets.end(), begin) - offsets.begin();
        auto b = std::lower_bound(offsets.begin(), offsets.end(), end) - offsets.begin();
        return {static_cast<usize>(a), static_cast<usize>(b)};
    }

    // Levels for a line, with trailing whitespace at the paragraph's level (UAX #9, L1).
    std::vector<u8> line_levels(usize a, usize b) const {
        std::vector<u8> out(levels.begin() + static_cast<std::ptrdiff_t>(a), levels.begin() + static_cast<std::ptrdiff_t>(b));
        for (usize i = b; i > a; --i) {
            bidi::Type t = bidi::type_of(cps[i - 1]);
            if (t != bidi::Type::WS && t != bidi::Type::S) break;
            out[i - 1 - a] = paragraph_levels[a];
        }
        return out;
    }

    // Characters one by one, in visual order, with the font's kerning between neighbours.
    ShapedLine unshaped(usize a, usize b) const {
        ShapedLine line;
        std::vector<u32> order;
        if (any_rtl) order = bidi::visual_order(std::span(cps).subspan(a, b - a), line_levels(a, b), paragraph_levels[a]);
        f32 x = 0;
        u32 prev = 0;
        for (usize k = 0; k < b - a; ++k) {
            usize i = a + (order.empty() ? k : order[k]);
            u32 cp = levels[i] % 2 ? bidi::mirror(cps[i]) : cps[i];
            if (prev) x += font.kerning(prev, cp, px);
            line.glyphs.push_back({-1, cp, offsets[i], x, 0});
            x += font.advance(cp, px);
            prev = cp;
        }
        line.width = x;
        return line;
    }

#if PH_HARFBUZZ
    // Runs of one level, font and script, each shaped by HarfBuzz, in visual order.
    ShapedLine shaped(usize a, usize b) {
        struct Item {
            usize begin, end; // characters
            int slot;
            hb_script_t script;
            u8 level;
        };
        std::vector<u8> lv = line_levels(a, b);
        hb_unicode_funcs_t* unicode = hb_unicode_funcs_get_default();
        std::vector<Item> items;
        for (usize i = a; i < b; ++i) {
            u32 cp = cps[i];
            int slot = font.slot_for(cp);
            hb_script_t script = hb_unicode_script(unicode, cp);
            bool joins_previous = bidi::type_of(cp) == bidi::Type::NSM || cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F) ||
                                  (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0020 && cp <= 0xE007F) || cp == 0x20E3;
            bool common = script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED || script == HB_SCRIPT_UNKNOWN;
            if (!items.empty()) {
                Item& last = items.back();
                if (joins_previous) slot = last.slot;
                if (common) script = last.script;
                else if (last.script == HB_SCRIPT_COMMON) last.script = script; // a run of punctuation takes what follows
                if (slot == last.slot && script == last.script && lv[i - a] == last.level) {
                    last.end = i + 1;
                    continue;
                }
            }
            items.push_back({i, i + 1, slot, common ? HB_SCRIPT_COMMON : script, lv[i - a]});
        }
        std::vector<u32> dummy(items.size(), 'x');
        std::vector<u8> item_levels;
        for (const Item& it : items) item_levels.push_back(it.level);
        std::vector<u32> order = bidi::visual_order(dummy, item_levels, paragraph_levels.empty() ? 0 : paragraph_levels[a]);

        ShapedLine line;
        f32 x = 0;
        for (u32 k : order) {
            const Item& it = items[k];
            if (it.slot < 0) { // no font has these: one '?' each, right to left in right-to-left runs
                bool rtl = it.level % 2 != 0;
                for (usize k = 0; k < it.end - it.begin; ++k) {
                    usize i = rtl ? it.end - 1 - k : it.begin + k;
                    u32 cp = rtl ? bidi::mirror(cps[i]) : cps[i];
                    line.glyphs.push_back({-1, cp, offsets[i], x, 0});
                    x += font.advance(cp, px);
                }
                continue;
            }
            hb_buffer_clear_contents(buffer);
            usize from = offsets[it.begin], to = it.end < offsets.size() ? offsets[it.end] : text.size();
            hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), static_cast<unsigned>(from),
                               static_cast<int>(to - from));
            hb_buffer_set_direction(buffer, it.level % 2 ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
            hb_buffer_set_script(buffer, it.script == HB_SCRIPT_COMMON ? HB_SCRIPT_LATIN : it.script);
            hb_buffer_set_language(buffer, hb_language_get_default());
            hb_shape(static_cast<hb_font_t*>(font.hb_font(it.slot)), buffer, nullptr, 0);
            unsigned n = 0;
            hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, &n);
            hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buffer, &n);
            f32 scale = font.slot_scale(it.slot, px);
            for (unsigned g = 0; g < n; ++g) {
                line.glyphs.push_back({it.slot, info[g].codepoint, info[g].cluster, x + pos[g].x_offset * scale,
                                       -pos[g].y_offset * scale, it.level % 2 != 0});
                x += pos[g].x_advance * scale;
            }
        }
        line.width = x;
        return line;
    }
#endif

    ShapedLine shape(usize begin, usize end) {
        auto [a, b] = chars(begin, end);
#if PH_HARFBUZZ
        if (shape_runs && b > a) return shaped(a, b);
#endif
        return unshaped(a, b);
    }
    f32 measure(usize begin, usize end) { return shape(begin, end).width; }

    // The rasterized glyph for a line glyph.
    const Font::Glyph& glyph(const LineGlyph& lg, bool sdf) {
        if (lg.slot >= 0) return font.glyph_at(lg.slot, lg.id, px);
        return sdf ? font.sdf_glyph(lg.id) : font.glyph(lg.id, px);
    }
};

namespace {

// Paragraphs at '\n', each broken into lines no wider than max_width (if > 0) at spaces, or inside a word too long
// for a line.
std::vector<Line> break_lines(TextShaper& s, f32 max_width) {
    std::vector<Line> lines;
    std::string_view text = s.text;
    for (usize p = 0; p <= text.size();) {
        usize p_end = text.find('\n', p);
        if (p_end == std::string_view::npos) p_end = text.size();
        usize start = p;
        while (true) {
            if (max_width <= 0) {
                lines.push_back({start, p_end, s.measure(start, p_end)});
                break;
            }
            // The longest run of whole words that fits.
            usize fit_end = std::string_view::npos;
            f32 fit_width = 0;
            for (usize e = start;;) {
                usize space = text.find(' ', e);
                usize word_end = space == std::string_view::npos || space > p_end ? p_end : space;
                f32 w = s.measure(start, word_end);
                if (w > max_width) break; // (if it's the first word, it's too long on its own)
                fit_end = word_end;
                fit_width = w;
                if (word_end >= p_end) break;
                e = word_end + 1;
            }
            if (fit_end == std::string_view::npos) { // break inside the word: as many characters as fit, at least one
                usize at = start, last_fit = std::string_view::npos;
                f32 w = 0;
                while (at < p_end) {
                    usize next = at;
                    utf8::next(text, next);
                    f32 nw = s.measure(start, next);
                    if (nw > max_width && last_fit != std::string_view::npos) break;
                    last_fit = next;
                    w = nw;
                    at = next;
                }
                lines.push_back({start, last_fit, w});
                start = last_fit;
                if (start >= p_end) break;
                continue;
            }
            lines.push_back({start, fit_end, fit_width});
            if (fit_end >= p_end) break;
            start = fit_end + 1; // the space the line broke at
            if (start >= p_end) break;
        }
        p = p_end + 1;
    }
    return lines;
}

// A small, repeatable jitter for [shake]: the same for a character within a twentieth of a second.
f32 jitter(u32 index, f32 time, u32 axis) {
    u32 h = index * 374761393u + static_cast<u32>(time * 20.0f) * 668265263u + axis * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<f32>(h & 0xFFFF) / 32767.5f - 1.0f;
}

struct Laid {
    std::vector<PlacedGlyph> glyphs;
    Vec2 size;
};

Laid lay_out(Font& font, std::string_view source, Vec2 position, const TextStyle& style) {
    std::string plain;
    std::vector<Run> runs;
    std::string_view text = source;
    if (style.markup) {
        parse_markup(source, plain, runs);
        text = plain;
    }
    // SDF text lays out at the distance field's size and is scaled; bitmap text at screen pixels.
    f32 scale = style.sdf ? Font::sdf_size / std::max(style.size, 1e-3f) : std::max(style.raster_scale, 1e-3f);
    f32 unit = 1.0f / scale;
    f32 px = style.sdf ? Font::sdf_size : style.size * scale;
    TextShaper shaper(font, text, px, style.sdf, style.direction);
    auto lines = break_lines(shaper, style.max_width * scale);
    Font::Metrics m = font.metrics(px);
    f32 line_h = m.line_height() * style.line_spacing;

    Laid out;
    f32 widest = 0;
    struct Order {
        usize line, cluster, at; // reading order: by line, then character; `at` is its place in out.glyphs
    };
    std::vector<Order> reading;
    std::vector<Effect> effects;
    for (usize li = 0; li < lines.size(); ++li) {
        ShapedLine shaped = shaper.shape(lines[li].begin, lines[li].end);
        widest = std::max(widest, shaped.width);
        f32 x = 0;
        if (style.align == TextAlign::Center) x = -shaped.width * 0.5f;
        else if (style.align == TextAlign::Right) x = -shaped.width;
        f32 baseline = static_cast<f32>(li) * line_h + m.ascent;
        for (const LineGlyph& lg : shaped.glyphs) {
            const Font::Glyph& g = shaper.glyph(lg, style.sdf);
            if (!g.visible) continue;
            // Bitmap glyphs snap to whole raster pixels so they stay crisp; SDF glyphs don't need to.
            Vec2 at_px{x + lg.x + g.offset.x, baseline + lg.y + g.offset.y};
            if (!style.sdf) at_px = {std::round(at_px.x), std::round(at_px.y)};
            PlacedGlyph p;
            usize c = lg.cluster;
            if (lg.slot < 0) p.codepoint = lg.id; // mirrored already if it was
            else if (c < text.size()) p.codepoint = lg.rtl ? bidi::mirror(utf8::next(text, c)) : utf8::next(text, c);
            p.glyph_index = lg.slot >= 0 ? lg.id : 0;
            p.page = g.page;
            p.sdf = style.sdf;
            p.source = g.source;
            p.position = position + at_px * unit;
            p.size = g.source.size() * unit;
            p.color = style.color;
            Effect effect = Effect::None;
            if (style.markup) {
                const Run& r = run_at(runs, lg.cluster);
                if (r.has_color) p.color = r.color.with_alpha(r.color.a * style.color.a);
                effect = r.effect;
            }
            if (g.color) { // emoji keep their own colors; the text color only fades them
                p.color = Color::white().with_alpha(p.color.a);
                p.color_glyph = true;
            }
            reading.push_back({li, lg.cluster, out.glyphs.size()});
            out.glyphs.push_back(p);
            effects.push_back(effect);
        }
    }
    // Indexes count in reading order (right-to-left text is drawn the other way), for typewriter reveals and effects.
    std::stable_sort(reading.begin(), reading.end(), [](const Order& a, const Order& b) {
        return a.line != b.line ? a.line < b.line : a.cluster < b.cluster;
    });
    for (usize k = 0; k < reading.size(); ++k) {
        PlacedGlyph& p = out.glyphs[reading[k].at];
        p.index = static_cast<u32>(k);
        if (effects[reading[k].at] == Effect::Wave) p.position.y += std::sin(style.time * 6.0f + static_cast<f32>(p.index) * 0.6f) * style.size * 0.12f;
        if (effects[reading[k].at] == Effect::Shake)
            p.position += Vec2{jitter(p.index, style.time, 0), jitter(p.index, style.time, 1)} * (style.size * 0.05f);
    }
    if (style.max_glyphs >= 0)
        std::erase_if(out.glyphs, [&](const PlacedGlyph& p) { return p.index >= static_cast<u32>(style.max_glyphs); });
    out.size = Vec2{widest, line_h * static_cast<f32>(lines.size())} * unit;
    return out;
}

} // namespace

std::string strip_markup(std::string_view text) {
    std::string plain;
    std::vector<Run> runs;
    parse_markup(text, plain, runs);
    return plain;
}

std::vector<PlacedGlyph> layout_text(Font& font, std::string_view text, Vec2 position, const TextStyle& style) {
    return lay_out(font, text, position, style).glyphs;
}

Vec2 measure_text(Font& font, std::string_view text, const TextStyle& style) {
    std::string plain;
    std::vector<Run> runs;
    if (style.markup) {
        parse_markup(text, plain, runs);
        text = plain;
    }
    f32 scale = style.sdf ? Font::sdf_size / std::max(style.size, 1e-3f) : std::max(style.raster_scale, 1e-3f);
    f32 px = style.sdf ? Font::sdf_size : style.size * scale;
    TextShaper shaper(font, text, px, style.sdf, style.direction);
    auto lines = break_lines(shaper, style.max_width * scale);
    f32 widest = 0;
    for (const Line& l : lines) widest = std::max(widest, l.width);
    f32 line_h = font.metrics(px).line_height() * style.line_spacing;
    return Vec2{widest, line_h * static_cast<f32>(lines.size())} / scale;
}

Vec2 draw_text(SpriteBatch& batch, Font& font, std::string_view text, Vec2 position, const TextStyle& style, f32 depth) {
    Laid laid = lay_out(font, text, position, style);
    auto draw_pass = [&](Vec2 offset, std::optional<Color> tint) {
        for (const PlacedGlyph& g : laid.glyphs) {
            const Texture* page = g.sdf ? font.sdf_page(g.page) : font.page(g.page);
            if (!page) continue;
            Color c = tint ? tint->with_alpha(tint->a * g.color.a) : g.color;
            batch.draw_region(*page, g.source, g.position + offset, g.size, c, depth, 0, {0, 0});
        }
    };
    // Underneath first: the shadow, then the outline (copies around each letter), then the letters.
    if (style.shadow.x != 0 || style.shadow.y != 0) draw_pass(style.shadow, style.shadow_color);
    if (style.outline > 0 && !style.sdf) {
        f32 r = style.outline;
        int steps = r * std::max(style.raster_scale, 1.0f) > 1.5f ? 16 : 8;
        for (int k = 0; k < steps; ++k) draw_pass(Vec2::from_angle(tau * static_cast<f32>(k) / static_cast<f32>(steps)) * r, style.outline_color);
    }
    draw_pass({}, std::nullopt);
    return laid.size;
}

Material sdf_text_material(AssetManager& assets, const SdfTextLook& look) {
    struct Params {
        Color outline_color; // premultiplied
        f32 outline, softness, pad[2];
    };
    Material m;
    m.fragment = assets.load<Shader>("engine/shaders/text_sdf.frag");
    m.set_uniforms(Params{look.outline_color.premultiplied(), std::clamp(look.outline, 0.0f, 0.49f),
                          std::max(look.softness, 0.0f), {0, 0}});
    return m;
}

} // namespace ph::gfx
