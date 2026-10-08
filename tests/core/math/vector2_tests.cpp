#include "support/test_support.hpp"

#include "engine/math/vector2.hpp"

TEST_CASE("math/vector2: constructors and constants") {
    const Vector2 d;
    CHECK(d.x == 0.0f);
    CHECK(d.y == 0.0f);
    const Vector2 v(1.0f, 2.0f);
    CHECK(v[0] == 1.0f);
    CHECK(v[1] == 2.0f);
    CHECK(Vector2(3.0f) == Vector2(3.0f, 3.0f));
    CHECK(Vector2::unit_x() + Vector2::unit_y() == Vector2::one());
    static_assert(sizeof(Vector2) == 8);
}

TEST_CASE("math/vector2: arithmetic") {
    const Vector2 a(1.0f, 2.0f);
    const Vector2 b(4.0f, 8.0f);
    CHECK(a + b == Vector2(5.0f, 10.0f));
    CHECK(b - a == Vector2(3.0f, 6.0f));
    CHECK(a * b == Vector2(4.0f, 16.0f));
    CHECK(b / a == Vector2(4.0f, 4.0f));
    CHECK(a * 2.0f == Vector2(2.0f, 4.0f));
    CHECK(2.0f * a == Vector2(2.0f, 4.0f));
    CHECK(b / 2.0f == Vector2(2.0f, 4.0f));
    CHECK(-a == Vector2(-1.0f, -2.0f));
    Vector2 c = a;
    c += b;
    c *= 2.0f;
    CHECK(c == Vector2(10.0f, 20.0f));
}

TEST_CASE("math/vector2: length, normalize, dot, cross") {
    const Vector2 v(3.0f, 4.0f);
    CHECK(v.length_squared() == 25.0f);
    CHECK(v.length() == doctest::Approx(5.0f));
    CHECK(MATH::approx_equal(v.normalized(), Vector2(0.6f, 0.8f)));
    CHECK(Vector2().normalized() == Vector2());
    CHECK(MATH::dot(Vector2(1.0f, 2.0f), Vector2(3.0f, 4.0f)) == 11.0f);
    CHECK(MATH::cross(Vector2::unit_x(), Vector2::unit_y()) == 1.0f);
    CHECK(MATH::cross(Vector2::unit_y(), Vector2::unit_x()) == -1.0f);
    CHECK(Vector2::unit_x().perpendicular() == Vector2::unit_y());
}

TEST_CASE("math/vector2: helpers") {
    CHECK(MATH::lerp(Vector2(0.0f, 10.0f), Vector2(10.0f, 20.0f), 0.5f) == Vector2(5.0f, 15.0f));
    CHECK(MATH::min(Vector2(1.0f, 5.0f), Vector2(3.0f, 2.0f)) == Vector2(1.0f, 2.0f));
    CHECK(MATH::max(Vector2(1.0f, 5.0f), Vector2(3.0f, 2.0f)) == Vector2(3.0f, 5.0f));
    CHECK(MATH::abs(Vector2(-1.0f, 2.0f)) == Vector2(1.0f, 2.0f));
    CHECK(MATH::distance(Vector2(1.0f, 1.0f), Vector2(4.0f, 5.0f)) == doctest::Approx(5.0f));
    CHECK(MATH::approx_equal(Vector2(1.0f, 1.0f), Vector2(1.0f + 1e-6f, 1.0f)));
    CHECK_FALSE(MATH::approx_equal(Vector2(1.0f, 1.0f), Vector2(1.1f, 1.0f)));
}
