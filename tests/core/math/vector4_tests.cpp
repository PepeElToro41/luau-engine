#include "support/test_support.hpp"

#include "engine/math/vector4.hpp"

TEST_CASE("math/vector4: constructors") {
    const Vector4 d;
    CHECK(d == Vector4(0.0f, 0.0f, 0.0f, 0.0f));
    const Vector4 v(1.0f, 2.0f, 3.0f, 4.0f);
    CHECK(v.x == 1.0f);
    CHECK(v.y == 2.0f);
    CHECK(v.z == 3.0f);
    CHECK(v.w == 4.0f);
    CHECK(v[3] == 4.0f);
    CHECK(Vector4(2.0f) == Vector4(2.0f, 2.0f, 2.0f, 2.0f));
    CHECK(Vector4(Vector3(1.0f, 2.0f, 3.0f), 9.0f) == Vector4(1.0f, 2.0f, 3.0f, 9.0f));
    CHECK(v.xyz() == Vector3(1.0f, 2.0f, 3.0f));
    CHECK(Vector4::unit_x() + Vector4::unit_y() + Vector4::unit_z() + Vector4::unit_w() == Vector4::one());
}

TEST_CASE("math/vector4: load and store") {
    alignas(16) const f32 in[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    const Vector4 v = Vector4::load(in);
    CHECK(v == Vector4(1.0f, 2.0f, 3.0f, 4.0f));
    alignas(16) f32 out[4] = {};
    (v * 2.0f).store(out);
    CHECK(out[0] == 2.0f);
    CHECK(out[3] == 8.0f);
}

TEST_CASE("math/vector4: arithmetic uses every lane") {
    const Vector4 a(1.0f, 2.0f, 3.0f, 4.0f);
    const Vector4 b(2.0f, 4.0f, 6.0f, 8.0f);
    CHECK(a + b == Vector4(3.0f, 6.0f, 9.0f, 12.0f));
    CHECK(b - a == a);
    CHECK(a * b == Vector4(2.0f, 8.0f, 18.0f, 32.0f));
    CHECK(b / a == Vector4(2.0f));
    CHECK(a * 2.0f == b);
    CHECK(2.0f * a == b);
    CHECK(b / 2.0f == a);
    CHECK(-a == Vector4(-1.0f, -2.0f, -3.0f, -4.0f));
    Vector4 c = a;
    c += a;
    c *= Vector4(1.0f, 1.0f, 1.0f, 0.5f);
    CHECK(c == Vector4(2.0f, 4.0f, 6.0f, 4.0f));
    CHECK_FALSE(a == Vector4(1.0f, 2.0f, 3.0f, 4.5f));
}

TEST_CASE("math/vector4: dot, length, normalize") {
    const Vector4 a(1.0f, 2.0f, 3.0f, 4.0f);
    CHECK(MATH::dot(a, Vector4(1.0f)) == 10.0f);
    CHECK(MATH::dot(a, a) == 30.0f);
    CHECK(a.length_squared() == 30.0f);
    CHECK(Vector4(2.0f).length() == doctest::Approx(4.0f));
    CHECK(MATH::approx_equal(Vector4(0.0f, 3.0f, 0.0f, 4.0f).normalized(), Vector4(0.0f, 0.6f, 0.0f, 0.8f)));
    CHECK(Vector4().normalized() == Vector4());
}

TEST_CASE("math/vector4: helpers") {
    CHECK(MATH::lerp(Vector4(0.0f), Vector4(2.0f, 4.0f, 6.0f, 8.0f), 0.5f) == Vector4(1.0f, 2.0f, 3.0f, 4.0f));
    CHECK(MATH::min(Vector4(1.0f, 5.0f, -2.0f, 0.0f), Vector4(3.0f, 2.0f, -1.0f, -1.0f)) == Vector4(1.0f, 2.0f, -2.0f, -1.0f));
    CHECK(MATH::max(Vector4(1.0f, 5.0f, -2.0f, 0.0f), Vector4(3.0f, 2.0f, -1.0f, -1.0f)) == Vector4(3.0f, 5.0f, -1.0f, 0.0f));
    CHECK(MATH::abs(Vector4(-1.0f, 2.0f, -3.0f, -0.0f)) == Vector4(1.0f, 2.0f, 3.0f, 0.0f));
    CHECK(MATH::approx_equal(Vector4(1.0f), Vector4(1.0f, 1.0f, 1.0f, 1.0f + 1e-6f)));
    CHECK_FALSE(MATH::approx_equal(Vector4(1.0f), Vector4(1.0f, 1.0f, 1.0f, 1.1f)));
    static_assert(sizeof(Vector4) == 16 && alignof(Vector4) == 16);
}
