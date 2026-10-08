#include "support/test_support.hpp"

#include "engine/math/matrix4x4.hpp"

#include <cmath>

// Scalar reference product, column-major: r(row, col) = sum_k a(row, k) * b(k, col).
static Matrix4x4 reference_multiply(const Matrix4x4& a, const Matrix4x4& b) {
    Matrix4x4 r = Matrix4x4::zero();
    for (usz row = 0; row < 4; ++row) {
        for (usz col = 0; col < 4; ++col) {
            f32 sum = 0.0f;
            for (usz k = 0; k < 4; ++k) {
                sum += a.get(row, k) * b.get(k, col);
            }
            r.set(row, col, sum);
        }
    }
    return r;
}

static Matrix4x4 sample_matrix() {
    return Matrix4x4(
        Vector4(1.0f, 2.0f, 3.0f, 4.0f),
        Vector4(5.0f, 6.0f, 7.0f, 8.0f),
        Vector4(-1.0f, 0.5f, 2.0f, -3.0f),
        Vector4(0.25f, -2.0f, 1.0f, 1.5f));
}

TEST_CASE("math/matrix4x4: identity, element access and layout") {
    const Matrix4x4 i;
    CHECK(i == Matrix4x4::identity());
    for (usz r = 0; r < 4; ++r) {
        for (usz c = 0; c < 4; ++c) {
            CHECK(i.get(r, c) == (r == c ? 1.0f : 0.0f));
        }
    }
    Matrix4x4 m = sample_matrix();
    // columns[c][r] is row r of column c.
    CHECK(m.get(0, 1) == 5.0f);
    CHECK(m.get(3, 0) == 4.0f);
    CHECK(m.columns[2].y == 0.5f);
    m.set(3, 0, 9.0f);
    CHECK(m.columns[0].w == 9.0f);
    CHECK(m.row(1) == Vector4(2.0f, 6.0f, 0.5f, -2.0f));
    CHECK(m.translation_part() == Vector3(0.25f, -2.0f, 1.0f));
    m.set_translation(Vector3(7.0f, 8.0f, 9.0f));
    CHECK(m.columns[3] == Vector4(7.0f, 8.0f, 9.0f, 1.5f));
    static_assert(sizeof(Matrix4x4) == 64 && alignof(Matrix4x4) == 16);
}

TEST_CASE("math/matrix4x4: load and store are column-major GLSL layout") {
    alignas(16) f32 data[16];
    for (usz k = 0; k < 16; ++k) {
        data[k] = static_cast<f32>(k);
    }
    const Matrix4x4 m = Matrix4x4::load(data);
    CHECK(m.columns[0] == Vector4(0.0f, 1.0f, 2.0f, 3.0f));
    CHECK(m.columns[3] == Vector4(12.0f, 13.0f, 14.0f, 15.0f));
    CHECK(m.get(2, 1) == 6.0f);
    alignas(16) f32 out[16] = {};
    m.store(out);
    for (usz k = 0; k < 16; ++k) {
        CHECK(out[k] == data[k]);
    }
}

TEST_CASE("math/matrix4x4: multiply matches the scalar reference") {
    const Matrix4x4 a = sample_matrix();
    const Matrix4x4 b = Matrix4x4::trs(Vector3(1.0f, 2.0f, 3.0f), Quaternion::rotation_y(0.4f), Vector3(2.0f, 1.0f, 0.5f));
    CHECK(MATH::approx_equal(a * b, reference_multiply(a, b), 1e-4f));
    CHECK(MATH::approx_equal(b * a, reference_multiply(b, a), 1e-4f));
    CHECK(MATH::approx_equal(a * Matrix4x4(), a));
    CHECK(MATH::approx_equal(Matrix4x4() * a, a));
    Matrix4x4 c = a;
    c *= b;
    CHECK(MATH::approx_equal(c, a * b));
}

