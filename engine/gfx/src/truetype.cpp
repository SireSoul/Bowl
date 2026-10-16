#include "truetype.hpp"

#include "font_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
// stb asserts on some malformed fonts. With the checks below a broken font gives empty glyphs, in every build, instead
// of stopping a debug build.
#define STBTT_assert(x) ((void)0)
// A TrueType contour that starts off the curve on the glyph's last point makes stb read one vertex past its array (real
// fonts have them: Times on macOS). Every allocation gets room for one more, zeroed, so that read stays inside.
#define STBTT_malloc(size, user) ((void)(user), std::calloc((size) + 16, 1))
#define STBTT_free(pointer, user) ((void)(user), std::free(pointer))
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace bowl::gfx::truetype {

namespace {

constexpr u32 tag(const char (&t)[5]) {
    return u32(u8(t[0])) << 24 | u32(u8(t[1])) << 16 | u32(u8(t[2])) << 8 | u32(u8(t[3]));
}

// Limits no real glyph comes near. Composite glyphs are read again for every use of every part, so one that uses
// parts that use parts... could take forever; and stb keeps coordinates in 16 bits.
constexpr u32 max_vertices = 1 << 16;
constexpr u32 max_visits = 4096;
constexpr int max_depth = 16;
constexpr u32 max_charstring_ops = 1 << 16;
constexpr f32 max_coordinate = 32767;
// stb's distance fields compare every pixel with every point of the outline: a glyph both big and intricate would take
// seconds. Real glyphs at Font::sdf_size need a few hundred thousand.
constexpr u64 max_sdf_work = u64{1} << 24;

// stb puts offsets in ints, and its buffers refuse anything over 1 GB.
constexpr usize max_font_size = 0x40000000;

} // namespace

std::optional<u32> font_start(std::span<const u8> data) {
    FontReader r{data};
    u32 version = r.u32_(0);
    if (!r.ok || data.size() >= max_font_size) return std::nullopt;
    // The versions stb takes as a font (stbtt__isfont), or a collection whose first font it would read.
    if (version == 0x00010000 || version == 0x31000000 || version == tag("typ1") || version == tag("OTTO") ||
        version == tag("true"))
        return 0;
    if (version != tag("ttcf")) return std::nullopt;
    u32 collection = r.u32_(4);
    if (collection != 0x00010000 && collection != 0x00020000) return std::nullopt;
    if (static_cast<i32>(r.u32_(8)) <= 0) return std::nullopt; // how many fonts
    u32 start = r.u32_(12);
    if (!r.ok || start >= data.size()) return std::nullopt;
    return start;
}

bool Font::load(std::span<const u8> data, u32 start, bool bitmap_glyphs) {
    data_ = data;
    outlines_.clear();
    FontReader r{data};
    if (data.size() >= max_font_size) return false;

    // The table directory. stb finds tables by tag, the first one listed, and trusts its offset; here a table that
    // runs past the end of the file counts as missing.
    u32 tables = r.u16_(u64{start} + 4);
    if (!r.has(u64{start} + 12, 16 * u64{tables})) return false;
    auto find = [&](u32 want) -> Table {
        for (u32 i = 0; i < tables; ++i) {
            u64 record = u64{start} + 12 + 16 * u64{i};
            if (r.u32_(record) != want) continue;
            u32 offset = r.u32_(record + 8), length = r.u32_(record + 12);
            if (u64{offset} + length > data.size()) return {};
            return {offset, length};
        }
        return {};
    };
    Table cmap = find(tag("cmap")), head = find(tag("head")), hhea = find(tag("hhea")), hmtx = find(tag("hmtx")),
          maxp = find(tag("maxp"));
    if (!cmap || !head || head.length < 54 || !hhea || hhea.length < 36 || !hmtx || !maxp || maxp.length < 6)
        return false;

    // Metrics. stb divides by ascent - descent for the scale (the spec's smallest em is 16 units, so less is broken),
    // and reads every glyph's advance from hmtx without looking at its size.
    glyph_count_ = r.u16_(u64{maxp.offset} + 4);
    i32 ascent = r.i16_(u64{hhea.offset} + 4), descent = r.i16_(u64{hhea.offset} + 6);
    u32 long_metrics = r.u16_(u64{hhea.offset} + 34);
    u64 metrics = glyph_count_ <= long_metrics ? 4 * u64{glyph_count_}
                                               : 4 * u64{long_metrics} + 2 * u64{glyph_count_ - long_metrics};
    if (glyph_count_ == 0 || ascent - descent < 16 || long_metrics == 0 || hmtx.length < metrics) return false;

    // The character map stb would use: the last Unicode one listed.
    u32 maps = r.u16_(u64{cmap.offset} + 2);
    if (4 + 8 * u64{maps} > cmap.length) return false;
    u32 index_map = 0;
    for (u32 i = 0; i < maps; ++i) {
        u64 record = u64{cmap.offset} + 4 + 8 * u64{i};
        u16 platform = r.u16_(record), encoding = r.u16_(record + 2);
        if (platform == STBTT_PLATFORM_ID_UNICODE ||
            (platform == STBTT_PLATFORM_ID_MICROSOFT &&
             (encoding == STBTT_MS_EID_UNICODE_BMP || encoding == STBTT_MS_EID_UNICODE_FULL)))
            index_map = r.u32_(record + 4);
    }
    if (index_map == 0 || index_map >= cmap.length) return false;
    cmap_ = {cmap.offset + index_map, cmap.length - index_map};

    // What stb_InitFont would set up, from the checked tables. Kerning and character lookup never reach stb.
    info_ = {};
    info_.data = const_cast<u8*>(data.data()); // stb keeps a non-const pointer but only reads through it
    info_.fontstart = static_cast<int>(start);
    info_.head = static_cast<int>(head.offset);
    info_.hhea = static_cast<int>(hhea.offset);
    info_.hmtx = static_cast<int>(hmtx.offset);
    info_.index_map = static_cast<int>(cmap_.offset);
    info_.indexToLocFormat = r.u16_(u64{head.offset} + 50);
    info_.svg = 0; // none: stb would go looking for an SVG table otherwise
    info_.cff = info_.charstrings = info_.gsubrs = info_.subrs = info_.fontdicts = info_.fdselect =
        stbtt__new_buf(nullptr, 0);
    kern_ = find(tag("kern"));
    gpos_ = find(tag("GPOS"));

    // Outlines: TrueType (glyf, with loca saying where each glyph is) or CFF. Fonts made only of pictures have none:
    // numGlyphs 0 makes stb find no outline for any glyph, and the pictures come from the color tables.
    glyf_ = find(tag("glyf"));
    loca_ = find(tag("loca"));
    Table cff = find(tag("CFF "));
    u32 loca_entry = info_.indexToLocFormat == 0 ? 2 : 4;
    bool outlines = false;
    if (glyf_) {
        outlines = info_.indexToLocFormat <= 1 && loca_ && loca_.length >= (u64{glyph_count_} + 1) * loca_entry;
        if (outlines) {
            info_.glyf = static_cast<int>(glyf_.offset);
            info_.loca = static_cast<int>(loca_.offset);
        }
    } else if (cff) {
        outlines = load_cff(cff);
    }
    if (!outlines && !bitmap_glyphs) return false;
    info_.numGlyphs = outlines ? static_cast<int>(glyph_count_) : 0;
    return r.ok;
}

// stbtt_InitFont's CFF part, with the table's real length where stb assumes 512 MB: stb reads CFF data only through
// these buffers, which stop at their ends.
bool Font::load_cff(Table cff) {
    stbtt__buf b = stbtt__new_buf(data_.data() + cff.offset, cff.length);
    stbtt_fontinfo& info = info_;
    info.cff = b;
    stbtt__buf_skip(&b, 2);
    stbtt__buf_seek(&b, stbtt__buf_get8(&b)); // header size
    stbtt__cff_get_index(&b);                 // names
    stbtt__buf top_dicts = stbtt__cff_get_index(&b);
    stbtt__buf top = stbtt__cff_index_get(top_dicts, 0);
    stbtt__cff_get_index(&b); // strings
    info.gsubrs = stbtt__cff_get_index(&b);
    stbtt_uint32 charstring_type = 2, charstrings = 0, fd_array = 0, fd_select = 0;
    stbtt__dict_get_ints(&top, 17, 1, &charstrings);
    stbtt__dict_get_ints(&top, 0x100 | 6, 1, &charstring_type);
    stbtt__dict_get_ints(&top, 0x100 | 36, 1, &fd_array);
    stbtt__dict_get_ints(&top, 0x100 | 37, 1, &fd_select);
    info.subrs = stbtt__get_subrs(b, top);
    if (charstring_type != 2 || charstrings == 0) return false;
    if (fd_array) { // a CID font
        if (!fd_select) return false;
        stbtt__buf_seek(&b, static_cast<int>(fd_array));
        info.fontdicts = stbtt__cff_get_index(&b);
        info.fdselect = stbtt__buf_range(&b, static_cast<int>(fd_select), b.size - static_cast<int>(fd_select));
    }
    stbtt__buf_seek(&b, static_cast<int>(charstrings));
    info.charstrings = stbtt__cff_get_index(&b);
    return true;
}

// Character lookup: stbtt_FindGlyphIndex, reading only inside the character map.
int Font::glyph_for(u32 codepoint) const {
    FontReader r{cmap_.in(data_)};
    u32 found = 0;
    u16 format = r.u16_(0);
    if (format == 0) { // a byte per character
        u32 bytes = r.u16_(2);
        if (codepoint + 6 < bytes) found = r.u8_(6 + u64{codepoint});
    } else if (format == 6) { // a range of characters
        u32 first = r.u16_(6), count = r.u16_(8);
        if (codepoint >= first && codepoint < first + count) found = r.u16_(10 + 2 * u64{codepoint - first});
    } else if (format == 4 && codepoint <= 0xFFFF) { // segments, binary searched as stb does
        u32 segments = r.u16_(6) >> 1, range = r.u16_(8) >> 1, selector = r.u16_(10), shift = r.u16_(12) >> 1;
        u64 search = 14;
        if (codepoint >= r.u16_(search + 2 * u64{shift})) search += 2 * u64{shift};
        search -= 2;
        for (; selector; --selector) {
            range >>= 1;
            if (codepoint > r.u16_(search + 2 * u64{range})) search += 2 * u64{range};
        }
        search += 2;
        u64 item = static_cast<u16>((search - 14) >> 1);
        u32 start = r.u16_(14 + 2 * u64{segments} + 2 + 2 * item), last = r.u16_(14 + 2 * item);
        if (codepoint >= start && codepoint <= last) {
            u64 range_offset = 14 + 6 * u64{segments} + 2 + 2 * item;
            u16 offset = r.u16_(range_offset);
            if (offset == 0)
                found = static_cast<u16>(codepoint + static_cast<u32>(r.i16_(14 + 4 * u64{segments} + 2 + 2 * item)));
            else
                found = r.u16_(offset + 2 * u64{codepoint - start} + range_offset);
        }
    } else if (format == 12 || format == 13) { // groups of characters
        i64 low = 0, high = static_cast<i32>(r.u32_(12));
        while (low < high && r.ok) {
            i64 mid = low + ((high - low) >> 1);
            u64 group = 16 + 12 * static_cast<u64>(mid);
            u32 first = r.u32_(group), last = r.u32_(group + 4);
            if (codepoint < first) {
                high = mid;
            } else if (codepoint > last) {
                low = mid + 1;
            } else {
                u32 start = r.u32_(group + 8);
                found = format == 12 ? start + (codepoint - first) : start;
                break;
            }
        }
    }
    return r.ok && found < glyph_count_ ? static_cast<int>(found) : 0;
}

// Kerning: stbtt_GetGlyphKernAdvance (GPOS if the font has it, else kern), reading only inside those tables.
int Font::kerning(int left, int right) const {
    if (left < 0 || right < 0 || left > 0xFFFF || right > 0xFFFF) return 0;
    if (gpos_) return gpos_kerning(left, right);
    if (kern_) return kern_kerning(left, right);
    return 0;
}

int Font::kern_kerning(int left, int right) const {
    FontReader r{kern_.in(data_)};
    if (r.u16_(2) < 1 || r.u16_(8) != 1) return 0; // the first table must be horizontal, format 0
    u32 needle = static_cast<u32>(left) << 16 | static_cast<u32>(right);
    i64 l = 0, h = i64{r.u16_(10)} - 1;
    while (l <= h && r.ok) {
        i64 m = (l + h) >> 1;
        u32 straw = r.u32_(18 + 6 * static_cast<u64>(m));
        if (needle < straw)
            h = m - 1;
        else if (needle > straw)
            l = m + 1;
        else
            return r.ok ? r.i16_(22 + 6 * static_cast<u64>(m)) : 0;
    }
    return 0;
}

namespace {

// stbtt__GetCoverageIndex and stbtt__GetGlyphClass, checked: -1 where stb gives -1 (or would read out of bounds).
i64 coverage_index(const FontReader& r, u64 table, int glyph) {
    u16 format = r.u16_(table);
    i64 l = 0, h = i64{r.u16_(table + 2)} - 1;
    while (l <= h && r.ok) {
        i64 m = (l + h) >> 1;
        if (format == 1) {
            int straw = r.u16_(table + 4 + 2 * static_cast<u64>(m));
            if (glyph < straw)
                h = m - 1;
            else if (glyph > straw)
                l = m + 1;
            else
                return r.ok ? m : -1;
        } else if (format == 2) {
            u64 record = table + 4 + 6 * static_cast<u64>(m);
            int first = r.u16_(record), last = r.u16_(record + 2);
            if (glyph < first)
                h = m - 1;
            else if (glyph > last)
                l = m + 1;
            else
                return r.ok ? i64{r.u16_(record + 4)} + glyph - first : -1;
        } else {
            return -1;
        }
    }
    return -1;
}

i64 glyph_class(const FontReader& r, u64 table, int glyph) {
    u16 format = r.u16_(table);
    if (format == 1) {
        int first = r.u16_(table + 2), count = r.u16_(table + 4);
        if (glyph >= first && glyph < first + count) return r.u16_(table + 6 + 2 * u64(glyph - first));
        return 0;
    }
    if (format != 2) return -1;
    i64 l = 0, h = i64{r.u16_(table + 2)} - 1;
    while (l <= h && r.ok) {
        i64 m = (l + h) >> 1;
        u64 record = table + 4 + 6 * static_cast<u64>(m);
        int first = r.u16_(record), last = r.u16_(record + 2);
        if (glyph < first)
            h = m - 1;
        else if (glyph > last)
            l = m + 1;
        else
            return r.u16_(record + 4);
    }
    return 0; // glyphs in no class are in class 0
}

} // namespace

int Font::gpos_kerning(int left, int right) const {
    FontReader r{gpos_.in(data_)};
    if (r.u16_(0) != 1 || r.u16_(2) != 0) return 0; // version 1.0
    u64 lookups = r.u16_(8);
    u32 lookup_count = r.u16_(lookups);
    for (u32 i = 0; i < lookup_count && r.ok; ++i) {
        u64 lookup = lookups + r.u16_(lookups + 2 + 2 * u64{i});
        if (r.u16_(lookup) != 2) continue; // pair adjustment
        u32 subtables = r.u16_(lookup + 4);
        for (u32 k = 0; k < subtables && r.ok; ++k) {
            u64 table = lookup + r.u16_(lookup + 6 + 2 * u64{k});
            u16 format = r.u16_(table);
            i64 coverage = coverage_index(r, table + r.u16_(table + 2), left);
            if (coverage == -1) continue;
            // Only a plain x advance for the first glyph, as stb reads.
            if ((format != 1 && format != 2) || r.u16_(table + 4) != 4 || r.u16_(table + 6) != 0) return 0;
            if (format == 1) {
                if (coverage >= r.u16_(table + 8)) return 0;
                u64 pairs = table + r.u16_(table + 10 + 2 * static_cast<u64>(coverage));
                i64 l = 0, h = i64{r.u16_(pairs)} - 1;
                while (l <= h && r.ok) {
                    i64 m = (l + h) >> 1;
                    u64 pair = pairs + 2 + 4 * static_cast<u64>(m);
                    int straw = r.u16_(pair);
                    if (right < straw)
                        h = m - 1;
                    else if (right > straw)
                        l = m + 1;
                    else
                        return r.ok ? r.i16_(pair + 2) : 0;
                }
                continue; // not in this subtable: the next may have it
            }
            i64 left_class = glyph_class(r, table + r.u16_(table + 8), left);
            i64 right_class = glyph_class(r, table + r.u16_(table + 10), right);
            i64 left_classes = r.u16_(table + 12), right_classes = r.u16_(table + 14);
            if (left_class < 0 || left_class >= left_classes || right_class < 0 || right_class >= right_classes)
                return 0;
            i16 advance = r.i16_(table + 16 + 2 * static_cast<u64>(left_class * right_classes + right_class));
            return r.ok ? advance : 0;
        }
    }
    return 0;
}

// Whether stb may read a glyph's outline.
bool Font::outline(int glyph) const {
    // Past numGlyphs a TrueType font has no outline, but stb's CFF reading doesn't look, so neither gets asked.
    if (glyph < 0 || glyph >= info_.numGlyphs) return false;
    if (info_.cff.size) {
        auto [it, fresh] = outlines_.try_emplace(static_cast<u32>(glyph));
        if (fresh)
            it->second.state =
                check_charstring(glyph, it->second.vertices) ? Outline::State::Good : Outline::State::Bad;
        return it->second.state == Outline::State::Good;
    }
    return check_glyf(static_cast<u32>(glyph), 0).state == Outline::State::Good;
}

// A TrueType glyph, read the way stbtt__GetGlyphShapeTT reads it: every byte inside the glyph's own data, every point
// stb makes counted, and every coordinate one stb can keep in 16 bits.
const Font::Outline& Font::check_glyf(u32 glyph, int depth) const {
    static const Outline bad{Outline::State::Bad};
    auto [it, fresh] = outlines_.try_emplace(glyph);
    Outline& out = it->second;
    if (!fresh) return out.state == Outline::State::Checking ? bad : out; // a composite that contains itself
    if (depth > max_depth) {
        outlines_.erase(it);
        return bad;
    }
    auto fail = [&]() -> const Outline& {
        out.state = Outline::State::Bad;
        return out;
    };
    // Where it is (loca was checked to hold every glyph's entry when the font loaded).
    FontReader table{loca_.in(data_)};
    u64 g0 = info_.indexToLocFormat == 0 ? 2 * u64{table.u16_(2 * u64{glyph})} : table.u32_(4 * u64{glyph});
    u64 g1 = info_.indexToLocFormat == 0 ? 2 * u64{table.u16_(2 * u64{glyph} + 2)} : table.u32_(4 * u64{glyph} + 4);
    if (g0 == g1) { // empty
        out.state = Outline::State::Good;
        return out;
    }
    if (g0 > g1 || g1 > glyf_.length) return fail();
    FontReader r{data_.subspan(glyf_.offset + g0, g1 - g0)};
    i16 contours = r.i16_(0);

    if (contours > 0) {
        u64 ends = 10;
        u32 instructions = r.u16_(ends + 2 * u64(contours));
        u32 n = 1u + r.u16_(ends + 2 * u64(contours) - 2); // points
        u64 at = ends + 2 * u64(contours) + 2 + instructions;
        std::vector<u8> flags(n);
        u8 f = 0, repeat = 0;
        for (u32 i = 0; i < n; ++i) {
            if (repeat == 0) {
                f = r.u8_(at++);
                if (f & 8) repeat = r.u8_(at++);
            } else {
                --repeat;
            }
            flags[i] = f;
        }
        out.x0 = out.y0 = out.x1 = out.y1 = 0;
        for (int axis = 0; axis < 2; ++axis) { // x, then y
            u8 short_bit = axis == 0 ? 2 : 4, same_bit = axis == 0 ? 16 : 32;
            i32 v = 0;
            for (u32 i = 0; i < n; ++i) {
                if (flags[i] & short_bit) {
                    i32 d = r.u8_(at++);
                    v += (flags[i] & same_bit) ? d : -d;
                } else if (!(flags[i] & same_bit)) {
                    v += r.i16_(at);
                    at += 2;
                }
                i32 c = static_cast<i16>(v); // stb keeps each in 16 bits
                if (axis == 0)
                    out.x0 = std::min(out.x0, c), out.x1 = std::max(out.x1, c);
                else
                    out.y0 = std::min(out.y0, c), out.y1 = std::max(out.y1, c);
            }
        }
        if (!r.ok) return fail();
        // stb's walk through the contours, counting the vertices it writes into room for n + 2 * contours. A contour
        // that starts off the curve looks at the point after it; after the last point that's the zeroed spare vertex
        // STBTT_malloc adds, an off-curve point.
        u32 vertices = 0, next_move = 0, contour = 0;
        bool was_off = false, start_off = false;
        auto close = [&] { vertices += start_off && was_off ? 2 : 1; };
        for (u32 i = 0; i < n; ++i) {
            if (next_move == i) {
                if (i != 0) close();
                start_off = !(flags[i] & 1);
                if (start_off && i + 1 < n && (flags[i + 1] & 1)) ++i;
                ++vertices;
                was_off = false;
                if (contour >= u32(contours)) return fail();
                next_move = 1u + r.u16_(ends + 2 * u64{contour++});
            } else if (!(flags[i] & 1)) {
                vertices += was_off ? 1 : 0;
                was_off = true;
            } else {
                ++vertices;
                was_off = false;
            }
        }
        close();
        if (vertices > n + 2 * u32(contours) || vertices > max_vertices) return fail();
        out.vertices = vertices;
        out.visits = 1;
    } else if (contours < 0) { // composite: parts placed with offsets and scales
        out.x0 = out.y0 = out.x1 = out.y1 = 0;
        out.visits = 1;
        u64 at = 10;
        for (bool more = true; more;) {
            u16 flags = r.u16_(at), part = r.u16_(at + 2);
            at += 4;
            f32 m[6] = {1, 0, 0, 1, 0, 0};
            if (flags & 2) { // offsets (stb skips nothing without them)
                if (flags & 1)
                    m[4] = r.i16_(at), m[5] = r.i16_(at + 2), at += 4;
                else
                    m[4] = static_cast<i8>(r.u8_(at)), m[5] = static_cast<i8>(r.u8_(at + 1)), at += 2;
            }
            if (flags & 8) {
                m[0] = m[3] = r.i16_(at) / 16384.0f, at += 2;
            } else if (flags & 64) {
                m[0] = r.i16_(at) / 16384.0f, m[3] = r.i16_(at + 2) / 16384.0f, at += 4;
            } else if (flags & 128) {
                for (int k = 0; k < 4; ++k) m[k] = r.i16_(at + 2 * u64(k)) / 16384.0f;
                at += 8;
            }
            more = flags & 32;
            if (!r.ok) return fail();
            if (part >= glyph_count_) continue; // stb finds no outline there
            const Outline& p = check_glyf(part, depth + 1);
            if (p.state != Outline::State::Good) return fail();
            out.visits += p.visits;
            if (out.visits > max_visits) return fail();
            if (p.vertices == 0) continue;
            // stb moves each point to sx * (a x + c y + e), sy * (b x + d y + f) and keeps it in 16 bits.
            f32 sx = std::sqrt(m[0] * m[0] + m[1] * m[1]), sy = std::sqrt(m[2] * m[2] + m[3] * m[3]);
            for (i32 x : {p.x0, p.x1})
                for (i32 y : {p.y0, p.y1}) {
                    f32 tx = sx * (m[0] * f32(x) + m[2] * f32(y) + m[4]),
                        ty = sy * (m[1] * f32(x) + m[3] * f32(y) + m[5]);
                    if (!(std::abs(tx) <= max_coordinate && std::abs(ty) <= max_coordinate)) return fail();
                    out.x0 = std::min(out.x0, i32(tx)), out.x1 = std::max(out.x1, i32(tx));
                    out.y0 = std::min(out.y0, i32(ty)), out.y1 = std::max(out.y1, i32(ty));
                }
            out.vertices += p.vertices;
            if (out.vertices > max_vertices) return fail();
        }
    }
    out.state = Outline::State::Good;
    return out;
}

// A CFF glyph, run the way stbtt__run_charstring runs it, but counting: stb converts each point to an int (undefined
// past its range) and to 16 bits, and nests subroutines ten deep with nothing to stop a glyph from calling the same
// long subroutine over and over.
bool Font::check_charstring(int glyph, u32& vertices) const {
    stbtt__buf b = stbtt__cff_index_get(info_.charstrings, glyph), subrs = info_.subrs, stack[10];
    int depth = 0, sp = 0, mask_bits = 0;
    bool in_header = true, has_subrs = false;
    f32 s[48];
    f32 x = 0, y = 0;
    u32 ops = 0;
    vertices = 0;
    auto to = [&](f32 dx, f32 dy) {
        x += dx, y += dy;
        ++vertices;
        return std::abs(x) <= max_coordinate && std::abs(y) <= max_coordinate;
    };
    auto curve = [&](f32 dx1, f32 dy1, f32 dx2, f32 dy2, f32 dx3, f32 dy3) {
        f32 cx1 = x + dx1, cy1 = y + dy1, cx2 = cx1 + dx2, cy2 = cy1 + dy2;
        if (!(std::abs(cx1) <= max_coordinate && std::abs(cy1) <= max_coordinate && std::abs(cx2) <= max_coordinate &&
              std::abs(cy2) <= max_coordinate))
            return false;
        x = cx2, y = cy2;
        return to(dx3, dy3);
    };
    while (b.cursor < b.size) {
        if (++ops > max_charstring_ops || vertices > max_vertices) return false;
        int i = 0;
        bool clear = true;
        int b0 = stbtt__buf_get8(&b);
        switch (b0) {
        case 0x13: // hintmask
        case 0x14: // cntrmask
            if (in_header) mask_bits += sp / 2;
            in_header = false;
            stbtt__buf_skip(&b, (mask_bits + 7) / 8);
            break;
        case 0x01:
        case 0x03:
        case 0x12:
        case 0x17: // stems
            mask_bits += sp / 2;
            break;
        case 0x15: // rmoveto
            in_header = false;
            if (sp < 2 || !to(s[sp - 2], s[sp - 1])) return false;
            break;
        case 0x04: // vmoveto
            in_header = false;
            if (sp < 1 || !to(0, s[sp - 1])) return false;
            break;
        case 0x16: // hmoveto
            in_header = false;
            if (sp < 1 || !to(s[sp - 1], 0)) return false;
            break;
        case 0x05: // rlineto
            if (sp < 2) return false;
            for (; i + 1 < sp; i += 2)
                if (!to(s[i], s[i + 1])) return false;
            break;
        case 0x06: // hlineto, then alternating
        case 0x07: // vlineto, then alternating
            if (sp < 1) return false;
            for (bool horizontal = b0 == 0x06; i < sp; ++i, horizontal = !horizontal)
                if (!(horizontal ? to(s[i], 0) : to(0, s[i]))) return false;
            break;
        case 0x1E: // vhcurveto
        case 0x1F: // hvcurveto
            if (sp < 4) return false;
            for (bool vertical = b0 == 0x1E; i + 3 < sp; i += 4, vertical = !vertical) {
                f32 last = sp - i == 5 ? s[i + 4] : 0.0f;
                if (!(vertical ? curve(0, s[i], s[i + 1], s[i + 2], s[i + 3], last)
                               : curve(s[i], 0, s[i + 1], s[i + 2], last, s[i + 3])))
                    return false;
            }
            break;
        case 0x08: // rrcurveto
            if (sp < 6) return false;
            for (; i + 5 < sp; i += 6)
                if (!curve(s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5])) return false;
            break;
        case 0x18: // rcurveline
            if (sp < 8) return false;
            for (; i + 5 < sp - 2; i += 6)
                if (!curve(s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5])) return false;
            if (i + 1 >= sp || !to(s[i], s[i + 1])) return false;
            break;
        case 0x19: // rlinecurve
            if (sp < 8) return false;
            for (; i + 1 < sp - 6; i += 2)
                if (!to(s[i], s[i + 1])) return false;
            if (i + 5 >= sp || !curve(s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5])) return false;
            break;
        case 0x1A:   // vvcurveto
        case 0x1B: { // hhcurveto
            if (sp < 4) return false;
            f32 f = 0;
            if (sp & 1) f = s[i++];
            for (; i + 3 < sp; i += 4, f = 0)
                if (!(b0 == 0x1B ? curve(s[i], f, s[i + 1], s[i + 2], s[i + 3], 0)
                                 : curve(f, s[i], s[i + 1], s[i + 2], 0, s[i + 3])))
                    return false;
            break;
        }
        case 0x0A: // callsubr
            if (!has_subrs) {
                if (info_.fdselect.size) subrs = stbtt__cid_get_glyph_subrs(&info_, glyph);
                has_subrs = true;
            }
            [[fallthrough]];
        case 0x1D: { // callgsubr
            if (sp < 1 || depth >= 10) return false;
            int v = static_cast<int>(s[--sp]);
            stack[depth++] = b;
            b = stbtt__get_subr(b0 == 0x0A ? subrs : info_.gsubrs, v);
            if (b.size == 0) return false;
            b.cursor = 0;
            clear = false;
            break;
        }
        case 0x0B: // return
            if (depth <= 0) return false;
            b = stack[--depth];
            clear = false;
            break;
        case 0x0E: // endchar
            return vertices + 1 <= max_vertices;
        case 0x0C: { // flex and friends: two curves
            int b1 = stbtt__buf_get8(&b);
            bool ok = false;
            if (b1 == 0x22 && sp >= 7) { // hflex
                ok = curve(s[0], 0, s[1], s[2], s[3], 0) && curve(s[4], 0, s[5], -s[2], s[6], 0);
            } else if (b1 == 0x23 && sp >= 13) { // flex
                ok = curve(s[0], s[1], s[2], s[3], s[4], s[5]) && curve(s[6], s[7], s[8], s[9], s[10], s[11]);
            } else if (b1 == 0x24 && sp >= 9) { // hflex1
                ok = curve(s[0], s[1], s[2], s[3], s[4], 0) && curve(s[5], 0, s[6], s[7], s[8], -(s[1] + s[3] + s[7]));
            } else if (b1 == 0x25 && sp >= 11) { // flex1
                f32 dx = s[0] + s[2] + s[4] + s[6] + s[8], dy = s[1] + s[3] + s[5] + s[7] + s[9];
                f32 dx6 = s[10], dy6 = s[10];
                if (std::fabs(dx) > std::fabs(dy))
                    dy6 = -dy;
                else
                    dx6 = -dx;
                ok = curve(s[0], s[1], s[2], s[3], s[4], s[5]) && curve(s[6], s[7], s[8], s[9], dx6, dy6);
            }
            if (!ok) return false;
            break;
        }
        default: // a number
            if (b0 != 255 && b0 != 28 && b0 < 32) return false;
            f32 f;
            if (b0 == 255) {
                f = static_cast<f32>(static_cast<stbtt_int32>(stbtt__buf_get32(&b))) / 0x10000;
            } else {
                stbtt__buf_skip(&b, -1);
                f = static_cast<f32>(static_cast<stbtt_int16>(stbtt__cff_int(&b)));
            }
            if (sp >= 48) return false;
            s[sp++] = f;
            clear = false;
            break;
        }
        if (clear) sp = 0;
    }
    return false; // no endchar: stb draws nothing
}

