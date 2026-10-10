#include "support/test_support.hpp"

#include "engine/math/matrix4x4.hpp"
#include "engine/math/quaternion.hpp"

#include <cmath>

// Scalar Hamilton product to check the SIMD one against.
static Quaternion reference_multiply(const Quaternion a, const Quaternion b) {
    const f32 x1 = a.x(), y1 = a.y(), z1 = a.z(), w1 = a.w();
    const f32 x2 = b.x(), y2 = b.y(), z2 = b.z(), w2 = b.w();
    return Quaternion(
        w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
        w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
        w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2,
        w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2);
}

TEST_CASE("math/quaternion: identity") {
    const Quaternion q;
    CHECK(q == Quaternion(0.0f, 0.0f, 0.0f, 1.0f));
    CHECK(q == Quaternion::identity());
    CHECK(q.length() == 1.0f);
    const Vector3 p(1.0f, 2.0f, 3.0f);
    CHECK(q.rotate(p) == p);
    CHECK(q * p == p);
    CHECK(q.xyz() == Vector3());
    CHECK(q.w() == 1.0f);
    static_assert(sizeof(Quaternion) == 16 && alignof(Quaternion) == 16);
}

TEST_CASE("math/quaternion: axis-angle rotates right-handed") {
    const Quaternion ry = Quaternion::rotation_y(MATH::HALF_PI);
    CHECK(ry.length() == doctest::Approx(1.0f));
    // 90 degrees about +Y takes +X to -Z and -Z to -X.
    CHECK(MATH::approx_equal(ry.rotate(Vector3::unit_x()), Vector3(0.0f, 0.0f, -1.0f)));
    CHECK(MATH::approx_equal(ry.rotate(Vector3::forward()), Vector3(-1.0f, 0.0f, 0.0f)));
    CHECK(MATH::approx_equal(ry.rotate(Vector3::unit_y()), Vector3::unit_y()));
    // 90 degrees about +Z takes +X to +Y; about +X takes +Y to +Z.
    CHECK(MATH::approx_equal(Quaternion::rotation_z(MATH::HALF_PI).rotate(Vector3::unit_x()), Vector3::unit_y()));
    CHECK(MATH::approx_equal(Quaternion::rotation_x(MATH::HALF_PI).rotate(Vector3::unit_y()), Vector3::unit_z()));
    // Any axis, any angle: rotation preserves length and the angle to the axis.
    const Vector3 axis = Vector3(1.0f, 2.0f, 3.0f).normalized();
    const Quaternion q = Quaternion::from_axis_angle(axis, 1.234f);
    const Vector3 p(-4.0f, 0.5f, 2.0f);
    const Vector3 r = q.rotate(p);
    CHECK(r.length() == doctest::Approx(p.length()));
    CHECK(MATH::dot(r, axis) == doctest::Approx(MATH::dot(p, axis)));
    CHECK(MATH::approx_equal(q.rotate(axis), axis));
}

TEST_CASE("math/quaternion: multiply matches the scalar Hamilton product") {
    const Quaternion a = Quaternion::from_axis_angle(Vector3(0.3f, -0.7f, 0.2f).normalized(), 0.9f);
    const Quaternion b = Quaternion::from_axis_angle(Vector3(-0.5f, 0.1f, 0.8f).normalized(), 2.1f);
    CHECK(MATH::approx_equal((a * b).v, reference_multiply(a, b).v));
    CHECK(MATH::approx_equal((b * a).v, reference_multiply(b, a).v));
    // Not commutative in general.
    CHECK_FALSE(MATH::approx_equal((a * b).v, (b * a).v));
    // Identity is neutral on both sides.
    CHECK(MATH::approx_equal(a * Quaternion(), a));
    CHECK(MATH::approx_equal(Quaternion() * a, a));
    // Unit inputs stay unit.
    CHECK((a * b).length() == doctest::Approx(1.0f));
}

