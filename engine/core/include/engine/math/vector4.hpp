#pragma once

#include "engine/defines.hpp"
#include "engine/math/scalar.hpp"
#include "engine/math/simd.hpp"
#include "engine/math/vector3.hpp"

// Four floats in one SIMD register: homogeneous positions, colors, and the
// columns of Matrix4x4. Every lane takes part in every operation.

struct alignas(16) Vector4 {
    union {
        SIMD::f32x4 simd;
        struct {
            f32 x;
            f32 y;
            f32 z;
            f32 w;
        };
    };

    Vector4() : simd(SIMD::zero()) {}
    Vector4(const f32 x, const f32 y, const f32 z, const f32 w) : simd(SIMD::make(x, y, z, w)) {}
    explicit Vector4(const f32 s) : simd(SIMD::splat(s)) {}
    explicit Vector4(const SIMD::f32x4 v) : simd(v) {}
    // xyz from `v`, w from `w`.
    Vector4(const Vector3 v, const f32 w) : simd(SIMD::make(v.x, v.y, v.z, w)) {}

    static Vector4 zero() { return Vector4(); }
    static Vector4 one() { return Vector4(1.0f); }
    static Vector4 unit_x() { return Vector4(1.0f, 0.0f, 0.0f, 0.0f); }
    static Vector4 unit_y() { return Vector4(0.0f, 1.0f, 0.0f, 0.0f); }
    static Vector4 unit_z() { return Vector4(0.0f, 0.0f, 1.0f, 0.0f); }
    static Vector4 unit_w() { return Vector4(0.0f, 0.0f, 0.0f, 1.0f); }

    // Four floats, 16-byte aligned.
    static Vector4 load(const f32* xyzw) { return Vector4(SIMD::load(xyzw)); }
    void store(f32* xyzw) const { SIMD::store(xyzw, this->simd); }

    // The xyz lanes; w becomes padding.
    Vector3 xyz() const { return Vector3(this->simd); }

    f32& operator[](const usz i) { return (&this->x)[i]; }
    f32 operator[](const usz i) const { return (&this->x)[i]; }

    f32 length_squared() const { return SIMD::dot4(this->simd, this->simd); }
    f32 length() const { return std::sqrt(this->length_squared()); }
    // Unit-length copy; the zero vector stays zero.
    Vector4 normalized() const {
        const f32 len_sq = this->length_squared();
        if (len_sq <= 0.0f) {
            return Vector4();
        }
        return Vector4(SIMD::div(this->simd, SIMD::sqrt(SIMD::splat(len_sq))));
    }

    Vector4 operator-() const { return Vector4(SIMD::neg(this->simd)); }
    Vector4 operator+(const Vector4 o) const { return Vector4(SIMD::add(this->simd, o.simd)); }
    Vector4 operator-(const Vector4 o) const { return Vector4(SIMD::sub(this->simd, o.simd)); }
    Vector4 operator*(const Vector4 o) const { return Vector4(SIMD::mul(this->simd, o.simd)); }
    Vector4 operator/(const Vector4 o) const { return Vector4(SIMD::div(this->simd, o.simd)); }
    Vector4 operator*(const f32 s) const { return Vector4(SIMD::mul(this->simd, SIMD::splat(s))); }
    Vector4 operator/(const f32 s) const { return Vector4(SIMD::div(this->simd, SIMD::splat(s))); }

    Vector4& operator+=(const Vector4 o) { return *this = *this + o; }
    Vector4& operator-=(const Vector4 o) { return *this = *this - o; }
    Vector4& operator*=(const Vector4 o) { return *this = *this * o; }
    Vector4& operator/=(const Vector4 o) { return *this = *this / o; }
    Vector4& operator*=(const f32 s) { return *this = *this * s; }
    Vector4& operator/=(const f32 s) { return *this = *this / s; }

    bool operator==(const Vector4 o) const { return SIMD::mask_eq(this->simd, o.simd) == 0xF; }
    bool operator!=(const Vector4 o) const { return !(*this == o); }
};

static_assert(sizeof(Vector4) == 16 && alignof(Vector4) == 16, "Vector4 is one SIMD register");

inline Vector4 operator*(const f32 s, const Vector4 v) { return v * s; }

namespace MATH {

inline f32 dot(const Vector4 a, const Vector4 b) { return SIMD::dot4(a.simd, b.simd); }
inline Vector4 lerp(const Vector4 a, const Vector4 b, const f32 t) { return a + (b - a) * t; }
inline Vector4 min(const Vector4 a, const Vector4 b) { return Vector4(SIMD::min(a.simd, b.simd)); }
inline Vector4 max(const Vector4 a, const Vector4 b) { return Vector4(SIMD::max(a.simd, b.simd)); }
inline Vector4 abs(const Vector4 v) { return Vector4(SIMD::abs(v.simd)); }
inline bool approx_equal(const Vector4 a, const Vector4 b, const f32 epsilon = EPSILON) {
    const SIMD::f32x4 diff = SIMD::abs(SIMD::sub(a.simd, b.simd));
    return SIMD::mask_le(diff, SIMD::splat(epsilon)) == 0xF;
}

} // namespace MATH