TEST_CASE("math/matrix4x4: matrix times vector") {
    const Matrix4x4 m = sample_matrix();
    const Vector4 v(1.0f, 2.0f, 3.0f, 4.0f);
    Vector4 expected;
    for (usz r = 0; r < 4; ++r) {
        f32 sum = 0.0f;
        for (usz k = 0; k < 4; ++k) {
            sum += m.get(r, k) * v[k];
        }
        expected[r] = sum;
    }
    CHECK(MATH::approx_equal(m * v, expected));
    CHECK(Matrix4x4() * v == v);
}

TEST_CASE("math/matrix4x4: affine builders") {
    const Vector3 p(1.0f, 2.0f, 3.0f);
    CHECK(Matrix4x4::translation(Vector3(10.0f, 20.0f, 30.0f)).transform_point(p) == Vector3(11.0f, 22.0f, 33.0f));
    CHECK(Matrix4x4::translation(Vector3(10.0f, 20.0f, 30.0f)).transform_direction(p) == p);
    CHECK(Matrix4x4::scaling(Vector3(2.0f, 3.0f, 4.0f)).transform_point(p) == Vector3(2.0f, 6.0f, 12.0f));
    CHECK(Matrix4x4::scaling(2.0f).transform_direction(p) == p * 2.0f);

    // Axis rotations agree with the quaternion ones and are right-handed.
    CHECK(MATH::approx_equal(Matrix4x4::rotation_x(0.7f), Matrix4x4::rotation(Quaternion::rotation_x(0.7f))));
    CHECK(MATH::approx_equal(Matrix4x4::rotation_y(0.7f), Matrix4x4::rotation(Quaternion::rotation_y(0.7f))));
    CHECK(MATH::approx_equal(Matrix4x4::rotation_z(0.7f), Matrix4x4::rotation(Quaternion::rotation_z(0.7f))));
    CHECK(MATH::approx_equal(Matrix4x4::rotation_y(MATH::HALF_PI).transform_direction(Vector3::unit_x()), Vector3(0.0f, 0.0f, -1.0f)));
    CHECK(MATH::approx_equal(Matrix4x4::rotation_z(MATH::HALF_PI).transform_direction(Vector3::unit_x()), Vector3::unit_y()));

    // rotation(q) agrees with q.rotate.
    const Quaternion q = Quaternion::from_axis_angle(Vector3(0.2f, -0.4f, 0.9f).normalized(), 2.3f);
    CHECK(MATH::approx_equal(Matrix4x4::rotation(q).transform_direction(p), q.rotate(p)));
    CHECK(MATH::approx_equal(Matrix4x4::rotation(q).transform_point(p), q.rotate(p)));
    CHECK(Matrix4x4::rotation(q).row(3) == Vector4::unit_w());

    // trs is translation * rotation * scaling.
    const Vector3 t(5.0f, -1.0f, 2.0f);
    const Vector3 s(2.0f, 3.0f, 0.5f);
    const Matrix4x4 trs = Matrix4x4::trs(t, q, s);
    const Matrix4x4 composed = Matrix4x4::translation(t) * Matrix4x4::rotation(q) * Matrix4x4::scaling(s);
    CHECK(MATH::approx_equal(trs, composed));
    CHECK(MATH::approx_equal(trs.transform_point(p), q.rotate(p * s) + t));
}

TEST_CASE("math/matrix4x4: transpose") {
    const Matrix4x4 m = sample_matrix();
    const Matrix4x4 t = m.transposed();
    for (usz r = 0; r < 4; ++r) {
        for (usz c = 0; c < 4; ++c) {
            CHECK(t.get(r, c) == m.get(c, r));
        }
    }
    CHECK(t.transposed() == m);
    // The transpose of a rotation is its inverse.
    const Matrix4x4 rot = Matrix4x4::rotation(Quaternion::rotation_x(1.1f));
    CHECK(MATH::approx_equal(rot * rot.transposed(), Matrix4x4()));
}

