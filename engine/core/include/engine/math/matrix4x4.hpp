#pragma once

#include "engine/defines.hpp"
#include "engine/math/quaternion.hpp"
#include "engine/math/scalar.hpp"
#include "engine/math/simd.hpp"
#include "engine/math/vector3.hpp"
#include "engine/math/vector4.hpp"

#include <cmath>

// 4x4 float matrix stored as four column Vector4s (column-major, like GLSL),
// acting on column vectors: `m * v`, and `a * b` applies `b` first. The
// memory layout is exactly what a GLSL mat4 uniform or push constant expects.
//
//     columns[c][r] is row r of column c; get(row, col) says the same thing.
//     Translation lives in columns[3].xyz.
//
// Conventions: right-handed world, +Y up, the camera looks down -Z. The
// projections produce Vulkan clip space: depth 0 at the near plane and 1 at
// the far plane, and clip-space Y negated so that world +Y ends up at the top
// of a Vulkan framebuffer (whose Y grows downward). Use them with a depth test
// of LESS and a depth clear of 1.0 (see render_target_clear_values).

struct alignas(16) Matrix4x4 {
    Vector4 columns[4];

    // The identity.
    Matrix4x4() : columns{Vector4::unit_x(), Vector4::unit_y(), Vector4::unit_z(), Vector4::unit_w()} {}
    Matrix4x4(const Vector4 c0, const Vector4 c1, const Vector4 c2, const Vector4 c3) : columns{c0, c1, c2, c3} {}

    static Matrix4x4 identity() { return Matrix4x4(); }
    static Matrix4x4 zero() { return Matrix4x4(Vector4(), Vector4(), Vector4(), Vector4()); }
    // 16 floats in column-major order, 16-byte aligned (a GLSL mat4).
    static Matrix4x4 load(const f32* m) {
        return Matrix4x4(Vector4::load(m), Vector4::load(m + 4), Vector4::load(m + 8), Vector4::load(m + 12));
    }
    void store(f32* m) const {
        this->columns[0].store(m);
        this->columns[1].store(m + 4);
        this->columns[2].store(m + 8);
        this->columns[3].store(m + 12);
    }

    // --- Affine builders --------------------------------------------------------

