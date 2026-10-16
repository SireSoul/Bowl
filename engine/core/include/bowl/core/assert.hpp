#pragma once

// BOWL_ASSERT(cond[, "message"]): checked in debug and dev builds, compiled out when BOWL_ASSERTS is 0.
// BOWL_VERIFY(cond[, "message"]): like BOWL_ASSERT, but cond is evaluated even when asserts are off.

#ifndef BOWL_ASSERTS
#if !defined(NDEBUG) || BOWL_DEV_TOOLS
#define BOWL_ASSERTS 1
#else
#define BOWL_ASSERTS 0
#endif
#endif

namespace bowl::detail {
[[noreturn]] void assert_fail(const char* condition, const char* file, int line, const char* message = nullptr);
}

#if BOWL_ASSERTS
#define BOWL_ASSERT(cond, ...)                                                                                         \
    do {                                                                                                               \
        if (!(cond)) [[unlikely]]                                                                                      \
            ::bowl::detail::assert_fail(#cond, __FILE__, __LINE__ __VA_OPT__(, ) __VA_ARGS__);                         \
    } while (0)
#define BOWL_VERIFY(cond, ...) BOWL_ASSERT(cond __VA_OPT__(, ) __VA_ARGS__)
#else
#define BOWL_ASSERT(cond, ...) ((void)0)
#define BOWL_VERIFY(cond, ...) ((void)(cond))
#endif

#define BOWL_UNREACHABLE() ::bowl::detail::assert_fail("unreachable", __FILE__, __LINE__)
