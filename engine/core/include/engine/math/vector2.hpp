#pragma once

#include "engine/defines.hpp"
#include "engine/math/scalar.hpp"

#include <cmath>

// Two floats, scalar code only: there is nothing for SIMD to gain at this
// width, and 8 bytes with 4-byte alignment packs into UVs and screen
// positions. Binary operations that are not operators live in MATH::.

struct Vector2 {
    f32 x = 0.0f;
    f32 y = 0.0f;

    constexpr Vector2() = default;
    constexpr Vector2(const f32 x, const f32 y) : x(x), y(y) {}
    constexpr explicit Vector2(const f32 s) : x(s), y(s) {}

    static constexpr Vector2 zero() { return Vector2(0.0f, 0.0f); }
    static constexpr Vector2 one() { return Vector2(1.0f, 1.0f); }
    static constexpr Vector2 unit_x() { return Vector2(1.0f, 0.0f); }
    static constexpr Vector2 unit_y() { return Vector2(0.0f, 1.0f); }

    constexpr f32& operator[](const usz i) { return (&this->x)[i]; }
    constexpr f32 operator[](const usz i) const { return (&this->x)[i]; }

    constexpr f32 length_squared() const { return this->x * this->x + this->y * this->y; }
    f32 length() const { return std::sqrt(this->length_squared()); }
    // Unit-length copy; the zero vector stays zero.
    Vector2 normalized() const {
        const f32 len = this->length();
        return len > 0.0f ? Vector2(this->x / len, this->y / len) : Vector2();
    }
    // Rotated 90 degrees counter-clockwise.
    constexpr Vector2 perpendicular() const { return Vector2(-this->y, this->x); }

    constexpr Vector2 operator-() const { return Vector2(-this->x, -this->y); }
    constexpr Vector2 operator+(const Vector2 o) const { return Vector2(this->x + o.x, this->y + o.y); }
    constexpr Vector2 operator-(const Vector2 o) const { return Vector2(this->x - o.x, this->y - o.y); }
    constexpr Vector2 operator*(const Vector2 o) const { return Vector2(this->x * o.x, this->y * o.y); }
    constexpr Vector2 operator/(const Vector2 o) const { return Vector2(this->x / o.x, this->y / o.y); }
    constexpr Vector2 operator*(const f32 s) const { return Vector2(this->x * s, this->y * s); }
    constexpr Vector2 operator/(const f32 s) const { return Vector2(this->x / s, this->y / s); }

    constexpr Vector2& operator+=(const Vector2 o) { return *this = *this + o; }
    constexpr Vector2& operator-=(const Vector2 o) { return *this = *this - o; }
    constexpr Vector2& operator*=(const Vector2 o) { return *this = *this * o; }
    constexpr Vector2& operator/=(const Vector2 o) { return *this = *this / o; }
    constexpr Vector2& operator*=(const f32 s) { return *this = *this * s; }
    constexpr Vector2& operator/=(const f32 s) { return *this = *this / s; }

    constexpr bool operator==(const Vector2 o) const { return this->x == o.x && this->y == o.y; }
    constexpr bool operator!=(const Vector2 o) const { return !(*this == o); }
};

constexpr Vector2 operator*(const f32 s, const Vector2 v) { return v * s; }

namespace MATH {

constexpr f32 dot(const Vector2 a, const Vector2 b) { return a.x * b.x + a.y * b.y; }
// The z component of the 3D cross product of (a, 0) and (b, 0): positive when
// b is counter-clockwise from a.
constexpr f32 cross(const Vector2 a, const Vector2 b) { return a.x * b.y - a.y * b.x; }
constexpr Vector2 lerp(const Vector2 a, const Vector2 b, const f32 t) { return a + (b - a) * t; }
constexpr Vector2 min(const Vector2 a, const Vector2 b) { return Vector2(min(a.x, b.x), min(a.y, b.y)); }
constexpr Vector2 max(const Vector2 a, const Vector2 b) { return Vector2(max(a.x, b.x), max(a.y, b.y)); }
inline Vector2 abs(const Vector2 v) { return Vector2(std::fabs(v.x), std::fabs(v.y)); }
inline f32 distance(const Vector2 a, const Vector2 b) { return (b - a).length(); }
inline bool approx_equal(const Vector2 a, const Vector2 b, const f32 epsilon = EPSILON) {
    return approx_equal(a.x, b.x, epsilon) && approx_equal(a.y, b.y, epsilon);
}

} // namespace MATH
