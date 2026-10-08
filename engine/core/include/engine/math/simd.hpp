#pragma once

#include "engine/defines.hpp"

#include <cmath>

// Four packed floats and the handful of operations the math types are built
// from. On x86-64 this is SSE2, which every x86-64 CPU has, so no compiler
// flags are needed. Anywhere else (or with ENGINE_MATH_FORCE_SCALAR defined,
// useful to test both paths) it is a plain struct of four floats with the same
// API, so Vector3/Vector4/Quaternion/Matrix4x4 compile unchanged.
//
// Lanes are named x, y, z, w = index 0, 1, 2, 3. Nothing here knows about
// vectors or matrices; it is the register layer only.

#if !defined(ENGINE_MATH_FORCE_SCALAR) && (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#define ENGINE_SIMD_SSE 1
#include <emmintrin.h>
#else
#define ENGINE_SIMD_SSE 0
#endif

namespace SIMD {

#if ENGINE_SIMD_SSE
using f32x4 = __m128;
#else
struct alignas(16) f32x4 {
    f32 lane[4];
};
#endif

// --- Construction ----------------------------------------------------------

inline f32x4 make(const f32 x, const f32 y, const f32 z, const f32 w) {
#if ENGINE_SIMD_SSE
    return _mm_set_ps(w, z, y, x);
#else
    return f32x4{{x, y, z, w}};
#endif
}

inline f32x4 splat(const f32 s) {
#if ENGINE_SIMD_SSE
    return _mm_set1_ps(s);
#else
    return f32x4{{s, s, s, s}};
#endif
}

inline f32x4 zero() {
#if ENGINE_SIMD_SSE
    return _mm_setzero_ps();
#else
    return f32x4{{0, 0, 0, 0}};
#endif
}

// `p` must be 16-byte aligned.
inline f32x4 load(const f32* p) {
#if ENGINE_SIMD_SSE
    return _mm_load_ps(p);
#else
    return f32x4{{p[0], p[1], p[2], p[3]}};
#endif
}

// Three floats, any alignment; w becomes 0.
inline f32x4 load3(const f32* p) {
#if ENGINE_SIMD_SSE
    return _mm_set_ps(0.0f, p[2], p[1], p[0]);
#else
    return f32x4{{p[0], p[1], p[2], 0.0f}};
#endif
}

// `p` must be 16-byte aligned.
inline void store(f32* p, const f32x4 v) {
#if ENGINE_SIMD_SSE
    _mm_store_ps(p, v);
#else
    p[0] = v.lane[0];
    p[1] = v.lane[1];
    p[2] = v.lane[2];
    p[3] = v.lane[3];
#endif
}

// Three floats, any alignment; w is not written.
inline void store3(f32* p, const f32x4 v) {
#if ENGINE_SIMD_SSE
    alignas(16) f32 tmp[4];
    _mm_store_ps(tmp, v);
    p[0] = tmp[0];
    p[1] = tmp[1];
    p[2] = tmp[2];
#else
    p[0] = v.lane[0];
    p[1] = v.lane[1];
    p[2] = v.lane[2];
#endif
}

// --- Lane access -------------------------------------------------------------

template <int I>
inline f32 get(const f32x4 v) {
    static_assert(I >= 0 && I < 4, "lane index out of range");
#if ENGINE_SIMD_SSE
    if constexpr (I == 0) {
        return _mm_cvtss_f32(v);
    } else {
        return _mm_cvtss_f32(_mm_shuffle_ps(v, v, _MM_SHUFFLE(I, I, I, I)));
    }
#else
    return v.lane[I];
#endif
}

inline f32 get_x(const f32x4 v) { return get<0>(v); }
inline f32 get_y(const f32x4 v) { return get<1>(v); }
inline f32 get_z(const f32x4 v) { return get<2>(v); }
inline f32 get_w(const f32x4 v) { return get<3>(v); }

// Lanes of `v` rearranged: result = (v[X], v[Y], v[Z], v[W]).
template <int X, int Y, int Z, int W>
inline f32x4 swizzle(const f32x4 v) {
    static_assert(X >= 0 && X < 4 && Y >= 0 && Y < 4 && Z >= 0 && Z < 4 && W >= 0 && W < 4, "lane index out of range");
#if ENGINE_SIMD_SSE
    return _mm_shuffle_ps(v, v, _MM_SHUFFLE(W, Z, Y, X));
#else
    return f32x4{{v.lane[X], v.lane[Y], v.lane[Z], v.lane[W]}};
#endif
}

// Two lanes from `a` then two from `b`: result = (a[X], a[Y], b[Z], b[W]).
template <int X, int Y, int Z, int W>
inline f32x4 shuffle(const f32x4 a, const f32x4 b) {
    static_assert(X >= 0 && X < 4 && Y >= 0 && Y < 4 && Z >= 0 && Z < 4 && W >= 0 && W < 4, "lane index out of range");
#if ENGINE_SIMD_SSE
    return _mm_shuffle_ps(a, b, _MM_SHUFFLE(W, Z, Y, X));
#else
    return f32x4{{a.lane[X], a.lane[Y], b.lane[Z], b.lane[W]}};
#endif
}

inline f32x4 splat_x(const f32x4 v) { return swizzle<0, 0, 0, 0>(v); }
inline f32x4 splat_y(const f32x4 v) { return swizzle<1, 1, 1, 1>(v); }
inline f32x4 splat_z(const f32x4 v) { return swizzle<2, 2, 2, 2>(v); }
inline f32x4 splat_w(const f32x4 v) { return swizzle<3, 3, 3, 3>(v); }

// --- Arithmetic ------------------------------------------------------------------

inline f32x4 add(const f32x4 a, const f32x4 b) {
#if ENGINE_SIMD_SSE
    return _mm_add_ps(a, b);
#else
    return f32x4{{a.lane[0] + b.lane[0], a.lane[1] + b.lane[1], a.lane[2] + b.lane[2], a.lane[3] + b.lane[3]}};
#endif
}

inline f32x4 sub(const f32x4 a, const f32x4 b) {
#if ENGINE_SIMD_SSE
    return _mm_sub_ps(a, b);
#else
    return f32x4{{a.lane[0] - b.lane[0], a.lane[1] - b.lane[1], a.lane[2] - b.lane[2], a.lane[3] - b.lane[3]}};
#endif
}

inline f32x4 mul(const f32x4 a, const f32x4 b) {
#if ENGINE_SIMD_SSE
    return _mm_mul_ps(a, b);
#else
    return f32x4{{a.lane[0] * b.lane[0], a.lane[1] * b.lane[1], a.lane[2] * b.lane[2], a.lane[3] * b.lane[3]}};
#endif
}

inline f32x4 div(const f32x4 a, const f32x4 b) {
#if ENGINE_SIMD_SSE
    return _mm_div_ps(a, b);
#else
    return f32x4{{a.lane[0] / b.lane[0], a.lane[1] / b.lane[1], a.lane[2] / b.lane[2], a.lane[3] / b.lane[3]}};
#endif
}

// a * b + c
inline f32x4 fmadd(const f32x4 a, const f32x4 b, const f32x4 c) {
    return add(mul(a, b), c);
}

inline f32x4 neg(const f32x4 v) {
    return sub(zero(), v);
}

inline f32x4 min(const f32x4 a, const f32x4 b) {
#if ENGINE_SIMD_SSE
    return _mm_min_ps(a, b);
#else
    return f32x4{{a.lane[0] < b.lane[0] ? a.lane[0] : b.lane[0], a.lane[1] < b.lane[1] ? a.lane[1] : b.lane[1],
        a.lane[2] < b.lane[2] ? a.lane[2] : b.lane[2], a.lane[3] < b.lane[3] ? a.lane[3] : b.lane[3]}};
#endif
}

inline f32x4 max(const f32x4 a, const f32x4 b) {
#if ENGINE_SIMD_SSE
    return _mm_max_ps(a, b);
#else
    return f32x4{{a.lane[0] > b.lane[0] ? a.lane[0] : b.lane[0], a.lane[1] > b.lane[1] ? a.lane[1] : b.lane[1],
        a.lane[2] > b.lane[2] ? a.lane[2] : b.lane[2], a.lane[3] > b.lane[3] ? a.lane[3] : b.lane[3]}};
#endif
}

inline f32x4 abs(const f32x4 v) {
#if ENGINE_SIMD_SSE
    // Clear the sign bit.
    return _mm_andnot_ps(_mm_set1_ps(-0.0f), v);
#else
    return f32x4{{std::fabs(v.lane[0]), std::fabs(v.lane[1]), std::fabs(v.lane[2]), std::fabs(v.lane[3])}};
#endif
}

inline f32x4 sqrt(const f32x4 v) {
#if ENGINE_SIMD_SSE
    return _mm_sqrt_ps(v);
#else
    return f32x4{{std::sqrt(v.lane[0]), std::sqrt(v.lane[1]), std::sqrt(v.lane[2]), std::sqrt(v.lane[3])}};
#endif
}

// --- Horizontal ------------------------------------------------------------------

// x*x' + y*y' + z*z', ignoring w, in every lane.
inline f32x4 dot3_splat(const f32x4 a, const f32x4 b) {
    const f32x4 m = mul(a, b);
    const f32x4 s = add(splat_x(m), add(splat_y(m), splat_z(m)));
    return s;
}

inline f32 dot3(const f32x4 a, const f32x4 b) {
    return get_x(dot3_splat(a, b));
}

// Four-lane dot product in every lane.
inline f32x4 dot4_splat(const f32x4 a, const f32x4 b) {
    const f32x4 m = mul(a, b);
    // (x+y, x+y, z+w, z+w) then (x+y+z+w) everywhere.
    const f32x4 pairs = add(m, swizzle<1, 0, 3, 2>(m));
    return add(pairs, swizzle<2, 3, 0, 1>(pairs));
}

inline f32 dot4(const f32x4 a, const f32x4 b) {
    return get_x(dot4_splat(a, b));
}

// Cross product of the xyz lanes; w is 0 when both inputs have w = 0, and
// otherwise unspecified.
inline f32x4 cross3(const f32x4 a, const f32x4 b) {
    // (a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x)
    const f32x4 a_yzx = swizzle<1, 2, 0, 3>(a);
    const f32x4 b_yzx = swizzle<1, 2, 0, 3>(b);
    const f32x4 c = sub(mul(a, b_yzx), mul(a_yzx, b));
    return swizzle<1, 2, 0, 3>(c);
}

// --- Comparison ------------------------------------------------------------------

// Bit i of the result is set when lane i of `a` equals lane i of `b`.
inline u32 mask_eq(const f32x4 a, const f32x4 b) {
#if ENGINE_SIMD_SSE
    return static_cast<u32>(_mm_movemask_ps(_mm_cmpeq_ps(a, b)));
#else
    return (a.lane[0] == b.lane[0] ? 1u : 0u) | (a.lane[1] == b.lane[1] ? 2u : 0u) | (a.lane[2] == b.lane[2] ? 4u : 0u) | (a.lane[3] == b.lane[3] ? 8u : 0u);
#endif
}

// Bit i of the result is set when lane i of `a` is <= lane i of `b`.
inline u32 mask_le(const f32x4 a, const f32x4 b) {
#if ENGINE_SIMD_SSE
    return static_cast<u32>(_mm_movemask_ps(_mm_cmple_ps(a, b)));
#else
    return (a.lane[0] <= b.lane[0] ? 1u : 0u) | (a.lane[1] <= b.lane[1] ? 2u : 0u) | (a.lane[2] <= b.lane[2] ? 4u : 0u) | (a.lane[3] <= b.lane[3] ? 8u : 0u);
#endif
}

} // namespace SIMD
