#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ReSharper disable CppInconsistentNaming
using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;

using i8 = char;
using i16 = short;
using i32 = int;
using i64 = long long;

using usz = unsigned long long;
using f32 = float;
using f64 = double;

#if defined(__clang__) || defined(__gcc__)
#define STATIC_ASSERT _Static_assert
#else
#define STATIC_ASSERT static_assert
#endif

STATIC_ASSERT(sizeof(u8) == 1, "Expected u8 to be 1 byte.");
STATIC_ASSERT(sizeof(u16) == 2, "Expected u16 to be 2 bytes.");
STATIC_ASSERT(sizeof(u32) == 4, "Expected u32 to be 4 bytes.");
STATIC_ASSERT(sizeof(u64) == 8, "Expected u64 to be 8 bytes.");

STATIC_ASSERT(sizeof(i8) == 1, "Expected i8 to be 1 byte.");
STATIC_ASSERT(sizeof(i16) == 2, "Expected i16 to be 2 bytes.");
STATIC_ASSERT(sizeof(i32) == 4, "Expected i32 to be 4 bytes.");
STATIC_ASSERT(sizeof(i64) == 8, "Expected i64 to be 8 bytes.");

STATIC_ASSERT(sizeof(f32) == 4, "Expected f32 to be 4 bytes.");
STATIC_ASSERT(sizeof(f64) == 8, "Expected f64 to be 8 bytes.");

// --- Debug assertions --------------------------------------------------------
// ENGINE_ASSERT(condition, format, ...) checks `condition` in debug builds and
// compiles to nothing when NDEBUG is defined (CMake's Release and MinSizeRel
// configurations), so it is free to use on hot paths: the condition is not
// evaluated at all in release. On failure it prints the location, the
// condition and the formatted message to stderr and aborts. Use it for caller
// mistakes that release builds either tolerate or cannot afford to check;
// errors that must be reported in release print explicitly instead.
#ifndef NDEBUG
#define ENGINE_DEBUG 1
#else
#define ENGINE_DEBUG 0
#endif

namespace ENGINE_ASSERTS {

[[noreturn]] inline void fail(const char* condition, const char* file, const int line, const char* format, ...) {
    fprintf(stderr, "%s:%d: assertion failed: %s\n  ", file, line, condition);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    abort();
}

} // namespace ENGINE_ASSERTS

#if ENGINE_DEBUG
#define ENGINE_ASSERT(condition, ...)                                                    \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            ENGINE_ASSERTS::fail(#condition, __FILE__, __LINE__, __VA_ARGS__);           \
        }                                                                                \
    } while (0)
#else
#define ENGINE_ASSERT(condition, ...) ((void)0)
#endif
