#include "color_glyphs.hpp"

#include "bowl/gfx/renderer.hpp" // decode_png

#include <algorithm>
#include <cmath>

namespace bowl::gfx::color {

namespace {

// Big-endian reads that give 0 past the end, with `ok` cleared, so broken fonts fail softly.
struct Reader {
    std::span<const u8> data;
    mutable bool ok = true;
    bool has(u64 at, u64 n) const {
        if (at + n > data.size() || at + n < at) ok = false;
        return ok;
    }
    u8 u8_(u64 at) const { return has(at, 1) ? data[at] : 0; }
    u16 u16_(u64 at) const { return has(at, 2) ? static_cast<u16>(data[at] << 8 | data[at + 1]) : 0; }
    i16 i16_(u64 at) const { return static_cast<i16>(u16_(at)); }
    u32 u32_(u64 at) const {
        return has(at, 4) ? (u32{data[at]} << 24 | u32{data[at + 1]} << 16 | u32{data[at + 2]} << 8 | data[at + 3]) : 0;
    }
};

std::optional<Bitmap> decode(std::span<const u8> png) {
    Bitmap b;
    if (!decode_png(png, b.width, b.height, b.rgba)) return std::nullopt;
    for (usize i = 0; i < b.rgba.size(); i += 4) { // premultiply
        u32 a = b.rgba[i + 3];
        for (int c = 0; c < 3; ++c) b.rgba[i + c] = static_cast<u8>((b.rgba[i + c] * a + 127) / 255);
    }
    return b;
}

// Scales a decoded strike picture from `strike_ppem` to `ppem`. `left` and `top` are where it sits at the strike's
// size, in pixels from the origin (top measured up).
std::optional<Bitmap> place(std::optional<Bitmap> b, f32 strike_ppem, f32 ppem, f32 left, f32 top) {
    if (!b || b->width == 0 || b->height == 0 || strike_ppem <= 0) return std::nullopt;
    f32 f = ppem / strike_ppem;
    u32 w = std::max(1u, static_cast<u32>(std::lround(static_cast<f32>(b->width) * f)));
    u32 h = std::max(1u, static_cast<u32>(std::lround(static_cast<f32>(b->height) * f)));
    if (w != b->width || h != b->height) b->rgba = resize(b->rgba, b->width, b->height, w, h);
    b->width = w;
    b->height = h;
    b->offset = {std::round(left * f), std::round(-top * f)};
    return b;
}

std::optional<Bitmap> sbix_glyph(const Reader& r, const Tables& t, u32 glyph, f32 ppem) {
    u32 strikes = r.u32_(t.sbix + 4);
    // The smallest strike at least as big as needed (sharpest after shrinking), else the biggest.
    u32 best = 0, best_ppem = 0;
    for (u32 i = 0; i < strikes && i < 256; ++i) {
        u32 strike = t.sbix + r.u32_(t.sbix + 8 + 4 * u64{i});
        u32 p = r.u16_(strike);
        bool better = best == 0 || (best_ppem < ppem ? p > best_ppem : (p >= ppem && p < best_ppem));
        if (r.ok && p > 0 && better) best = strike, best_ppem = p;
    }
    if (!best || !r.ok) return std::nullopt;
    for (int follow = 0; follow < 2; ++follow) { // a "dupe" points at another glyph's picture, once
        if (glyph >= t.glyph_count) return std::nullopt;
        u32 begin = r.u32_(best + 4 + 4 * u64{glyph}), end = r.u32_(best + 4 + 4 * (u64{glyph} + 1));
        if (!r.ok || end <= begin + 8) return std::nullopt;
        u64 at = u64{best} + begin;
        i16 ox = r.i16_(at), oy = r.i16_(at + 2);
        u32 type = r.u32_(at + 4);
        u64 size = end - begin - 8;
        if (!r.has(at + 8, size)) return std::nullopt;
        if (type == 0x64757065) { // "dupe"
            glyph = r.u16_(at + 8);
            continue;
        }
        if (type != 0x706E6720) return std::nullopt; // only "png "
        auto picture = decode(r.data.subspan(at + 8, size));
        if (!picture) return std::nullopt;
        f32 height = static_cast<f32>(picture->height);
        // The origin offsets put the picture's bottom-left corner relative to the glyph origin.
        return place(std::move(picture), static_cast<f32>(best_ppem), ppem, ox, oy + height);
    }
    return std::nullopt;
}

std::optional<Bitmap> cbdt_glyph(const Reader& r, const Tables& t, u32 glyph, f32 ppem) {
    u32 sizes = r.u32_(t.cblc + 4);
    u64 best = 0;
    u32 best_ppem = 0;
    for (u32 i = 0; i < sizes && i < 256; ++i) {
        u64 rec = t.cblc + 8 + 48 * u64{i};
        u32 first = r.u16_(rec + 40), last = r.u16_(rec + 42), p = r.u8_(rec + 45);
        if (!r.ok || glyph < first || glyph > last || p == 0) continue;
        bool better = best == 0 || (best_ppem < ppem ? p > best_ppem : (p >= ppem && p < best_ppem));
        if (better) best = rec, best_ppem = p;
    }
    if (!best) return std::nullopt;
    u64 array = t.cblc + u64{r.u32_(best)};
    u32 subtables = r.u32_(best + 8);
    for (u32 i = 0; i < subtables && i < 4096 && r.ok; ++i) {
        u64 entry = array + 8 * u64{i};
        u32 first = r.u16_(entry), last = r.u16_(entry + 2);
        if (glyph < first || glyph > last) continue;
        u64 sub = array + u64{r.u32_(entry + 4)};
        u16 index_format = r.u16_(sub), image_format = r.u16_(sub + 2);
        u64 image_data = t.cbdt + u64{r.u32_(sub + 4)};
        u64 at = 0;
        u64 big_metrics = 0; // formats 2 and 5 keep the metrics here
        u32 n = glyph - first;
        switch (index_format) {
        case 1: at = image_data + r.u32_(sub + 8 + 4 * u64{n}); break;
        case 2: at = image_data + u64{r.u32_(sub + 8)} * n, big_metrics = sub + 12; break;
        case 3: at = image_data + r.u16_(sub + 8 + 2 * u64{n}); break;
        case 4: {
            u32 count = r.u32_(sub + 8);
            for (u32 k = 0; k < count && k < 65536 && r.ok; ++k)
                if (r.u16_(sub + 12 + 4 * u64{k}) == glyph) {
                    at = image_data + r.u16_(sub + 12 + 4 * u64{k} + 2);
                    break;
                }
            if (!at) return std::nullopt;
            break;
        }
        case 5: {
            u32 count = r.u32_(sub + 20);
            u32 k = 0;
            while (k < count && k < 65536 && r.ok && r.u16_(sub + 24 + 2 * u64{k}) != glyph) ++k;
            if (k >= count) return std::nullopt;
            at = image_data + u64{r.u32_(sub + 8)} * k, big_metrics = sub + 12;
            break;
        }
        default: return std::nullopt;
        }
        // Metrics, then u32 length, then the PNG.
        i32 bearing_x = 0, bearing_y = 0;
        u64 png = 0;
        if (image_format == 17) { // small metrics: height, width, bearingX, bearingY, advance
            bearing_x = static_cast<i8>(r.u8_(at + 2)), bearing_y = static_cast<i8>(r.u8_(at + 3));
            png = at + 5;
        } else if (image_format == 18) { // big metrics: height, width, horiBearingX, horiBearingY, ...
            bearing_x = static_cast<i8>(r.u8_(at + 2)), bearing_y = static_cast<i8>(r.u8_(at + 3));
            png = at + 8;
        } else if (image_format == 19 && big_metrics) {
            bearing_x = static_cast<i8>(r.u8_(big_metrics + 2)), bearing_y = static_cast<i8>(r.u8_(big_metrics + 3));
            png = at;
        } else {
            return std::nullopt;
        }
        u32 length = r.u32_(png);
        if (!r.has(png + 4, length) || length == 0) return std::nullopt;
        return place(decode(r.data.subspan(png + 4, length)), static_cast<f32>(best_ppem), ppem,
                     static_cast<f32>(bearing_x), static_cast<f32>(bearing_y));
    }
    return std::nullopt;
}

} // namespace

Tables find_tables(std::span<const u8> font, u32 font_start) {
    Reader r{font};
    Tables t;
    u32 count = r.u16_(font_start + 4);
    for (u32 i = 0; i < count && r.ok; ++i) {
        u64 rec = font_start + 12 + 16 * u64{i};
        u32 tag = r.u32_(rec), offset = r.u32_(rec + 8), length = r.u32_(rec + 12);
        if (!r.has(offset, length)) {
            r.ok = true; // skip a table that runs past the end
            continue;
        }
        switch (tag) {
        case 0x73626978: t.sbix = offset; break; // sbix
        case 0x43424C43: t.cblc = offset; break; // CBLC
        case 0x43424454: t.cbdt = offset; break; // CBDT
        case 0x434F4C52: t.colr = offset; break; // COLR
        case 0x4350414C: t.cpal = offset; break; // CPAL
        case 0x68656164: t.units_per_em = r.u16_(u64{offset} + 18); break; // head
        case 0x6D617870: t.glyph_count = r.u16_(u64{offset} + 4); break;   // maxp
        default: break;
        }
    }
    return t;
}

std::optional<Bitmap> bitmap_glyph(std::span<const u8> font, const Tables& t, u32 glyph, f32 ppem) {
    Reader r{font};
    if (t.sbix)
        if (auto b = sbix_glyph(r, t, glyph, ppem)) return b;
    r.ok = true;
    if (t.cblc && t.cbdt)
        if (auto b = cbdt_glyph(r, t, glyph, ppem)) return b;
    return std::nullopt;
}

std::vector<std::pair<u32, std::optional<Color>>> layers(std::span<const u8> font, const Tables& t, u32 glyph) {
    std::vector<std::pair<u32, std::optional<Color>>> out;
    if (!t.colr || !t.cpal) return out;
    Reader r{font};
    u32 bases = r.u16_(t.colr + 2);
    u64 base_records = t.colr + u64{r.u32_(t.colr + 4)}, layer_records = t.colr + u64{r.u32_(t.colr + 8)};
    u32 layer_count = r.u16_(t.colr + 12);
    // Base glyph records are sorted by glyph id.
    u32 lo = 0, hi = bases;
    while (lo < hi && r.ok) {
        u32 mid = (lo + hi) / 2;
        u32 id = r.u16_(base_records + 6 * u64{mid});
        if (id < glyph) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= bases || r.u16_(base_records + 6 * u64{lo}) != glyph) return out;
    u32 first = r.u16_(base_records + 6 * u64{lo} + 2), n = r.u16_(base_records + 6 * u64{lo} + 4);
    u32 entries = r.u16_(t.cpal + 2), records = r.u16_(t.cpal + 6);
    u64 colors = t.cpal + u64{r.u32_(t.cpal + 8)};
    u32 palette_start = r.u16_(t.cpal + 12); // palette 0
    for (u32 i = first; i < first + n && i < layer_count && r.ok; ++i) {
        u32 layer_glyph = r.u16_(layer_records + 4 * u64{i}), index = r.u16_(layer_records + 4 * u64{i} + 2);
        std::optional<Color> c;
        if (index != 0xFFFF && index < entries && palette_start + index < records) {
            u64 at = colors + 4 * u64{palette_start + index}; // B, G, R, A
            c = Color{r.u8_(at + 2) / 255.0f, r.u8_(at + 1) / 255.0f, r.u8_(at) / 255.0f, r.u8_(at + 3) / 255.0f};
        }
        out.emplace_back(layer_glyph, c);
    }
    if (!r.ok) out.clear();
    return out;
}

std::vector<u8> resize(std::span<const u8> rgba, u32 w, u32 h, u32 nw, u32 nh) {
    std::vector<u8> out(static_cast<usize>(nw) * nh * 4);
    f32 sx = static_cast<f32>(w) / static_cast<f32>(nw), sy = static_cast<f32>(h) / static_cast<f32>(nh);
    for (u32 y = 0; y < nh; ++y)
        for (u32 x = 0; x < nw; ++x) {
            f32 sum[4] = {0, 0, 0, 0};
            if (sx > 1 || sy > 1) { // shrinking: average the source area this pixel covers
                f32 x0 = static_cast<f32>(x) * sx, x1 = x0 + sx, y0 = static_cast<f32>(y) * sy, y1 = y0 + sy;
                f32 total = 0;
                for (u32 py = static_cast<u32>(y0); py < std::min(h, static_cast<u32>(std::ceil(y1))); ++py) {
                    f32 wy = std::min(y1, static_cast<f32>(py + 1)) - std::max(y0, static_cast<f32>(py));
                    for (u32 px = static_cast<u32>(x0); px < std::min(w, static_cast<u32>(std::ceil(x1))); ++px) {
                        f32 weight = wy * (std::min(x1, static_cast<f32>(px + 1)) - std::max(x0, static_cast<f32>(px)));
                        const u8* p = &rgba[(static_cast<usize>(py) * w + px) * 4];
                        for (int c = 0; c < 4; ++c) sum[c] += p[c] * weight;
                        total += weight;
                    }
                }
                for (f32& s : sum) s /= std::max(total, 1e-6f);
            } else { // growing: bilinear
                f32 fx = std::clamp((static_cast<f32>(x) + 0.5f) * sx - 0.5f, 0.0f, static_cast<f32>(w - 1));
                f32 fy = std::clamp((static_cast<f32>(y) + 0.5f) * sy - 0.5f, 0.0f, static_cast<f32>(h - 1));
                u32 ix = static_cast<u32>(fx), iy = static_cast<u32>(fy);
                u32 ix1 = std::min(ix + 1, w - 1), iy1 = std::min(iy + 1, h - 1);
                f32 tx = fx - static_cast<f32>(ix), ty = fy - static_cast<f32>(iy);
                auto at = [&](u32 px, u32 py, int c) { return static_cast<f32>(rgba[(static_cast<usize>(py) * w + px) * 4 + c]); };
                for (int c = 0; c < 4; ++c)
                    sum[c] = (at(ix, iy, c) * (1 - tx) + at(ix1, iy, c) * tx) * (1 - ty) + (at(ix, iy1, c) * (1 - tx) + at(ix1, iy1, c) * tx) * ty;
            }
            u8* o = &out[(static_cast<usize>(y) * nw + x) * 4];
            for (int c = 0; c < 4; ++c) o[c] = static_cast<u8>(std::clamp(std::lround(sum[c]), 0L, 255L));
        }
    return out;
}

} // namespace bowl::gfx::color
