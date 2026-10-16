#pragma once

#include <cstddef>
#include <cstdint>

namespace bowl {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;
using isize = std::ptrdiff_t;

// Non-copyable, non-movable base for objects with identity (devices, worlds, windows).
class NonCopyable {
public:
    NonCopyable() = default;
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
    NonCopyable(NonCopyable&&) = delete;
    NonCopyable& operator=(NonCopyable&&) = delete;

protected:
    ~NonCopyable() = default;
};

#define BOWL_CONCAT_IMPL(a, b) a##b
#define BOWL_CONCAT(a, b) BOWL_CONCAT_IMPL(a, b)

} // namespace bowl
