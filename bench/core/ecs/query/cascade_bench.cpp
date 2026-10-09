#include "support/bench.hpp"

#include "engine/ecs/ecs.hpp"
#include "engine/ecs/query/dynamic_query.hpp"
#include "engine/ecs/query/query_builder.hpp"
#include "engine/ecs/query/query_scan.hpp"
#include "engine/ecs/world.hpp"

// cascade(): what ordering the archetypes by depth adds on top of an up()
// walk. Per archetype, like dynamic_query_bench: every entity is in its own
// archetype (a distinct (CHILD_OF, parent) pair per entity does that by
// itself), spread over `levels` depths.

namespace {

ankerl::nanobench::Bench walks(const ankerl::nanobench::Bench& bench, const usz batch) {
    ankerl::nanobench::Bench local = bench;
    local.warmup(2).minEpochIterations(20).batch(batch);
    return local;
}

// `count` entities with a Position, `count / levels` per depth: the first
// slice are roots, every later entity is the child of the entity one slice
// before it, so each child has a parent of its own and its own archetype.
void populate(World& world, const usz count, const usz levels, DynamicArray<EntityId>& entities) {
    const usz width = count / levels;
    entities.resize(count);
    for (usz i = 0; i < count; i++) {
        entities[i] = world.new_entity();
    }
    for (usz i = 0; i < count; i++) {
        if (i >= width) {
            world.add(entities[i], world.pair(ECS::CHILD_OF, entities[i - width]));
        }
        world.set<Position>(entities[i], { static_cast<f32>(i), 0 });
    }
}

} // namespace

BENCH_CASE("ecs/dynamic_query: cascade() per archetype against up()") {
    constexpr usz count = 10000;
    constexpr usz levels = 10;

    World world;
    world.init();
    DynamicArray<EntityId> entities;
    populate(world, count, levels, entities);

    QueryBuilder up_builder = world.query_build();
    up_builder.term<Position>().term<Position>().up().optional();
    DynamicQuery up = up_builder.build();
    QueryBuilder cascade_builder = world.query_build();
    cascade_builder.term<Position>().term<Position>().cascade().optional();
    DynamicQuery cascade = cascade_builder.build();
    QueryBuilder up_cached_builder = world.query_build(QUERY_CACHED);
    up_cached_builder.term<Position>().term<Position>().up().optional();
    DynamicQuery up_cached = up_cached_builder.build();
    QueryBuilder cascade_cached_builder = world.query_build(QUERY_CACHED);
    cascade_cached_builder.term<Position>().term<Position>().cascade().optional();
    DynamicQuery cascade_cached = cascade_cached_builder.build();

    auto propagate = [](EntityId, Position& own, Position* parent) {
        if (parent != nullptr) {
            own.y = parent->y + 1;
        }
    };

    walks(bench, count).run("up().optional() each, 1 row per archetype", [&] {
        up.each<Position, Position*>(propagate);
    });
    walks(bench, count).run("cascade().optional() each, sorted every run", [&] {
        cascade.each<Position, Position*>(propagate);
    });
    walks(bench, count).run("up().optional() each, cached", [&] {
        up_cached.each<Position, Position*>(propagate);
    });
    walks(bench, count).run("cascade().optional() each, cached, order current", [&] {
        cascade_cached.each<Position, Position*>(propagate);
    });
    cascade_cached.ensure_cache();
    walks(bench, count).run("cascade().optional() each, cached, order stale every run", [&] {
        // What a frame that changed some depth pays: the lazy re-sort.
        world.hierarchy_generation++;
        cascade_cached.each<Position, Position*>(propagate);
    });
    walks(bench, count).run("cache order rebuild alone", [&] {
        cascade_cached.cache->order_dirty = true;
        QUERY_SCAN::ensure_order(cascade_cached.cache);
    });

    cascade_cached.free();
    cascade_cached_builder.free();
    up_cached.free();
    up_cached_builder.free();
    cascade.free();
    cascade_builder.free();
    up.free();
    up_builder.free();
    entities.free();
    world.free();
}
