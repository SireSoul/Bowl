#include "ph/gfx/bidi.hpp"

#include <algorithm>

namespace ph::gfx::bidi {

namespace {

struct Range {
    u32 first, last;
    Type type;
};

// Character types that aren't L, by range: right-to-left scripts, their marks and digits, numbers and their
// punctuation, spaces and separators, and neutral symbols. Sorted; the first match wins.
constexpr Range ranges[] = {
    {0x0009, 0x0009, Type::S},     {0x000A, 0x000A, Type::B},     {0x000B, 0x000B, Type::S},
    {0x000C, 0x000C, Type::WS},    {0x000D, 0x000D, Type::B},     {0x001C, 0x001E, Type::B},
    {0x001F, 0x001F, Type::S},     {0x0020, 0x0020, Type::WS},    {0x0021, 0x0022, Type::ON},
    {0x0023, 0x0025, Type::ET},    {0x0026, 0x002A, Type::ON},    {0x002B, 0x002B, Type::ES},
    {0x002C, 0x002C, Type::CS},    {0x002D, 0x002D, Type::ES},    {0x002E, 0x002F, Type::CS},
    {0x0030, 0x0039, Type::EN},    {0x003A, 0x003A, Type::CS},    {0x003B, 0x0040, Type::ON},
    {0x005B, 0x0060, Type::ON},    {0x007B, 0x007E, Type::ON},    {0x0085, 0x0085, Type::B},
    {0x00A0, 0x00A0, Type::CS},    {0x00A1, 0x00A1, Type::ON},    {0x00A2, 0x00A5, Type::ET},
    {0x00A6, 0x00A9, Type::ON},    {0x00AB, 0x00AC, Type::ON},    {0x00AE, 0x00AF, Type::ON},
    {0x00B0, 0x00B1, Type::ET},    {0x00B2, 0x00B3, Type::EN},    {0x00B4, 0x00B4, Type::ON},
    {0x00B6, 0x00B8, Type::ON},    {0x00B9, 0x00B9, Type::EN},    {0x00BB, 0x00BF, Type::ON},
    {0x00D7, 0x00D7, Type::ON},    {0x00F7, 0x00F7, Type::ON},    {0x0300, 0x036F, Type::NSM},
    {0x0483, 0x0489, Type::NSM},   {0x0591, 0x05BD, Type::NSM},   {0x05BE, 0x05BE, Type::R},
    {0x05BF, 0x05BF, Type::NSM},   {0x05C0, 0x05C0, Type::R},     {0x05C1, 0x05C2, Type::NSM},
    {0x05C3, 0x05C3, Type::R},     {0x05C4, 0x05C5, Type::NSM},   {0x05C6, 0x05C6, Type::R},
    {0x05C7, 0x05C7, Type::NSM},   {0x05C8, 0x05FF, Type::R},     {0x0600, 0x0605, Type::AN},
    {0x0606, 0x0607, Type::ON},    {0x0608, 0x0608, Type::AL},    {0x0609, 0x060A, Type::ET},
    {0x060B, 0x060B, Type::AL},    {0x060C, 0x060C, Type::CS},    {0x060D, 0x060D, Type::AL},
    {0x060E, 0x060F, Type::ON},    {0x0610, 0x061A, Type::NSM},   {0x061B, 0x064A, Type::AL},
    {0x064B, 0x065F, Type::NSM},   {0x0660, 0x0669, Type::AN},    {0x066A, 0x066A, Type::ET},
    {0x066B, 0x066C, Type::AN},    {0x066D, 0x066F, Type::AL},    {0x0670, 0x0670, Type::NSM},
    {0x0671, 0x06D5, Type::AL},    {0x06D6, 0x06DC, Type::NSM},   {0x06DD, 0x06DD, Type::AN},
    {0x06DE, 0x06DE, Type::ON},    {0x06DF, 0x06E4, Type::NSM},   {0x06E5, 0x06E6, Type::AL},
    {0x06E7, 0x06E8, Type::NSM},   {0x06E9, 0x06E9, Type::ON},    {0x06EA, 0x06ED, Type::NSM},
    {0x06EE, 0x06EF, Type::AL},    {0x06F0, 0x06F9, Type::EN},    {0x06FA, 0x0710, Type::AL},
    {0x0711, 0x0711, Type::NSM},   {0x0712, 0x072F, Type::AL},    {0x0730, 0x074A, Type::NSM},
    {0x074B, 0x07A5, Type::AL},    {0x07A6, 0x07B0, Type::NSM},   {0x07B1, 0x07BF, Type::AL},
    {0x07C0, 0x07EA, Type::R},     {0x07EB, 0x07F3, Type::NSM},   {0x07F4, 0x07FF, Type::R},
    {0x0800, 0x0815, Type::R},     {0x0816, 0x082D, Type::NSM},   {0x082E, 0x085F, Type::R},
    {0x0860, 0x08CF, Type::AL},    {0x08D3, 0x08E1, Type::NSM},   {0x08E2, 0x08E2, Type::AN},
    {0x08E3, 0x08FF, Type::NSM},   {0x1AB0, 0x1AFF, Type::NSM},   {0x1DC0, 0x1DFF, Type::NSM},
    {0x2000, 0x200A, Type::WS},    {0x200B, 0x200D, Type::ON},    {0x2010, 0x2027, Type::ON},
    {0x2028, 0x2028, Type::WS},    {0x2029, 0x2029, Type::B},     {0x202F, 0x202F, Type::CS},
    {0x2030, 0x2034, Type::ET},    {0x2035, 0x2043, Type::ON},    {0x2044, 0x2044, Type::CS},
    {0x2045, 0x205E, Type::ON},    {0x205F, 0x205F, Type::WS},    {0x2070, 0x2070, Type::EN},
    {0x2074, 0x2079, Type::EN},    {0x207A, 0x207B, Type::ES},    {0x207C, 0x207E, Type::ON},
    {0x2080, 0x2089, Type::EN},    {0x208A, 0x208B, Type::ES},    {0x208C, 0x208E, Type::ON},
    {0x20A0, 0x20CF, Type::ET},    {0x20D0, 0x20FF, Type::NSM},   {0x2100, 0x2101, Type::ON},
    {0x2103, 0x2106, Type::ON},    {0x2108, 0x2109, Type::ON},    {0x2114, 0x2114, Type::ON},
    {0x2116, 0x2118, Type::ON},    {0x211E, 0x2123, Type::ON},    {0x2190, 0x2211, Type::ON},
    {0x2212, 0x2212, Type::ES},    {0x2213, 0x2213, Type::ET},    {0x2214, 0x2335, Type::ON},
    {0x237B, 0x2394, Type::ON},    {0x2396, 0x2487, Type::ON},    {0x2488, 0x249B, Type::EN},
    {0x24EA, 0x26AB, Type::ON},    {0x26AD, 0x27FF, Type::ON},    {0x2900, 0x2BFF, Type::ON},
    {0x2E00, 0x2E7F, Type::ON},    {0x3000, 0x3000, Type::WS},    {0x3001, 0x3004, Type::ON},
    {0x3008, 0x3020, Type::ON},    {0xFB1D, 0xFB1D, Type::R},     {0xFB1E, 0xFB1E, Type::NSM},
    {0xFB1F, 0xFB28, Type::R},     {0xFB29, 0xFB29, Type::ES},    {0xFB2A, 0xFB4F, Type::R},
    {0xFB50, 0xFD3D, Type::AL},    {0xFD3E, 0xFD3F, Type::ON},    {0xFD40, 0xFDFF, Type::AL},
    {0xFE00, 0xFE0F, Type::NSM},   {0xFE10, 0xFE19, Type::ON},    {0xFE20, 0xFE2F, Type::NSM},
    {0xFE30, 0xFE4F, Type::ON},    {0xFE50, 0xFE50, Type::CS},    {0xFE51, 0xFE51, Type::ON},
    {0xFE52, 0xFE52, Type::CS},    {0xFE54, 0xFE54, Type::ON},    {0xFE55, 0xFE55, Type::CS},
    {0xFE56, 0xFE5E, Type::ON},    {0xFE5F, 0xFE5F, Type::ET},    {0xFE60, 0xFE61, Type::ON},
    {0xFE62, 0xFE63, Type::ES},    {0xFE64, 0xFE68, Type::ON},    {0xFE69, 0xFE6A, Type::ET},
    {0xFE6B, 0xFE6B, Type::ON},    {0xFE70, 0xFEFE, Type::AL},    {0xFF01, 0xFF02, Type::ON},
    {0xFF03, 0xFF05, Type::ET},    {0xFF06, 0xFF0A, Type::ON},    {0xFF0B, 0xFF0B, Type::ES},
    {0xFF0C, 0xFF0C, Type::CS},    {0xFF0D, 0xFF0D, Type::ES},    {0xFF0E, 0xFF0F, Type::CS},
    {0xFF10, 0xFF19, Type::EN},    {0xFF1A, 0xFF1A, Type::CS},    {0xFF1B, 0xFF20, Type::ON},
    {0xFF3B, 0xFF40, Type::ON},    {0xFF5B, 0xFF65, Type::ON},    {0x10800, 0x10FFF, Type::R},
    {0x1D167, 0x1D169, Type::NSM}, {0x1E800, 0x1EDFF, Type::R},   {0x1EE00, 0x1EEFF, Type::AL},
    {0x1F000, 0x1FAFF, Type::ON},  {0xE0100, 0xE01EF, Type::NSM},
};

bool strong(Type t) { return t == Type::L || t == Type::R || t == Type::AL; }
bool neutral(Type t) { return t == Type::B || t == Type::S || t == Type::WS || t == Type::ON; }
bool rtl(Type t) { return t == Type::R || t == Type::AL || t == Type::EN || t == Type::AN; } // for neutrals (N1)

} // namespace

Type type_of(u32 cp) {
    auto it = std::upper_bound(std::begin(ranges), std::end(ranges), cp, [](u32 c, const Range& r) { return c < r.first; });
    if (it == std::begin(ranges)) return Type::L;
    --it;
    return cp <= it->last ? it->type : Type::L;
}

Paragraph resolve(std::span<const u32> cps, Direction direction) {
    Paragraph p;
    usize n = cps.size();
    std::vector<Type> t(n);
    for (usize i = 0; i < n; ++i) t[i] = type_of(cps[i]);

    // P2, P3: the first strong letter sets the paragraph's direction.
    if (direction == Direction::RightToLeft) p.level = 1;
    else if (direction == Direction::Auto)
        for (Type x : t)
            if (strong(x)) {
                p.level = x == Type::L ? 0 : 1;
                break;
            }
    Type sos = p.level ? Type::R : Type::L;

    // W1: a mark takes the type of what it marks.
    for (usize i = 0; i < n; ++i)
        if (t[i] == Type::NSM) t[i] = i ? t[i - 1] : sos;
    // W2: European digits after Arabic letters are Arabic numbers. W3: Arabic letters are right-to-left.
    Type last_strong = sos;
    for (usize i = 0; i < n; ++i) {
        if (t[i] == Type::EN && last_strong == Type::AL) t[i] = Type::AN;
        if (strong(t[i])) last_strong = t[i];
    }
    for (Type& x : t)
        if (x == Type::AL) x = Type::R;
    // W4: one separator between two numbers of a kind joins them.
    for (usize i = 1; i + 1 < n; ++i) {
        if (t[i] == Type::ES && t[i - 1] == Type::EN && t[i + 1] == Type::EN) t[i] = Type::EN;
        else if (t[i] == Type::CS && t[i - 1] == t[i + 1] && (t[i - 1] == Type::EN || t[i - 1] == Type::AN)) t[i] = t[i - 1];
    }
    // W5: number terminators ($, %) next to European digits belong to them.
    for (usize i = 0; i < n;) {
        if (t[i] != Type::ET) {
            ++i;
            continue;
        }
        usize end = i;
        while (end < n && t[end] == Type::ET) ++end;
        bool by_number = (i > 0 && t[i - 1] == Type::EN) || (end < n && t[end] == Type::EN);
        if (by_number) std::fill(t.begin() + static_cast<std::ptrdiff_t>(i), t.begin() + static_cast<std::ptrdiff_t>(end), Type::EN);
        i = end;
    }
    // W6: separators and terminators left over are neutral. W7: European digits in left-to-right text are letters.
    for (Type& x : t)
        if (x == Type::ES || x == Type::ET || x == Type::CS) x = Type::ON;
    last_strong = sos;
    for (usize i = 0; i < n; ++i) {
        if (t[i] == Type::L || t[i] == Type::R) last_strong = t[i];
        if (t[i] == Type::EN && last_strong == Type::L) t[i] = Type::L;
    }
    // N1, N2: neutrals between two of the same direction take it; otherwise the paragraph's.
    for (usize i = 0; i < n;) {
        if (!neutral(t[i])) {
            ++i;
            continue;
        }
        usize end = i;
        while (end < n && neutral(t[end])) ++end;
        bool before = i == 0 ? p.level == 1 : rtl(t[i - 1]);
        bool after = end == n ? p.level == 1 : rtl(t[end]);
        Type fill = before == after ? (before ? Type::R : Type::L) : (p.level ? Type::R : Type::L);
        for (usize k = i; k < end; ++k)
            if (t[k] != Type::B && t[k] != Type::S) t[k] = fill;
        i = end;
    }
    // I1, I2: levels.
    p.levels.resize(n);
    for (usize i = 0; i < n; ++i) {
        u8 level = p.level;
        if (t[i] == Type::B || t[i] == Type::S) level = p.level;
        else if (p.level == 0) level = t[i] == Type::R ? 1 : (t[i] == Type::AN || t[i] == Type::EN) ? 2 : 0;
        else level = (t[i] == Type::L || t[i] == Type::EN || t[i] == Type::AN) ? 2 : 1;
        p.levels[i] = level;
    }
    return p;
}

std::vector<u32> visual_order(std::span<const u32> cps, std::span<const u8> levels_in, u8 paragraph_level) {
    usize n = levels_in.size();
    std::vector<u8> levels(levels_in.begin(), levels_in.end());
    // L1: whitespace at the end of the line, and before tabs, goes to the paragraph's level.
    auto is_space = [&](usize i) {
        Type x = type_of(cps[i]);
        return x == Type::WS || x == Type::S || x == Type::B;
    };
    for (usize i = n; i > 0 && is_space(i - 1); --i) levels[i - 1] = paragraph_level;
    for (usize i = 0; i < n; ++i)
        if (type_of(cps[i]) == Type::S)
            for (usize k = i + 1; k > 0 && is_space(k - 1); --k) levels[k - 1] = paragraph_level;

    std::vector<u32> order(n);
    for (usize i = 0; i < n; ++i) order[i] = static_cast<u32>(i);
    if (n == 0) return order;
    // L2: from the highest level down to the lowest odd one, reverse every run at that level or above.
    u8 highest = *std::max_element(levels.begin(), levels.end());
    u8 lowest_odd = 255;
    for (u8 l : levels)
        if (l % 2) lowest_odd = std::min(lowest_odd, l);
    if (lowest_odd == 255) return order;
    for (int level = highest; level >= lowest_odd; --level)
        for (usize i = 0; i < n;) {
            if (levels[order[i]] < level) {
                ++i;
                continue;
            }
            usize end = i;
            while (end < n && levels[order[end]] >= level) ++end;
            std::reverse(order.begin() + static_cast<std::ptrdiff_t>(i), order.begin() + static_cast<std::ptrdiff_t>(end));
            i = end;
        }
    return order;
}

u32 mirror(u32 cp) {
    static constexpr std::pair<u32, u32> pairs[] = {
        {'(', ')'}, {'<', '>'}, {'[', ']'}, {'{', '}'}, {0x00AB, 0x00BB}, {0x2039, 0x203A}, {0x2045, 0x2046},
        {0x207D, 0x207E}, {0x208D, 0x208E}, {0x2264, 0x2265}, {0x2329, 0x232A}, {0x27E8, 0x27E9}, {0x3008, 0x3009},
        {0x300A, 0x300B}, {0x300C, 0x300D}, {0x300E, 0x300F}, {0x3010, 0x3011}, {0xFF08, 0xFF09}, {0xFF3B, 0xFF3D},
    };
    for (auto [a, b] : pairs) {
        if (cp == a) return b;
        if (cp == b) return a;
    }
    return cp;
}

} // namespace ph::gfx::bidi