TEST_CASE("math/matrix4x4: determinant and inverse") {
    CHECK(Matrix4x4().determinant() == 1.0f);
    CHECK(Matrix4x4::scaling(Vector3(2.0f, 3.0f, 4.0f)).determinant() == doctest::Approx(24.0f));
    CHECK(Matrix4x4::rotation(Quaternion::rotation_y(0.3f)).determinant() == doctest::Approx(1.0f));

    const Matrix4x4 m = sample_matrix();
    CHECK(m.determinant() != 0.0f);
    const Matrix4x4 inv = m.inverse();
    CHECK(MATH::approx_equal(m * inv, Matrix4x4(), 1e-4f));
    CHECK(MATH::approx_equal(inv * m, Matrix4x4(), 1e-4f));
    CHECK(inv.determinant() == doctest::Approx(1.0f / m.determinant()).epsilon(1e-3));

    // A singular matrix inverts to zero instead of NaNs.
    Matrix4x4 singular = m;
    singular.columns[1] = singular.columns[0];
    CHECK(singular.determinant() == doctest::Approx(0.0f));
    CHECK(singular.inverse() == Matrix4x4::zero());
    CHECK(Matrix4x4::scaling(Vector3(1.0f, 0.0f, 1.0f)).inverse_affine() == Matrix4x4::zero());
}

TEST_CASE("math/matrix4x4: inverse_affine agrees with inverse on transforms") {
    const Matrix4x4 trs = Matrix4x4::trs(Vector3(3.0f, -2.0f, 7.0f), Quaternion::from_euler(0.3f, 1.2f, -0.5f), Vector3(2.0f, 0.5f, 1.5f));
    const Matrix4x4 fast = trs.inverse_affine();
    CHECK(MATH::approx_equal(fast, trs.inverse(), 1e-4f));
    CHECK(MATH::approx_equal(trs * fast, Matrix4x4(), 1e-4f));
    CHECK(fast.row(3) == Vector4::unit_w());
    const Vector3 p(1.0f, 2.0f, 3.0f);
    CHECK(MATH::approx_equal(fast.transform_point(trs.transform_point(p)), p, 1e-4f));
    // Pure translation inverts to the opposite translation.
    CHECK(Matrix4x4::translation(Vector3(1.0f, 2.0f, 3.0f)).inverse_affine() == Matrix4x4::translation(Vector3(-1.0f, -2.0f, -3.0f)));
}

TEST_CASE("math/matrix4x4: look_at builds a right-handed view") {
    const Vector3 eye(0.0f, 2.0f, 5.0f);
    const Vector3 target(0.0f, 2.0f, 0.0f);
    const Matrix4x4 view = Matrix4x4::look_at(eye, target, Vector3::up());
    // The eye goes to the origin, the target ends up straight ahead down -Z.
    CHECK(MATH::approx_equal(view.transform_point(eye), Vector3()));
    CHECK(MATH::approx_equal(view.transform_point(target), Vector3(0.0f, 0.0f, -5.0f)));
    // World +X stays +X (camera right) and world up stays up.
    CHECK(MATH::approx_equal(view.transform_direction(Vector3::unit_x()), Vector3::unit_x()));
    CHECK(MATH::approx_equal(view.transform_direction(Vector3::up()), Vector3::up()));
    // A view matrix is rigid: its affine inverse is the camera's world transform.
    const Matrix4x4 camera = view.inverse_affine();
    CHECK(MATH::approx_equal(camera.translation_part(), eye));
    CHECK(MATH::approx_equal(camera.transform_direction(Vector3::forward()), (target - eye).normalized()));

    // Looking along a diagonal still maps the target onto -Z.
    const Vector3 eye2(3.0f, 1.0f, -4.0f);
    const Vector3 target2(-1.0f, 0.5f, 2.0f);
    const Matrix4x4 view2 = Matrix4x4::look_at(eye2, target2, Vector3::up());
    const Vector3 t = view2.transform_point(target2);
    CHECK(t.x == doctest::Approx(0.0f));
    CHECK(t.y == doctest::Approx(0.0f));
    CHECK(t.z == doctest::Approx(-(target2 - eye2).length()));
}

