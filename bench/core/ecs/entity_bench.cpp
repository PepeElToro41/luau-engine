#include "support/bench.hpp"

#include "engine/ecs/world.hpp"

BENCH_CASE("ecs/entity: new_entity") {
    World world;
    world.init();
    bench.run("new_entity", [&] { ankerl::nanobench::doNotOptimizeAway(world.new_entity()); });
    world.free();
}

BENCH_CASE("ecs/entity: create + delete churn") {
    World world;
    world.init();
    bench.run("empty entity", [&] {
        const EntityId e = world.new_entity();
        world.delete_entity(e);
    });
    bench.run("entity with 1 component", [&] {
        const EntityId e = world.new_entity();
        world.set<Position>(e, {1, 2});
        world.delete_entity(e);
    });
    bench.run("entity with 4 components", [&] {
        const EntityId e = world.new_entity();
        world.set<Position>(e, {1, 2});
        world.set<Velocity>(e, {1, 1});
        world.set<Health>(e, {10});
        world.add<TagA>(e);
        world.delete_entity(e);
    });
    world.free();
}

BENCH_CASE("ecs/entity: alive") {
    World world;
    world.init();
    const EntityId e = world.new_entity();
    const EntityId dead = world.new_entity();
    world.delete_entity(dead);
    bench.run("alive entity", [&] { ankerl::nanobench::doNotOptimizeAway(world.alive(e)); });
    bench.run("dead entity", [&] { ankerl::nanobench::doNotOptimizeAway(world.alive(dead)); });
    world.free();
}
