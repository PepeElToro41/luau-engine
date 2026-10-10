#pragma once

#include "engine/defines.hpp"
#include "engine/math/scalar.hpp"
#include "engine/math/simd.hpp"
#include "engine/math/vector3.hpp"
#include "engine/math/vector4.hpp"

#include <cmath>

// A rotation as a unit quaternion (x, y, z, w) with w the scalar part, stored
// as a Vector4 so the component-wise work reuses its SIMD operators. Hamilton
// product: `a * b` rotates by `b` first, then by `a`, matching matrix
// composition. Angles are radians; rotation follows the right-hand rule
// (counter-clockwise looking down the axis towards the origin).
//
// Every constructor and operation except operator+/-/scaling keeps the
// quaternion normalized if its inputs are; call normalized() after
// accumulating many multiplications.

struct Quaternion {
    Vector4 v;

    // Identity rotation.
    Quaternion() : v(0.0f, 0.0f, 0.0f, 1.0f) {}
    Quaternion(const f32 x, const f32 y, const f32 z, const f32 w) : v(x, y, z, w) {}
    explicit Quaternion(const Vector4 v) : v(v) {}

    f32 x() const { return this->v.x; }
    f32 y() const { return this->v.y; }
    f32 z() const { return this->v.z; }
    f32 w() const { return this->v.w; }
    // The vector part (x, y, z).
    Vector3 xyz() const { return this->v.xyz(); }

    static Quaternion identity() { return Quaternion(); }

    // Rotation of `angle` radians about unit vector `axis`.
    static Quaternion from_axis_angle(const Vector3 axis, const f32 angle) {
        const f32 half = angle * 0.5f;
        return Quaternion(Vector4(axis * std::sin(half), std::cos(half)));
    }
    static Quaternion rotation_x(const f32 angle) { return from_axis_angle(Vector3::unit_x(), angle); }
    static Quaternion rotation_y(const f32 angle) { return from_axis_angle(Vector3::unit_y(), angle); }
    static Quaternion rotation_z(const f32 angle) { return from_axis_angle(Vector3::unit_z(), angle); }
    // Euler angles applied as roll about Z, then pitch about X, then yaw
    // about Y: yaw * pitch * roll.
    static Quaternion from_euler(const f32 pitch, const f32 yaw, const f32 roll) {
        return rotation_y(yaw) * rotation_x(pitch) * rotation_z(roll);
    }
    // The Euler angles (pitch, yaw, roll) that from_euler() turns back into
    // this unit quaternion: pitch in [-PI/2, PI/2], yaw and roll in [-PI, PI].
    // At the gimbal lock (pitch of +-90 degrees) yaw and roll are not
    // separable, so roll is reported as 0 and yaw carries the whole turn.
    Vector3 to_euler() const {
        const f32 x = this->x(), y = this->y(), z = this->z(), w = this->w();
        // Rows of the rotation matrix yaw * pitch * roll builds; see the
        // derivation in the test file.
        const f32 m00 = 1.0f - 2.0f * (y * y + z * z);
        const f32 m01 = 2.0f * (x * y - w * z);
        const f32 m02 = 2.0f * (x * z + w * y);
        const f32 m10 = 2.0f * (x * y + w * z);
        const f32 m11 = 1.0f - 2.0f * (x * x + z * z);
        const f32 m12 = 2.0f * (y * z - w * x);
        const f32 m22 = 1.0f - 2.0f * (x * x + y * y);
        // m12 = -sin(pitch)
        const f32 sin_pitch = MATH::clamp(-m12, -1.0f, 1.0f);
        if (std::fabs(sin_pitch) > 1.0f - 1e-6f) {
            // cos(pitch) = 0: m00 = cos(yaw -+ roll), m01 = +-sin(yaw -+ roll).
            const f32 pitch = sin_pitch > 0.0f ? MATH::PI * 0.5f : -MATH::PI * 0.5f;
            const f32 yaw = std::atan2(sin_pitch > 0.0f ? m01 : -m01, m00);
            return Vector3(pitch, yaw, 0.0f);
        }
        // m02 = sin(yaw) cos(pitch), m22 = cos(yaw) cos(pitch),
        // m10 = cos(pitch) sin(roll), m11 = cos(pitch) cos(roll).
        return Vector3(std::asin(sin_pitch), std::atan2(m02, m22), std::atan2(m10, m11));
    }
    // The shortest rotation taking unit vector `from` onto unit vector `to`.
    // Opposite vectors rotate 180 degrees about an arbitrary perpendicular.
    static Quaternion from_to(const Vector3 from, const Vector3 to) {
        const f32 d = MATH::dot(from, to);
        if (d < -1.0f + MATH::EPSILON) {
            Vector3 axis = MATH::cross(Vector3::unit_x(), from);
            if (axis.length_squared() < MATH::EPSILON) {
                axis = MATH::cross(Vector3::unit_y(), from);
            }
            return from_axis_angle(axis.normalized(), MATH::PI);
        }
        const Vector3 c = MATH::cross(from, to);
        return Quaternion(Vector4(c, 1.0f + d)).normalized();
    }