TEST_CASE("math/matrix4x4: perspective maps to Vulkan clip space") {
    const f32 near = 0.5f, far = 100.0f;
    const Matrix4x4 proj = Matrix4x4::perspective(MATH::radians(90.0f), 2.0f, near, far);
    // Points on the view axis land at NDC x = y = 0, near at depth 0, far at depth 1.
    const Vector3 at_near = proj.project_point(Vector3(0.0f, 0.0f, -near));
    const Vector3 at_far = proj.project_point(Vector3(0.0f, 0.0f, -far));
    CHECK(at_near.x == doctest::Approx(0.0f));
    CHECK(at_near.y == doctest::Approx(0.0f));
    CHECK(at_near.z == doctest::Approx(0.0f));
    CHECK(at_far.z == doctest::Approx(1.0f));
    // Depth grows monotonically in between.
    const f32 mid = proj.project_point(Vector3(0.0f, 0.0f, -10.0f)).z;
    CHECK(mid > 0.0f);
    CHECK(mid < 1.0f);
    // With a 90 degree vertical FOV, a point at y = depth sits on the top edge,
    // and Vulkan's Y-down means world up is NDC y = -1.
    const Vector3 top = proj.project_point(Vector3(0.0f, 10.0f, -10.0f));
    CHECK(top.y == doctest::Approx(-1.0f));
    const Vector3 bottom = proj.project_point(Vector3(0.0f, -10.0f, -10.0f));
    CHECK(bottom.y == doctest::Approx(1.0f));
    // Aspect 2: the right edge is at x = 2 * depth. World right is NDC +x.
    const Vector3 right = proj.project_point(Vector3(20.0f, 0.0f, -10.0f));
    CHECK(right.x == doctest::Approx(1.0f));
    // Clip w is the view-space distance.
    const Vector4 clip = proj * Vector4(1.0f, 2.0f, -7.0f, 1.0f);
    CHECK(clip.w == doctest::Approx(7.0f));
}

TEST_CASE("math/matrix4x4: orthographic maps the box to Vulkan clip space") {
    const Matrix4x4 ortho = Matrix4x4::orthographic(-2.0f, 6.0f, -1.0f, 3.0f, 1.0f, 11.0f);
    // Corners of the box: (left, bottom, -near) and (right, top, -far).
    CHECK(MATH::approx_equal(ortho.project_point(Vector3(-2.0f, -1.0f, -1.0f)), Vector3(-1.0f, 1.0f, 0.0f)));
    CHECK(MATH::approx_equal(ortho.project_point(Vector3(6.0f, 3.0f, -11.0f)), Vector3(1.0f, -1.0f, 1.0f)));
    // The center of the box goes to the center of clip space, depth 0.5.
    CHECK(MATH::approx_equal(ortho.project_point(Vector3(2.0f, 1.0f, -6.0f)), Vector3(0.0f, 0.0f, 0.5f)));
    // No perspective: w stays 1.
    CHECK((ortho * Vector4(1.0f, 1.0f, -3.0f, 1.0f)).w == 1.0f);
}

TEST_CASE("math/matrix4x4: model-view-projection round trip") {
    const Matrix4x4 model = Matrix4x4::trs(Vector3(1.0f, 0.0f, -3.0f), Quaternion::rotation_y(0.5f), Vector3(1.0f));
    const Matrix4x4 view = Matrix4x4::look_at(Vector3(0.0f, 1.0f, 4.0f), Vector3(1.0f, 0.0f, -3.0f), Vector3::up());
    const Matrix4x4 proj = Matrix4x4::perspective(MATH::radians(60.0f), 16.0f / 9.0f, 0.1f, 50.0f);
    const Matrix4x4 mvp = proj * view * model;
    // The object's origin is what the camera looks at: it projects to the center.
    const Vector3 ndc = mvp.project_point(Vector3());
    CHECK(ndc.x == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(ndc.y == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(ndc.z > 0.0f);
    CHECK(ndc.z < 1.0f);
    // Composition order: the same as applying the stages one at a time.
    const Vector3 p(0.3f, -0.2f, 0.1f);
    const Vector3 stepwise = proj.project_point(view.transform_point(model.transform_point(p)));
    CHECK(MATH::approx_equal(mvp.project_point(p), stepwise, 1e-4f));
}
