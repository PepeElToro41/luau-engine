#pragma once

// Self-registering benchmarks. A bench file is one or more BENCH_CASE blocks;
// nothing else has to be edited for main.cpp to pick them up:
//
//     BENCH_CASE("ecs/entity: new_entity") {
//         World world;
//         world.init();
//         bench.run("new_entity", [&] { ankerl::nanobench::doNotOptimizeAway(world.new_entity()); });
//         world.free();
//     }
//
// `bench` is a configured ankerl::nanobench::Bench. Names use the same
// "<area>/<unit>: <what>" scheme as the tests so substring filters on the
// command line select by area or unit.

#include "engine/defines.hpp"
#include "engine/templates/dynamic_array.hpp"

#include <nanobench.h>

// --- Sample component types ---------------------------------------------------

struct Position {
    f32 x = 0;
    f32 y = 0;
};

struct Velocity {
    f32 dx = 0;
    f32 dy = 0;
};

struct Health {
    i32 value = 0;
};

struct TagA {};
struct TagB {};
struct Likes {};

// --- Registry ------------------------------------------------------------------

using BenchFn = void (*)(ankerl::nanobench::Bench& bench);

struct BenchEntry {
    const char* name;
    BenchFn fn;
};

namespace BENCH {

DynamicArray<BenchEntry>& entries();
void register_bench(const char* name, BenchFn fn);

// Runs `op(i)` exactly once for every i in [0, count), split over a fixed
// number of epochs so nanobench still reports an error estimate. Use it when
// every iteration must see a fresh index into a pre-built pool (an entity
// that has not had the component yet, a slot not yet filled, ...), which a
// plain bench.run() cannot do since it repeats the same lambda until the
// epoch time is reached:
//
//     BENCH::run_indexed(bench, "set on fresh entity", count, [&](usz i) {
//         world.set<Position>(pool[i], {1, 2});
//     });
//
// The pool must hold at least `count` elements. Warmup is disabled for the
// run so no index is consumed outside the measurement.
template <typename Op>
void run_indexed(ankerl::nanobench::Bench& bench, const char* name, const usz count, Op&& op) {
    constexpr usz epochs = 50;
    const usz per_epoch = count / epochs;
    usz cursor = 0;
    ankerl::nanobench::Bench local = bench;
    local.warmup(0).epochs(epochs).epochIterations(per_epoch).run(name, [&] { op(cursor++); });
}

} // namespace BENCH

struct BenchRegistrar {
    BenchRegistrar(const char* name, BenchFn fn) { BENCH::register_bench(name, fn); }
};

#define BENCH_CONCAT_INNER(a, b) a##b
#define BENCH_CONCAT(a, b) BENCH_CONCAT_INNER(a, b)

#define BENCH_CASE(name)                                                                               \
    static void BENCH_CONCAT(bench_fn_, __LINE__)(ankerl::nanobench::Bench & bench);                   \
    static BenchRegistrar BENCH_CONCAT(bench_reg_, __LINE__)(name, BENCH_CONCAT(bench_fn_, __LINE__)); \
    static void BENCH_CONCAT(bench_fn_, __LINE__)(ankerl::nanobench::Bench & bench)
