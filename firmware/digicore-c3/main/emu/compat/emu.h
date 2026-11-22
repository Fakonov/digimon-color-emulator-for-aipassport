// Minimal MAME shim so the original MAME unSP CPU sources in ref/unsp/*.cpp
// can be compiled unmodified for the PC-side reference emulator.
// Only the types and helpers those five .cpp files actually touch exist here.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef uint32_t offs_t;

#ifndef BIT
#define BIT(x, n) (((x) >> (n)) & 1)
#endif

#define ATTR_COLD

// logging: silent by default, enable with UNSP_VERBOSE_LOG
#ifdef UNSP_VERBOSE_LOG
#define logerror(...) do { std::printf(__VA_ARGS__); } while (0)
#else
#define logerror(...) do { } while (0)
#endif

#define LOGMASKED(mask, ...) do { } while (0)

#define LOG_GENERAL   (1U << 0)
#define LOG_UNSP_MULS (1U << 1)

namespace util {
template <typename... Args> inline void stream_format(std::ostream &s, const char *fmt, Args... args) {}
}