void Font::vertical_metrics(int& ascent, int& descent, int& line_gap) const {
    stbtt_GetFontVMetrics(&info_, &ascent, &descent, &line_gap);
}

void Font::horizontal_metrics(int glyph, int& advance, int& left_bearing) const {
    advance = left_bearing = 0;
    if (glyph >= 0 && static_cast<u32>(glyph) < glyph_count_)
        stbtt_GetGlyphHMetrics(&info_, glyph, &advance, &left_bearing);
}

void Font::bitmap_box(int glyph, f32 scale, int& x0, int& y0, int& x1, int& y1) const {
    x0 = y0 = x1 = y1 = 0;
    if (outline(glyph)) stbtt_GetGlyphBitmapBox(&info_, glyph, scale, scale, &x0, &y0, &x1, &y1);
}

void Font::make_bitmap(u8* out, int width, int height, int stride, f32 scale, int glyph) const {
    if (outline(glyph)) stbtt_MakeGlyphBitmap(&info_, out, width, height, stride, scale, scale, glyph);
}

u8* Font::sdf(f32 scale, int glyph, int padding, u8 on_edge, f32 pixel_dist_scale, int& width, int& height,
              int& x_offset, int& y_offset) const {
    width = height = x_offset = y_offset = 0;
    if (!outline(glyph)) return nullptr;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&info_, glyph, scale, scale, &x0, &y0, &x1, &y1);
    u64 area =
        static_cast<u64>(std::max(0, x1 - x0) + 2 * padding) * static_cast<u64>(std::max(0, y1 - y0) + 2 * padding);
    if (area * outlines_.at(static_cast<u32>(glyph)).vertices > max_sdf_work) return nullptr;
    return stbtt_GetGlyphSDF(&info_, scale, glyph, padding, on_edge, pixel_dist_scale, &width, &height, &x_offset,
                             &y_offset);
}

} // namespace bowl::gfx::truetype
