#include "support/bench.hpp"

#include "engine/templates/hash_map.hpp"

BENCH_CASE("templates/hash_map: insert") {
    constexpr usz count = 1024;
    bench.batch(count).run("insert 1024 u64 keys then free", [&] {
        HashMap<u64, u64> map;
        for (usz i = 0; i < count; ++i) {
            map.insert(i * 7919, i);
        }
        ankerl::nanobench::doNotOptimizeAway(map.count);
        map.free();
    });
}

BENCH_CASE("templates/hash_map: find") {
    constexpr usz count = 4096;
    HashMap<u64, u64> map;
    for (usz i = 0; i < count; ++i) {
        map.insert(i * 7919, i);
    }
    usz cursor = 0;
    bench.run("find hit", [&] {
        cursor = (cursor + 1) % count;
        ankerl::nanobench::doNotOptimizeAway(map.find(cursor * 7919));
    });
    bench.run("find miss", [&] {
        cursor = (cursor + 1) % count;
        ankerl::nanobench::doNotOptimizeAway(map.find(cursor * 7919 + 1));
    });
    map.free();
}

BENCH_CASE("templates/hash_map: remove + insert churn") {
    constexpr usz count = 4096;
    HashMap<u64, u64> map;
    for (usz i = 0; i < count; ++i) {
        map.insert(i, i);
    }
    u64 next = count;
    bench.run("remove oldest, insert new (tombstones)", [&] {
        map.remove(next - count);
        map.insert(next, next);
        next += 1;
    });
    map.free();
}
