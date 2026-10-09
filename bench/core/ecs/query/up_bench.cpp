#include "support/bench.hpp"

#include "engine/ecs/ecs.hpp"
#include "engine/ecs/query/dynamic_query.hpp"
#include "engine/ecs/query/query_builder.hpp"
#include "engine/ecs/world.hpp"

// up() per archetype as a function of how far the id is: on the parent, or
// only on the root of a chain `levels` deep. Every entity is in its own
// archetype (a distinct (CHILD_OF, parent) pair per entity does that), so
// the figures are per archetype, like dynamic_query_bench.

namespace {

ankerl::nanobench::Bench walks(const ankerl::nanobench::Bench& bench, const usz batch) {
    ankerl::nanobench::Bench local = bench;
    local.warmup(2).minEpochIterations(20).batch(batch);
    return local;
}

// `chains` chains of `levels` entities below a root each: the roots hold
// TagA, every entity holds a Position. Returns the number of leaves, the
// entities the queries below match (they hold Velocity).
usz populate(World& world, const usz chains, const usz levels) {
    for (usz c = 0; c < chains; c++) {
        EntityId parent = world.new_entity();
        world.add<TagA>(parent);
        world.set<Position>(parent, { 1, 1 });
        for (usz level = 0; level < levels; level++) {
            const EntityId child = world.new_entity();
            world.add(child, world.pair(ECS::CHILD_OF, parent));
            world.set<Position>(child, { 1, 1 });
            if (level + 1 == levels) {
                world.set<Velocity>(child, { 1, 1 });
            }
            parent = child;
        }
    }
    return chains;
}

} // namespace

BENCH_CASE("ecs/dynamic_query: up() per archetype against the distance to the id") {
    constexpr usz chains = 2000;
    constexpr usz levels = 8;

    World world;
    world.init();
    populate(world, chains, levels);

    // Position is on the parent: one level. TagA is on the root: `levels`
    // levels up. Health is nowhere: the whole chain is walked and fails.
    QueryBuilder near_builder = world.query_build();
    near_builder.term<Velocity>().term<Position>().up();
    DynamicQuery near = near_builder.build();
    QueryBuilder far_builder = world.query_build();
    far_builder.term<Velocity>().with<TagA>().up();
    DynamicQuery far = far_builder.build();
    QueryBuilder miss_builder = world.query_build();
    miss_builder.term<Velocity>().with<Health>().up().optional();
    DynamicQuery miss = miss_builder.build();
    QueryBuilder far_cached_builder = world.query_build(QUERY_CACHED);
    far_cached_builder.term<Velocity>().with<TagA>().up();
    DynamicQuery far_cached = far_cached_builder.build();

    walks(bench, chains).run("up(): id on the parent", [&] {
        near.each<Velocity, Position>([](EntityId, Velocity& velocity, Position& position) { velocity.dx += position.x; });
    });
    walks(bench, chains).run("up(): id on the root, 8 levels up", [&] {
        far.each<Velocity>([](EntityId, Velocity& velocity) { velocity.dx += 1; });
    });
    walks(bench, chains).run("up(): id on the root, 8 levels up, cached query", [&] {
        far_cached.each<Velocity>([](EntityId, Velocity& velocity) { velocity.dx += 1; });
    });
    walks(bench, chains).run("up().optional(): id nowhere, 8 levels walked", [&] {
        miss.each<Velocity>([](EntityId, Velocity& velocity) { velocity.dx += 1; });
    });

    far_cached.free();
    far_cached_builder.free();
    miss.free();
    miss_builder.free();
    far.free();
    far_builder.free();
    near.free();
    near_builder.free();
    world.free();
}
