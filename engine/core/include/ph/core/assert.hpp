#pragma once

// PH_ASSERT(cond[, "message"]) — checked in debug and dev builds, compiled out when PH_ASSERTS is 0.
// PH_VERIFY(cond[, "message"]) — always evaluates cond; only checks when asserts are on.

#ifndef PH_ASSERTS
#if !defined(NDEBUG) || PH_DEV_TOOLS
#define PH_ASSERTS 1
#else
#define PH_ASSERTS 0
#endif
#endif

namespace ph::detail {
[[noreturn]] void assert_fail(const char* condition, const char* file, int line, const char* message = nullptr);
}

#if PH_ASSERTS
#define PH_ASSERT(cond, ...)                                                                                          \
    do {                                                                                                              \
        if (!(cond)) [[unlikely]]                                                                                     \
            ::ph::detail::assert_fail(#cond, __FILE__, __LINE__ __VA_OPT__(, ) __VA_ARGS__);                          \
    } while (0)
#define PH_VERIFY(cond, ...) PH_ASSERT(cond __VA_OPT__(, ) __VA_ARGS__)
#else
#define PH_ASSERT(cond, ...) ((void)0)
#define PH_VERIFY(cond, ...) ((void)(cond))
#endif

#define PH_UNREACHABLE() ::ph::detail::assert_fail("unreachable", __FILE__, __LINE__)