TEST_CASE("math/quaternion: composition order is right to left") {
    const Quaternion first = Quaternion::rotation_x(MATH::HALF_PI);
    const Quaternion then = Quaternion::rotation_y(MATH::HALF_PI);
    const Vector3 p(0.0f, 1.0f, 0.0f);
    const Vector3 step_by_step = then.rotate(first.rotate(p));
    CHECK(MATH::approx_equal((then * first).rotate(p), step_by_step));
    // X 90: +Y -> +Z, then Y 90: +Z -> +X.
    CHECK(MATH::approx_equal(step_by_step, Vector3::unit_x()));
    // Same as composing the matrices.
    const Matrix4x4 m = Matrix4x4::rotation(then) * Matrix4x4::rotation(first);
    CHECK(MATH::approx_equal(m.transform_direction(p), step_by_step));
    CHECK(MATH::approx_equal(Matrix4x4::rotation(then * first), m));
}

TEST_CASE("math/quaternion: conjugate and inverse undo the rotation") {
    const Quaternion q = Quaternion::from_axis_angle(Vector3(1.0f, 1.0f, 0.0f).normalized(), 0.75f);
    const Vector3 p(2.0f, -3.0f, 5.0f);
    CHECK(MATH::approx_equal(q.conjugate().rotate(q.rotate(p)), p));
    CHECK(MATH::approx_equal((q * q.conjugate()), Quaternion()));
    CHECK(MATH::approx_equal((q.inverse() * q), Quaternion()));
    // Inverse of a non-unit quaternion still inverts.
    const Quaternion scaled(q.v * 3.0f);
    CHECK(MATH::approx_equal(scaled * scaled.inverse(), Quaternion()));
    CHECK(MATH::approx_equal(scaled.normalized(), q));
    CHECK(Quaternion(Vector4()).normalized() == Quaternion());
}

TEST_CASE("math/quaternion: from_euler is yaw * pitch * roll") {
    const f32 pitch = 0.3f, yaw = -1.1f, roll = 0.7f;
    const Quaternion expected = Quaternion::rotation_y(yaw) * Quaternion::rotation_x(pitch) * Quaternion::rotation_z(roll);
    CHECK(MATH::approx_equal(Quaternion::from_euler(pitch, yaw, roll), expected));
}

// to_euler reads the matrix of yaw * pitch * roll. With Ry(y) Rx(p) Rz(r):
//   row 0 = [cy cr + sy sp sr,  -cy sr + sy sp cr,  sy cp]
//   row 1 = [cp sr,              cp cr,             -sp   ]
//   row 2 = [-sy cr + cy sp sr,  sy sr + cy sp cr,  cy cp]
// so pitch = asin(-m12), yaw = atan2(m02, m22), roll = atan2(m10, m11).
TEST_CASE("math/quaternion: to_euler inverts from_euler") {
    const Vector3 identity = Quaternion().to_euler();
    CHECK(identity.x == doctest::Approx(0.0f));
    CHECK(identity.y == doctest::Approx(0.0f));
    CHECK(identity.z == doctest::Approx(0.0f));

    const Vector3 e = Quaternion::from_euler(0.3f, -1.1f, 0.7f).to_euler();
    CHECK(e.x == doctest::Approx(0.3f).epsilon(1e-4f));
    CHECK(e.y == doctest::Approx(-1.1f).epsilon(1e-4f));
    CHECK(e.z == doctest::Approx(0.7f).epsilon(1e-4f));

    SUBCASE("round trips over the whole range") {
        const f32 pitches[] = {-1.4f, -0.5f, 0.0f, 0.9f, 1.5f};
        const f32 yaws[] = {-3.0f, -1.7f, 0.0f, 0.4f, 2.9f};
        const f32 rolls[] = {-2.5f, -0.1f, 0.0f, 1.3f, 3.1f};
        for (const f32 pitch : pitches) {
            for (const f32 yaw : yaws) {
                for (const f32 roll : rolls) {
                    const Quaternion q = Quaternion::from_euler(pitch, yaw, roll);
                    const Vector3 back = q.to_euler();
                    CHECK(MATH::approx_equal(Quaternion::from_euler(back.x, back.y, back.z), q, 1e-4f));
                    CHECK(std::fabs(back.x) <= MATH::PI * 0.5f + 1e-5f);
                }
            }
        }
    }

    SUBCASE("gimbal lock reports the turn as yaw and roll as 0") {
        for (const f32 sign : {1.0f, -1.0f}) {
            const Quaternion q = Quaternion::from_euler(sign * MATH::PI * 0.5f, 0.4f, 0.9f);
            const Vector3 back = q.to_euler();
            CHECK(back.x == doctest::Approx(sign * MATH::PI * 0.5f));
            CHECK(back.z == doctest::Approx(0.0f));
            CHECK(MATH::approx_equal(Quaternion::from_euler(back.x, back.y, back.z), q, 1e-4f));
        }
    }
}

