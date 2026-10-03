#include "support/bench.hpp"

#include "engine/templates/dynamic_array.hpp"

BENCH_CASE("templates/dynamic_array: push") {
    constexpr usz count = 1024;
    bench.batch(count).run("push 1024 u64 then free", [&] {
        DynamicArray<u64> array;
        for (usz i = 0; i < count; ++i) {
            array.push(i);
        }
        ankerl::nanobench::doNotOptimizeAway(array.data);
        array.free();
    });

    DynamicArray<u64> reserved;
    reserved.reserve(count);
    bench.batch(count).run("push 1024 u64, reserved, then clear", [&] {
        for (usz i = 0; i < count; ++i) {
            reserved.push(i);
        }
        ankerl::nanobench::doNotOptimizeAway(reserved.data);
        reserved.clear();
    });
    reserved.free();
}

BENCH_CASE("templates/dynamic_array: iterate") {
    constexpr usz count = 4096;
    DynamicArray<u64> array;
    for (usz i = 0; i < count; ++i) {
        array.push(i);
    }
    bench.batch(count).run("sum 4096 u64", [&] {
        u64 sum = 0;
        for (const u64 v : array) {
            sum += v;
        }
        ankerl::nanobench::doNotOptimizeAway(sum);
    });
    array.free();
}
