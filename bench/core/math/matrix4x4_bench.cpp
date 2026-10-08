#include "support/bench.hpp"

#include "engine/math/math.hpp"

BENCH_CASE("math/matrix4x4: multiply") {
    const Matrix4x4 a = Matrix4x4::trs(Vector3(1.0f, 2.0f, 3.0f), Quaternion::rotation_y(0.4f), Vector3(2.0f));
    const Matrix4x4 b = Matrix4x4::perspective(MATH::radians(60.0f), 1.5f, 0.1f, 100.0f);
    bench.run("mat4 * mat4", [&] {
        ankerl::nanobench::doNotOptimizeAway(a * b);
    });
    const Vector4 v(1.0f, 2.0f, 3.0f, 1.0f);
    bench.run("mat4 * vec4", [&] {
        ankerl::nanobench::doNotOptimizeAway(a * v);
    });
    const Vector3 p(1.0f, 2.0f, 3.0f);
    bench.run("transform_point", [&] {
        ankerl::nanobench::doNotOptimizeAway(a.transform_point(p));
    });
}

BENCH_CASE("math/matrix4x4: inverse") {
    const Matrix4x4 m = Matrix4x4::trs(Vector3(1.0f, 2.0f, 3.0f), Quaternion::from_euler(0.3f, 1.2f, -0.5f), Vector3(2.0f, 0.5f, 1.5f));
    bench.run("inverse (general)", [&] {
        ankerl::nanobench::doNotOptimizeAway(m.inverse());
    });
    bench.run("inverse_affine", [&] {
        ankerl::nanobench::doNotOptimizeAway(m.inverse_affine());
    });
    bench.run("transposed", [&] {
        ankerl::nanobench::doNotOptimizeAway(m.transposed());
    });
}

BENCH_CASE("math/matrix4x4: batch transform") {
    constexpr usz count = 4096;
    DynamicArray<Matrix4x4> models;
    DynamicArray<Vector3> points;
    for (usz i = 0; i < count; ++i) {
        const f32 f = static_cast<f32>(i);
        models.push(Matrix4x4::trs(Vector3(f, 0.0f, -f), Quaternion::rotation_y(f * 0.01f), Vector3(1.0f)));
        points.push(Vector3(f * 0.5f, 1.0f, -2.0f));
    }
    const Matrix4x4 view_proj = Matrix4x4::perspective(MATH::radians(60.0f), 1.5f, 0.1f, 1000.0f)
        * Matrix4x4::look_at(Vector3(0.0f, 10.0f, 10.0f), Vector3(), Vector3::up());

    bench.batch(count).run("4096 model matrices * view_proj", [&] {
        Vector4 acc;
        for (usz i = 0; i < count; ++i) {
            acc += (view_proj * models[i]).columns[3];
        }
        ankerl::nanobench::doNotOptimizeAway(acc);
    });
    bench.batch(count).run("4096 transform_point", [&] {
        Vector3 acc;
        for (usz i = 0; i < count; ++i) {
            acc += models[i].transform_point(points[i]);
        }
        ankerl::nanobench::doNotOptimizeAway(acc);
    });
    models.free();
    points.free();
}

BENCH_CASE("math/quaternion: rotate and multiply") {
    const Quaternion a = Quaternion::from_axis_angle(Vector3(0.3f, -0.7f, 0.2f).normalized(), 0.9f);
    const Quaternion b = Quaternion::rotation_y(1.1f);
    const Vector3 p(1.0f, 2.0f, 3.0f);
    bench.run("quat * quat", [&] {
        ankerl::nanobench::doNotOptimizeAway(a * b);
    });
    bench.run("quat rotate vec3", [&] {
        ankerl::nanobench::doNotOptimizeAway(a.rotate(p));
    });
    bench.run("slerp", [&] {
        ankerl::nanobench::doNotOptimizeAway(MATH::slerp(a, b, 0.3f));
    });
}

BENCH_CASE("math/vector3: dot, cross, normalize") {
    const Vector3 a(1.0f, 2.0f, 3.0f);
    const Vector3 b(-4.0f, 0.5f, 2.0f);
    bench.run("dot", [&] {
        ankerl::nanobench::doNotOptimizeAway(MATH::dot(a, b));
    });
    bench.run("cross", [&] {
        ankerl::nanobench::doNotOptimizeAway(MATH::cross(a, b));
    });
    bench.run("normalized", [&] {
        ankerl::nanobench::doNotOptimizeAway(b.normalized());
    });
}
