#pragma once

#include "bowl/core/types.hpp"

#include <span>

namespace bowl::gfx {

// Big-endian reads from font data that give 0 past the end, with `ok` cleared, so broken fonts fail softly.
struct FontReader {
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

} // namespace bowl::gfx
