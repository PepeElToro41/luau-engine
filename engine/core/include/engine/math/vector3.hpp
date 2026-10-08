#pragma once

#include "engine/defines.hpp"
#include "engine/math/scalar.hpp"
#include "engine/math/simd.hpp"

// Three floats in one SIMD register, so 16 bytes and 16-byte aligned. The
// fourth lane `w` is padding: constructors zero it, arithmetic may leave
// anything there (including NaN after a component-wise division), and no
// operation reads it. Comparisons, dot, cross and lengths look at xyz only.
//
// The 12-byte layout used by vertex streams and asset files is reached with
// load() / store(); nothing else about this type knows about it.
//
// Conventions: right-handed, +Y up, forward is -Z.

struct alignas(16) Vector3 {
    union {
        SIMD::f32x4 simd;
        struct {
            f32 x;
            f32 y;
            f32 z;
            f32 w; // padding, never read
        };
    };

    Vector3() : simd(SIMD::zero()) {}
    Vector3(const f32 x, const f32 y, const f32 z) : simd(SIMD::make(x, y, z, 0.0f)) {}
    explicit Vector3(const f32 s) : simd(SIMD::make(s, s, s, 0.0f)) {}
    explicit Vector3(const SIMD::f32x4 v) : simd(v) {}

    static Vector3 zero() { return Vector3(); }
    static Vector3 one() { return Vector3(1.0f); }
    static Vector3 unit_x() { return Vector3(1.0f, 0.0f, 0.0f); }
    static Vector3 unit_y() { return Vector3(0.0f, 1.0f, 0.0f); }
    static Vector3 unit_z() { return Vector3(0.0f, 0.0f, 1.0f); }
    static Vector3 right() { return unit_x(); }
    static Vector3 up() { return unit_y(); }
    static Vector3 forward() { return Vector3(0.0f, 0.0f, -1.0f); }

    // Three floats at any alignment, as stored in vertex streams.
    static Vector3 load(const f32* xyz) { return Vector3(SIMD::load3(xyz)); }
    void store(f32* xyz) const { SIMD::store3(xyz, this->simd); }

    f32& operator[](const usz i) { return (&this->x)[i]; }
    f32 operator[](const usz i) const { return (&this->x)[i]; }

    f32 length_squared() const { return SIMD::dot3(this->simd, this->simd); }
    f32 length() const { return std::sqrt(this->length_squared()); }
    // Unit-length copy; the zero vector stays zero.
    Vector3 normalized() const {
        const f32 len_sq = this->length_squared();
        if (len_sq <= 0.0f) {
            return Vector3();
        }
        return Vector3(SIMD::div(this->simd, SIMD::sqrt(SIMD::splat(len_sq))));
    }

    Vector3 operator-() const { return Vector3(SIMD::neg(this->simd)); }
    Vector3 operator+(const Vector3 o) const { return Vector3(SIMD::add(this->simd, o.simd)); }
    Vector3 operator-(const Vector3 o) const { return Vector3(SIMD::sub(this->simd, o.simd)); }
    Vector3 operator*(const Vector3 o) const { return Vector3(SIMD::mul(this->simd, o.simd)); }
    Vector3 operator/(const Vector3 o) const { return Vector3(SIMD::div(this->simd, o.simd)); }
    Vector3 operator*(const f32 s) const { return Vector3(SIMD::mul(this->simd, SIMD::splat(s))); }
    Vector3 operator/(const f32 s) const { return Vector3(SIMD::div(this->simd, SIMD::splat(s))); }

    Vector3& operator+=(const Vector3 o) { return *this = *this + o; }
    Vector3& operator-=(const Vector3 o) { return *this = *this - o; }
    Vector3& operator*=(const Vector3 o) { return *this = *this * o; }
    Vector3& operator/=(const Vector3 o) { return *this = *this / o; }
    Vector3& operator*=(const f32 s) { return *this = *this * s; }
    Vector3& operator/=(const f32 s) { return *this = *this / s; }

    // Exact comparison of xyz.
    bool operator==(const Vector3 o) const { return (SIMD::mask_eq(this->simd, o.simd) & 0x7) == 0x7; }
    bool operator!=(const Vector3 o) const { return !(*this == o); }
};

static_assert(sizeof(Vector3) == 16 && alignof(Vector3) == 16, "Vector3 is one SIMD register");

inline Vector3 operator*(const f32 s, const Vector3 v) { return v * s; }

namespace MATH {

inline f32 dot(const Vector3 a, const Vector3 b) { return SIMD::dot3(a.simd, b.simd); }
inline Vector3 cross(const Vector3 a, const Vector3 b) { return Vector3(SIMD::cross3(a.simd, b.simd)); }
inline Vector3 lerp(const Vector3 a, const Vector3 b, const f32 t) { return a + (b - a) * t; }
inline Vector3 min(const Vector3 a, const Vector3 b) { return Vector3(SIMD::min(a.simd, b.simd)); }
inline Vector3 max(const Vector3 a, const Vector3 b) { return Vector3(SIMD::max(a.simd, b.simd)); }
inline Vector3 abs(const Vector3 v) { return Vector3(SIMD::abs(v.simd)); }
inline f32 distance(const Vector3 a, const Vector3 b) { return (b - a).length(); }
// Every xyz component within `epsilon`.
inline bool approx_equal(const Vector3 a, const Vector3 b, const f32 epsilon = EPSILON) {
    const SIMD::f32x4 diff = SIMD::abs(SIMD::sub(a.simd, b.simd));
    return (SIMD::mask_le(diff, SIMD::splat(epsilon)) & 0x7) == 0x7;
}
// `v` with its component along unit vector `n` removed.
inline Vector3 project_onto_plane(const Vector3 v, const Vector3 n) { return v - n * dot(v, n); }
// Mirror of `v` about the plane with unit normal `n`.
inline Vector3 reflect(const Vector3 v, const Vector3 n) { return v - n * (2.0f * dot(v, n)); }

} // namespace MATH
