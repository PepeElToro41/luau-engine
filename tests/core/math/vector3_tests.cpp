#include "support/test_support.hpp"

#include "engine/math/vector3.hpp"

#include <cmath>

TEST_CASE("math/vector3: constructors zero the padding lane") {
    const Vector3 d;
    CHECK(d.x == 0.0f);
    CHECK(d.y == 0.0f);
    CHECK(d.z == 0.0f);
    CHECK(d.w == 0.0f);
    const Vector3 v(1.0f, 2.0f, 3.0f);
    CHECK(v.x == 1.0f);
    CHECK(v.y == 2.0f);
    CHECK(v.z == 3.0f);
    CHECK(v.w == 0.0f);
    CHECK(v[0] == 1.0f);
    CHECK(v[2] == 3.0f);
    CHECK(Vector3(5.0f) == Vector3(5.0f, 5.0f, 5.0f));
    CHECK(Vector3::forward() == Vector3(0.0f, 0.0f, -1.0f));
    CHECK(Vector3::up() == Vector3::unit_y());
}

TEST_CASE("math/vector3: load and store use 12 bytes") {
    // Unaligned, packed like a vertex stream.
    const f32 stream[7] = {9.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    const Vector3 a = Vector3::load(stream + 1);
    const Vector3 b = Vector3::load(stream + 4);
    CHECK(a == Vector3(1.0f, 2.0f, 3.0f));
    CHECK(a.w == 0.0f);
    CHECK(b == Vector3(4.0f, 5.0f, 6.0f));

    f32 out[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    b.store(out);
    CHECK(out[0] == 4.0f);
    CHECK(out[1] == 5.0f);
    CHECK(out[2] == 6.0f);
    CHECK(out[3] == -1.0f); // untouched
}

TEST_CASE("math/vector3: arithmetic") {
    const Vector3 a(1.0f, 2.0f, 3.0f);
    const Vector3 b(4.0f, 8.0f, 12.0f);
    CHECK(a + b == Vector3(5.0f, 10.0f, 15.0f));
    CHECK(b - a == Vector3(3.0f, 6.0f, 9.0f));
    CHECK(a * b == Vector3(4.0f, 16.0f, 36.0f));
    CHECK(b / a == Vector3(4.0f, 4.0f, 4.0f));
    CHECK(a * 2.0f == Vector3(2.0f, 4.0f, 6.0f));
    CHECK(2.0f * a == Vector3(2.0f, 4.0f, 6.0f));
    CHECK(b / 2.0f == Vector3(2.0f, 4.0f, 6.0f));
    CHECK(-a == Vector3(-1.0f, -2.0f, -3.0f));
    Vector3 c = a;
    c += b;
    c -= Vector3(1.0f);
    c *= 2.0f;
    c /= Vector3(2.0f, 1.0f, 1.0f);
    CHECK(c == Vector3(4.0f, 18.0f, 28.0f));
}

TEST_CASE("math/vector3: comparisons ignore the padding lane") {
    const Vector3 a(1.0f, 2.0f, 3.0f);
    // 0 / 0 in the padding lane leaves a NaN there.
    const Vector3 b = a / Vector3(1.0f, 1.0f, 1.0f);
    CHECK(std::isnan(b.w));
    CHECK(b == a);
    CHECK(MATH::approx_equal(a, b));
    CHECK(b.length_squared() == 14.0f);
    CHECK(MATH::dot(a, b) == 14.0f);
    CHECK(MATH::cross(a, b) == Vector3());
    CHECK_FALSE(a == Vector3(1.0f, 2.0f, 3.5f));
    CHECK(a != Vector3(1.0f, 2.0f, 3.5f));
}

TEST_CASE("math/vector3: length and normalize") {
    const Vector3 v(2.0f, 3.0f, 6.0f);
    CHECK(v.length_squared() == 49.0f);
    CHECK(v.length() == doctest::Approx(7.0f));
    const Vector3 n = v.normalized();
    CHECK(n.length() == doctest::Approx(1.0f));
    CHECK(MATH::approx_equal(n, Vector3(2.0f / 7.0f, 3.0f / 7.0f, 6.0f / 7.0f)));
    CHECK(Vector3().normalized() == Vector3());
}

TEST_CASE("math/vector3: dot and cross") {
    CHECK(MATH::dot(Vector3(1.0f, 2.0f, 3.0f), Vector3(4.0f, -5.0f, 6.0f)) == 12.0f);
    CHECK(MATH::cross(Vector3::unit_x(), Vector3::unit_y()) == Vector3::unit_z());
    CHECK(MATH::cross(Vector3::unit_y(), Vector3::unit_z()) == Vector3::unit_x());
    CHECK(MATH::cross(Vector3::unit_z(), Vector3::unit_x()) == Vector3::unit_y());
    CHECK(MATH::cross(Vector3::unit_y(), Vector3::unit_x()) == -Vector3::unit_z());
    const Vector3 a(1.0f, 2.0f, 3.0f);
    const Vector3 b(-7.0f, 8.0f, 9.0f);
    const Vector3 c = MATH::cross(a, b);
    CHECK(c == Vector3(2.0f * 9.0f - 3.0f * 8.0f, 3.0f * -7.0f - 1.0f * 9.0f, 1.0f * 8.0f - 2.0f * -7.0f));
    CHECK(MATH::dot(c, a) == doctest::Approx(0.0f));
    CHECK(MATH::dot(c, b) == doctest::Approx(0.0f));
}

TEST_CASE("math/vector3: helpers") {
    CHECK(MATH::lerp(Vector3(0.0f), Vector3(10.0f, 20.0f, 30.0f), 0.25f) == Vector3(2.5f, 5.0f, 7.5f));
    CHECK(MATH::min(Vector3(1.0f, 5.0f, -2.0f), Vector3(3.0f, 2.0f, -1.0f)) == Vector3(1.0f, 2.0f, -2.0f));
    CHECK(MATH::max(Vector3(1.0f, 5.0f, -2.0f), Vector3(3.0f, 2.0f, -1.0f)) == Vector3(3.0f, 5.0f, -1.0f));
    CHECK(MATH::abs(Vector3(-1.0f, 2.0f, -3.0f)) == Vector3(1.0f, 2.0f, 3.0f));
    CHECK(MATH::distance(Vector3(1.0f, 1.0f, 1.0f), Vector3(3.0f, 4.0f, 7.0f)) == doctest::Approx(7.0f));
    CHECK(MATH::approx_equal(MATH::reflect(Vector3(1.0f, -1.0f, 0.0f), Vector3::unit_y()), Vector3(1.0f, 1.0f, 0.0f)));
    CHECK(MATH::approx_equal(MATH::project_onto_plane(Vector3(1.0f, 2.0f, 3.0f), Vector3::unit_y()), Vector3(1.0f, 0.0f, 3.0f)));
    CHECK(MATH::approx_equal(Vector3(1.0f), Vector3(1.0f + 1e-6f, 1.0f - 1e-6f, 1.0f)));
    CHECK_FALSE(MATH::approx_equal(Vector3(1.0f), Vector3(1.0f, 1.0f, 1.01f)));
}

TEST_CASE("math/vector3: layout fits SIMD and component storage") {
    static_assert(sizeof(Vector3) == 16);
    static_assert(alignof(Vector3) == 16);
    alignas(16) Vector3 array[3] = {Vector3(1.0f), Vector3(2.0f), Vector3(3.0f)};
    CHECK(reinterpret_cast<const u8*>(&array[1]) - reinterpret_cast<const u8*>(&array[0]) == 16);
    CHECK(array[2].x == 3.0f);
}