    static Matrix4x4 translation(const Vector3 t) {
        Matrix4x4 m;
        m.columns[3] = Vector4(t, 1.0f);
        return m;
    }
    static Matrix4x4 scaling(const Vector3 s) {
        return Matrix4x4(Vector4(s.x, 0.0f, 0.0f, 0.0f), Vector4(0.0f, s.y, 0.0f, 0.0f), Vector4(0.0f, 0.0f, s.z, 0.0f), Vector4::unit_w());
    }
    static Matrix4x4 scaling(const f32 s) { return scaling(Vector3(s)); }
    static Matrix4x4 rotation(const Quaternion q) {
        const f32 x = q.x(), y = q.y(), z = q.z(), w = q.w();
        const f32 xx = x * x, yy = y * y, zz = z * z;
        const f32 xy = x * y, xz = x * z, yz = y * z;
        const f32 wx = w * x, wy = w * y, wz = w * z;
        return Matrix4x4(
            Vector4(1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy), 0.0f),
            Vector4(2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx), 0.0f),
            Vector4(2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy), 0.0f),
            Vector4::unit_w());
    }
    static Matrix4x4 rotation_x(const f32 angle) {
        const f32 c = std::cos(angle), s = std::sin(angle);
        return Matrix4x4(Vector4::unit_x(), Vector4(0.0f, c, s, 0.0f), Vector4(0.0f, -s, c, 0.0f), Vector4::unit_w());
    }
    static Matrix4x4 rotation_y(const f32 angle) {
        const f32 c = std::cos(angle), s = std::sin(angle);
        return Matrix4x4(Vector4(c, 0.0f, -s, 0.0f), Vector4::unit_y(), Vector4(s, 0.0f, c, 0.0f), Vector4::unit_w());
    }
    static Matrix4x4 rotation_z(const f32 angle) {
        const f32 c = std::cos(angle), s = std::sin(angle);
        return Matrix4x4(Vector4(c, s, 0.0f, 0.0f), Vector4(-s, c, 0.0f, 0.0f), Vector4::unit_z(), Vector4::unit_w());
    }
    // translation(t) * rotation(r) * scaling(s): the usual object-to-world
    // matrix, built directly.
    static Matrix4x4 trs(const Vector3 t, const Quaternion r, const Vector3 s) {
        Matrix4x4 m = rotation(r);
        m.columns[0] *= s.x;
        m.columns[1] *= s.y;
        m.columns[2] *= s.z;
        m.columns[3] = Vector4(t, 1.0f);
        return m;
    }

    // --- Camera builders --------------------------------------------------------

    // World-to-view matrix for a camera at `eye` looking at `target`, with
    // `up` roughly up. The camera looks down its local -Z.
    static Matrix4x4 look_at(const Vector3 eye, const Vector3 target, const Vector3 up) {
        const Vector3 f = (target - eye).normalized();
        const Vector3 s = MATH::cross(f, up).normalized();
        const Vector3 u = MATH::cross(s, f);
        return Matrix4x4(
            Vector4(s.x, u.x, -f.x, 0.0f),
            Vector4(s.y, u.y, -f.y, 0.0f),
            Vector4(s.z, u.z, -f.z, 0.0f),
            Vector4(-MATH::dot(s, eye), -MATH::dot(u, eye), MATH::dot(f, eye), 1.0f));
    }
    // Perspective projection into Vulkan clip space (see the file comment).
    // `fov_y` is the vertical field of view in radians, `aspect` width over
    // height, `near` and `far` positive distances along -Z.
    static Matrix4x4 perspective(const f32 fov_y, const f32 aspect, const f32 near, const f32 far) {
        const f32 f = 1.0f / std::tan(fov_y * 0.5f);
        Matrix4x4 m = zero();
        m.columns[0].x = f / aspect;
        m.columns[1].y = -f;
        m.columns[2].z = far / (near - far);
        m.columns[2].w = -1.0f;
        m.columns[3].z = (near * far) / (near - far);
        return m;
    }
    // Orthographic projection into Vulkan clip space of the box
    // [left, right] x [bottom, top] x [-near, -far] in view space.
    static Matrix4x4 orthographic(const f32 left, const f32 right, const f32 bottom, const f32 top, const f32 near, const f32 far) {
        Matrix4x4 m = zero();
        m.columns[0].x = 2.0f / (right - left);
        m.columns[1].y = -2.0f / (top - bottom);
        m.columns[2].z = 1.0f / (near - far);
        m.columns[3].x = -(right + left) / (right - left);
        m.columns[3].y = (top + bottom) / (top - bottom);
        m.columns[3].z = near / (near - far);
        m.columns[3].w = 1.0f;
        return m;
    }

    // --- Element access ---------------------------------------------------------

    f32 get(const usz row, const usz col) const { return this->columns[col][row]; }
    void set(const usz row, const usz col, const f32 value) { this->columns[col][row] = value; }
    Vector4 row(const usz r) const {
        return Vector4(this->columns[0][r], this->columns[1][r], this->columns[2][r], this->columns[3][r]);
    }
    Vector3 translation_part() const { return this->columns[3].xyz(); }
    void set_translation(const Vector3 t) { this->columns[3] = Vector4(t, this->columns[3].w); }

    // --- Products ---------------------------------------------------------------

    Vector4 operator*(const Vector4 v) const {
        using namespace SIMD;
        f32x4 r = mul(this->columns[0].simd, splat_x(v.simd));
        r = fmadd(this->columns[1].simd, splat_y(v.simd), r);
        r = fmadd(this->columns[2].simd, splat_z(v.simd), r);
        r = fmadd(this->columns[3].simd, splat_w(v.simd), r);
        return Vector4(r);
    }
    Matrix4x4 operator*(const Matrix4x4& o) const {
        return Matrix4x4(*this * o.columns[0], *this * o.columns[1], *this * o.columns[2], *this * o.columns[3]);
    }
    Matrix4x4& operator*=(const Matrix4x4& o) { return *this = *this * o; }

    // Affine transform of a point (w = 1), ignoring any projective row.
    Vector3 transform_point(const Vector3 p) const {
        using namespace SIMD;
        f32x4 r = fmadd(this->columns[0].simd, splat_x(p.simd), this->columns[3].simd);
        r = fmadd(this->columns[1].simd, splat_y(p.simd), r);
        r = fmadd(this->columns[2].simd, splat_z(p.simd), r);
        return Vector3(r);
    }
    // Transform of a direction (w = 0): rotation and scale only.
    Vector3 transform_direction(const Vector3 d) const {
        using namespace SIMD;
        f32x4 r = mul(this->columns[0].simd, splat_x(d.simd));
        r = fmadd(this->columns[1].simd, splat_y(d.simd), r);
        r = fmadd(this->columns[2].simd, splat_z(d.simd), r);
        return Vector3(r);
    }
    // Full projective transform of a point with the perspective divide: what
    // the GPU does to a vertex. Returns normalized device coordinates.
    Vector3 project_point(const Vector3 p) const {
        const Vector4 clip = *this * Vector4(p, 1.0f);
        return Vector3(SIMD::div(clip.simd, SIMD::splat_w(clip.simd)));
    }

    // --- Derived matrices -------------------------------------------------------

    Matrix4x4 transposed() const {
        using namespace SIMD;
        const f32x4 c0 = this->columns[0].simd, c1 = this->columns[1].simd;
        const f32x4 c2 = this->columns[2].simd, c3 = this->columns[3].simd;
        const f32x4 t0 = shuffle<0, 1, 0, 1>(c0, c1); // c0.x c0.y c1.x c1.y
        const f32x4 t1 = shuffle<2, 3, 2, 3>(c0, c1); // c0.z c0.w c1.z c1.w
        const f32x4 t2 = shuffle<0, 1, 0, 1>(c2, c3);
        const f32x4 t3 = shuffle<2, 3, 2, 3>(c2, c3);
        return Matrix4x4(
            Vector4(shuffle<0, 2, 0, 2>(t0, t2)),
            Vector4(shuffle<1, 3, 1, 3>(t0, t2)),
            Vector4(shuffle<0, 2, 0, 2>(t1, t3)),
            Vector4(shuffle<1, 3, 1, 3>(t1, t3)));
    }

    f32 determinant() const;
    // General inverse by cofactors. A singular matrix returns zero(); check
    // determinant() first if that matters. Prefer inverse_affine() for
    // transforms with no projective row.
    Matrix4x4 inverse() const;
    // Inverse of a matrix whose last row is (0, 0, 0, 1): any product of
    // translation, rotation and scaling. Cheaper and more accurate than
    // inverse(); the result is undefined for other matrices.
    Matrix4x4 inverse_affine() const;

    bool operator==(const Matrix4x4& o) const {
        return this->columns[0] == o.columns[0] && this->columns[1] == o.columns[1]
            && this->columns[2] == o.columns[2] && this->columns[3] == o.columns[3];
    }
    bool operator!=(const Matrix4x4& o) const { return !(*this == o); }
};