TEST_CASE("math/quaternion: from_to") {
    const Vector3 from = Vector3(1.0f, 2.0f, 3.0f).normalized();
    const Vector3 to = Vector3(-2.0f, 0.5f, 1.0f).normalized();
    CHECK(MATH::approx_equal(Quaternion::from_to(from, to).rotate(from), to));
    CHECK(MATH::approx_equal(Quaternion::from_to(from, from), Quaternion()));
    // Opposite vectors: 180 degrees about some perpendicular.
    const Quaternion flip = Quaternion::from_to(Vector3::unit_x(), -Vector3::unit_x());
    CHECK(MATH::approx_equal(flip.rotate(Vector3::unit_x()), -Vector3::unit_x()));
    CHECK(flip.length() == doctest::Approx(1.0f));
    const Quaternion flip_y = Quaternion::from_to(Vector3::unit_y(), -Vector3::unit_y());
    CHECK(MATH::approx_equal(flip_y.rotate(Vector3::unit_y()), -Vector3::unit_y()));
}

TEST_CASE("math/quaternion: slerp and nlerp") {
    const Quaternion a;
    const Quaternion b = Quaternion::rotation_y(MATH::HALF_PI);
    CHECK(MATH::approx_equal(MATH::slerp(a, b, 0.0f), a));
    CHECK(MATH::approx_equal(MATH::slerp(a, b, 1.0f), b));
    // Halfway is 45 degrees.
    const Quaternion mid = MATH::slerp(a, b, 0.5f);
    CHECK(MATH::approx_equal(mid, Quaternion::rotation_y(MATH::HALF_PI * 0.5f), 1e-4f));
    CHECK(mid.length() == doctest::Approx(1.0f));
    // Constant angular speed: equal steps give equal angles.
    const f32 step1 = MATH::angle(MATH::slerp(a, b, 0.25f), MATH::slerp(a, b, 0.5f));
    const f32 step2 = MATH::angle(MATH::slerp(a, b, 0.5f), MATH::slerp(a, b, 0.75f));
    CHECK(step1 == doctest::Approx(step2).epsilon(1e-3));
    // Takes the short way: -b is the same rotation and must not go the long way.
    CHECK(MATH::approx_equal(MATH::slerp(a, Quaternion(-b.v), 0.5f), mid, 1e-4f));
    // Nearly parallel inputs fall back to nlerp without NaNs.
    const Quaternion close = Quaternion::rotation_y(1e-4f);
    const Quaternion s = MATH::slerp(a, close, 0.5f);
    CHECK(s.length() == doctest::Approx(1.0f));
    CHECK(MATH::approx_equal(s, Quaternion::rotation_y(0.5e-4f), 1e-5f));
    // nlerp endpoints and normalization.
    CHECK(MATH::approx_equal(MATH::nlerp(a, b, 1.0f), b));
    CHECK(MATH::nlerp(a, b, 0.5f).length() == doctest::Approx(1.0f));
    CHECK(MATH::approx_equal(MATH::nlerp(a, Quaternion(-b.v), 0.5f), MATH::nlerp(a, b, 0.5f)));
}

TEST_CASE("math/quaternion: angle and approx_equal treat q and -q alike") {
    const Quaternion q = Quaternion::rotation_z(1.0f);
    const Quaternion neg(-q.v);
    CHECK(q != neg);
    CHECK(MATH::approx_equal(q, neg));
    CHECK(MATH::angle(q, neg) == doctest::Approx(0.0f));
    CHECK(MATH::angle(Quaternion(), q) == doctest::Approx(1.0f));
    CHECK(MATH::angle(Quaternion(), Quaternion::rotation_z(MATH::PI)) == doctest::Approx(MATH::PI));
}