    f32 length_squared() const { return this->v.length_squared(); }
    f32 length() const { return this->v.length(); }
    // Unit-length copy; a zero quaternion becomes the identity.
    Quaternion normalized() const {
        const f32 len_sq = this->length_squared();
        return len_sq > 0.0f ? Quaternion(this->v / std::sqrt(len_sq)) : Quaternion();
    }
    // The reverse rotation of a unit quaternion.
    Quaternion conjugate() const { return Quaternion(this->v * Vector4(-1.0f, -1.0f, -1.0f, 1.0f)); }
    // The reverse rotation of any non-zero quaternion.
    Quaternion inverse() const { return Quaternion(this->conjugate().v / this->length_squared()); }

    // Hamilton product, see the file comment for the order.
    Quaternion operator*(const Quaternion o) const {
        using namespace SIMD;
        const f32x4 a = this->v.simd;
        const f32x4 b = o.v.simd;
        // Per output lane (x, y, z, w):
        //   w1 * (x2, y2, z2, w2)
        // + x1 * ( w2, -z2,  y2, -x2)
        // + y1 * ( z2,  w2, -x2, -y2)
        // + z1 * (-y2,  x2,  w2, -z2)
        f32x4 r = mul(splat_w(a), b);
        r = fmadd(splat_x(a), mul(swizzle<3, 2, 1, 0>(b), make(1.0f, -1.0f, 1.0f, -1.0f)), r);
        r = fmadd(splat_y(a), mul(swizzle<2, 3, 0, 1>(b), make(1.0f, 1.0f, -1.0f, -1.0f)), r);
        r = fmadd(splat_z(a), mul(swizzle<1, 0, 3, 2>(b), make(-1.0f, 1.0f, 1.0f, -1.0f)), r);
        return Quaternion(Vector4(r));
    }
    Quaternion& operator*=(const Quaternion o) { return *this = *this * o; }

    // Rotates `p` by this unit quaternion.
    Vector3 rotate(const Vector3 p) const {
        // p' = p + w * t + q_xyz x t, with t = 2 * (q_xyz x p)
        const SIMD::f32x4 q = this->v.simd;
        const SIMD::f32x4 t = SIMD::mul(SIMD::cross3(q, p.simd), SIMD::splat(2.0f));
        const SIMD::f32x4 r = SIMD::add(SIMD::fmadd(SIMD::splat_w(q), t, p.simd), SIMD::cross3(q, t));
        return Vector3(r);
    }
    Vector3 operator*(const Vector3 p) const { return this->rotate(p); }

    // Exact comparison of the four components. Note q and -q are the same
    // rotation but compare unequal.
    bool operator==(const Quaternion o) const { return this->v == o.v; }
    bool operator!=(const Quaternion o) const { return !(*this == o); }
};

static_assert(sizeof(Quaternion) == 16 && alignof(Quaternion) == 16, "Quaternion is one SIMD register");

namespace MATH {

inline f32 dot(const Quaternion a, const Quaternion b) { return dot(a.v, b.v); }

// Linear blend renormalized: cheap, not constant angular speed. Takes the
// short way round.
inline Quaternion nlerp(const Quaternion a, Quaternion b, const f32 t) {
    if (dot(a, b) < 0.0f) {
        b = Quaternion(-b.v);
    }
    return Quaternion(lerp(a.v, b.v, t)).normalized();
}

// Spherical interpolation at constant angular speed, the short way round.
inline Quaternion slerp(const Quaternion a, Quaternion b, const f32 t) {
    f32 cos_theta = dot(a, b);
    if (cos_theta < 0.0f) {
        b = Quaternion(-b.v);
        cos_theta = -cos_theta;
    }
    // Nearly parallel: the sine below would be unstable, and nlerp is exact
    // enough.
    if (cos_theta > 0.9995f) {
        return Quaternion(lerp(a.v, b.v, t)).normalized();
    }
    const f32 theta = std::acos(cos_theta);
    const f32 sin_theta = std::sin(theta);
    const f32 wa = std::sin((1.0f - t) * theta) / sin_theta;
    const f32 wb = std::sin(t * theta) / sin_theta;
    return Quaternion(a.v * wa + b.v * wb);
}

// Whether the two represent the same rotation within `epsilon` (q and -q
// count as equal).
inline bool approx_equal(const Quaternion a, const Quaternion b, const f32 epsilon = EPSILON) {
    return approx_equal(a.v, b.v, epsilon) || approx_equal(a.v, -b.v, epsilon);
}

// Angle in radians between two unit quaternions, in [0, PI].
inline f32 angle(const Quaternion a, const Quaternion b) {
    const f32 d = clamp(std::fabs(dot(a, b)), 0.0f, 1.0f);
    return 2.0f * std::acos(d);
}

} // namespace MATH