static_assert(sizeof(Matrix4x4) == 64 && alignof(Matrix4x4) == 16, "Matrix4x4 is four SIMD registers, GLSL mat4 layout");

namespace MATH {

inline bool approx_equal(const Matrix4x4& a, const Matrix4x4& b, const f32 epsilon = EPSILON) {
    return approx_equal(a.columns[0], b.columns[0], epsilon) && approx_equal(a.columns[1], b.columns[1], epsilon)
        && approx_equal(a.columns[2], b.columns[2], epsilon) && approx_equal(a.columns[3], b.columns[3], epsilon);
}

} // namespace MATH

// --- Inverse ----------------------------------------------------------------------

inline Matrix4x4 Matrix4x4::inverse_affine() const {
    // Inverse of [R | t] is [R^-1 | -R^-1 t] with R the upper 3x3.
    const f32 a00 = this->get(0, 0), a01 = this->get(0, 1), a02 = this->get(0, 2);
    const f32 a10 = this->get(1, 0), a11 = this->get(1, 1), a12 = this->get(1, 2);
    const f32 a20 = this->get(2, 0), a21 = this->get(2, 1), a22 = this->get(2, 2);

    const f32 c00 = a11 * a22 - a12 * a21;
    const f32 c01 = a12 * a20 - a10 * a22;
    const f32 c02 = a10 * a21 - a11 * a20;
    const f32 det = a00 * c00 + a01 * c01 + a02 * c02;
    if (det == 0.0f) {
        return zero();
    }
    const f32 inv_det = 1.0f / det;

    // inverse(r, c) = cofactor(c, r) / det: column c of the inverse holds the
    // cofactors of row c.
    Matrix4x4 r;
    r.columns[0] = Vector4(c00, c01, c02, 0.0f) * inv_det;
    r.columns[1] = Vector4(a02 * a21 - a01 * a22, a00 * a22 - a02 * a20, a01 * a20 - a00 * a21, 0.0f) * inv_det;
    r.columns[2] = Vector4(a01 * a12 - a02 * a11, a02 * a10 - a00 * a12, a00 * a11 - a01 * a10, 0.0f) * inv_det;
    const Vector3 t = -r.transform_direction(this->translation_part());
    r.columns[3] = Vector4(t, 1.0f);
    return r;
}

inline f32 Matrix4x4::determinant() const {
    alignas(16) f32 m[16];
    this->store(m);
    // Cofactor expansion along the first row, column-major indexing m[col*4+row].
    const f32 s0 = m[10] * m[15] - m[14] * m[11];
    const f32 s1 = m[9] * m[15] - m[13] * m[11];
    const f32 s2 = m[9] * m[14] - m[13] * m[10];
    const f32 s3 = m[8] * m[15] - m[12] * m[11];
    const f32 s4 = m[8] * m[14] - m[12] * m[10];
    const f32 s5 = m[8] * m[13] - m[12] * m[9];
    const f32 c0 = m[5] * s0 - m[6] * s1 + m[7] * s2;
    const f32 c1 = -(m[4] * s0 - m[6] * s3 + m[7] * s4);
    const f32 c2 = m[4] * s1 - m[5] * s3 + m[7] * s5;
    const f32 c3 = -(m[4] * s2 - m[5] * s4 + m[6] * s5);
    return m[0] * c0 + m[1] * c1 + m[2] * c2 + m[3] * c3;
}

inline Matrix4x4 Matrix4x4::inverse() const {
    alignas(16) f32 m[16];
    alignas(16) f32 inv[16];
    this->store(m);

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    const f32 det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f) {
        return zero();
    }
    const Vector4 inv_det(1.0f / det);
    return Matrix4x4(Vector4::load(inv) * inv_det, Vector4::load(inv + 4) * inv_det, Vector4::load(inv + 8) * inv_det, Vector4::load(inv + 12) * inv_det);
}
