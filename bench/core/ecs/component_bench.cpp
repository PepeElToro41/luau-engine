#include "support/bench.hpp"

#include "engine/ecs/world.hpp"

// Archetype moves: every add / remove of an id moves the entity's row to
// another archetype, so these are the core structural-change costs.
BENCH_CASE("ecs/component: add + remove (archetype move)") {
    World world;
    world.init();
    const EntityId e = world.new_entity();
    const Id position = world.id<Position>();
    const Id tag_a = world.id<TagA>();
    const Position value {1, 2};

    bench.run("1 tag, raw id", [&] {
        world.add(e, tag_a);
        world.remove(e, tag_a);
    });
    bench.run("1 tag, typed", [&] {
        world.add<TagA>(e);
        world.remove<TagA>(e);
    });
    bench.run("1 component, raw id", [&] {
        world.set(e, position, &value);
        world.remove(e, position);
    });
    bench.run("1 component, typed", [&] {
        world.set<Position>(e, value);
        world.remove<Position>(e);
    });
    bench.run("4 ids, typed", [&] {
        world.set<Position>(e, value);
        world.set<Velocity>(e, {1, 1});
        world.set<Health>(e, {10});
        world.add<TagA>(e);
        world.remove<Position>(e);
        world.remove<Velocity>(e);
        world.remove<Health>(e);
        world.remove<TagA>(e);
    });
    world.free();
}

BENCH_CASE("ecs/component: add + remove on a wide entity") {
    World world;
    world.init();
    const EntityId e = world.new_entity();
    // 15 resident ids so the moved row carries 15 columns across each move.
    world.set<Position>(e, {1, 2});
    world.set<Velocity>(e, {1, 1});
    world.set<Health>(e, {10});
    world.add<TagA>(e);
    world.add<TagB>(e);
    for (int i = 0; i < 10; ++i) {
        world.add(e, world.new_entity());
    }
    const Id extra = world.new_entity();

    bench.run("16th id add + remove", [&] {
        world.add(e, extra);
        world.remove(e, extra);
    });
    world.free();
}

BENCH_CASE("ecs/component: set / get / has in place") {
    World world;
    world.init();
    const EntityId e = world.new_entity();
    const Id position = world.id<Position>();
    world.set<Position>(e, {1, 2});
    Position value {3, 4};

    bench.run("set, raw id", [&] { world.set(e, position, &value); });
    bench.run("set<T>", [&] { world.set<Position>(e, value); });
    bench.run("get, raw id", [&] { ankerl::nanobench::doNotOptimizeAway(world.get(e, position)); });
    bench.run("get<T>", [&] { ankerl::nanobench::doNotOptimizeAway(world.get<Position>(e)); });
    bench.run("has, raw id", [&] { ankerl::nanobench::doNotOptimizeAway(world.has(e, position)); });
    bench.run("has<T>", [&] { ankerl::nanobench::doNotOptimizeAway(world.has<Position>(e)); });
    bench.run("has<T> missing", [&] { ankerl::nanobench::doNotOptimizeAway(world.has<Velocity>(e)); });
    world.free();
}

BENCH_CASE("ecs/component: get across a populated archetype") {
    World world;
    world.init();
    constexpr usz count = 4096;
    DynamicArray<EntityId> entities;
    for (usz i = 0; i < count; ++i) {
        const EntityId e = world.new_entity();
        world.set<Position>(e, {static_cast<f32>(i), 0});
        world.set<Velocity>(e, {1, 1});
        entities.push(e);
    }
    const Id position = world.id<Position>();

    bench.batch(count).run("get + write all rows, raw id", [&] {
        for (const EntityId e : entities) {
            Position* p = static_cast<Position*>(world.get(e, position));
            p->x += 1;
        }
    });
    entities.free();
    world.free();
}
