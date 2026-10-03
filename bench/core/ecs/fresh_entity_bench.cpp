#include "support/bench.hpp"

#include "engine/ecs/ecs.hpp"
#include "engine/ecs/world.hpp"

// One structural change on an entity that has never had the id: every
// iteration gets a fresh entity from a pool built before the measurement,
// so only the add / set itself is timed (no remove, no new_entity).

constexpr usz FRESH_COUNT = 100000;

// Fills `pool` with FRESH_COUNT new entities carrying `existing` ids already.
static void make_pool(World& world, DynamicArray<EntityId>& pool, const int existing) {
    pool.clear();
    for (usz i = 0; i < FRESH_COUNT; ++i) {
        const EntityId e = world.new_entity();
        if (existing >= 1) {
            world.set<Velocity>(e, {1, 1});
        }
        if (existing >= 2) {
            world.set<Health>(e, {10});
        }
        if (existing >= 3) {
            world.add<TagA>(e);
        }
        if (existing >= 4) {
            world.add<TagB>(e);
        }
        pool.push(e);
    }
}

BENCH_CASE("ecs/fresh: set on a fresh entity") {
    World world;
    world.init();
    DynamicArray<EntityId> pool;
    const Id position = world.id<Position>();
    const Position value {1, 2};

    make_pool(world, pool, 0);
    BENCH::run_indexed(bench, "set, raw id, entity with 0 ids", FRESH_COUNT, [&](const usz i) {
        world.set(pool[i], position, &value);
    });

    make_pool(world, pool, 0);
    BENCH::run_indexed(bench, "set<T>, entity with 0 ids", FRESH_COUNT, [&](const usz i) {
        world.set<Position>(pool[i], value);
    });

    make_pool(world, pool, 1);
    BENCH::run_indexed(bench, "set<T>, entity with 1 id", FRESH_COUNT, [&](const usz i) {
        world.set<Position>(pool[i], value);
    });

    make_pool(world, pool, 2);
    BENCH::run_indexed(bench, "set<T>, entity with 2 ids", FRESH_COUNT, [&](const usz i) {
        world.set<Position>(pool[i], value);
    });

    make_pool(world, pool, 4);
    BENCH::run_indexed(bench, "set<T>, entity with 4 ids", FRESH_COUNT, [&](const usz i) {
        world.set<Position>(pool[i], value);
    });

    // Same as the first case, but the destination archetype already has room
    // for every row (the previous pools grew it), so no column reallocation
    // happens during the measurement: this is the pure move cost.
    make_pool(world, pool, 0);
    BENCH::run_indexed(bench, "set<T>, entity with 0 ids, archetype pre-grown", FRESH_COUNT, [&](const usz i) {
        world.set<Position>(pool[i], value);
    });

    pool.free();
    world.free();
}

BENCH_CASE("ecs/fresh: add on a fresh entity") {
    World world;
    world.init();
    DynamicArray<EntityId> pool;
    const Id tag = world.id<Likes>();

    make_pool(world, pool, 0);
    BENCH::run_indexed(bench, "add tag, raw id, entity with 0 ids", FRESH_COUNT, [&](const usz i) {
        world.add(pool[i], tag);
    });

    make_pool(world, pool, 0);
    BENCH::run_indexed(bench, "add<T> tag, entity with 0 ids", FRESH_COUNT, [&](const usz i) {
        world.add<Likes>(pool[i]);
    });

    make_pool(world, pool, 4);
    BENCH::run_indexed(bench, "add<T> tag, entity with 4 ids", FRESH_COUNT, [&](const usz i) {
        world.add<Likes>(pool[i]);
    });

    const EntityId target = world.new_entity();
    make_pool(world, pool, 0);
    BENCH::run_indexed(bench, "add (CHILD_OF, target), entity with 0 ids", FRESH_COUNT, [&](const usz i) {
        world.add(pool[i], ECS::PAIR(ECS::CHILD_OF, target));
    });

    pool.free();
    world.free();
}
