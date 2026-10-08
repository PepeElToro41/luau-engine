#include "support/bench.hpp"

#include "engine/ecs/query/dynamic_query.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/query/query_builder.hpp"
#include "engine/ecs/world.hpp"

// The query VM works per archetype: SELECT, the matcher, variable bindings
// and traversal all read an archetype's type once and every row of the
// chunk shares the answer. So every case here spreads the entities over
// many archetypes with a single row each and reports the cost per
// archetype visited (or per chunk yielded, when an archetype yields
// several), next to the trivial scan where the shape allows it.

struct Health2 {
    i32 value = 0;
};

// One iteration here walks every archetype of the world, so the shared
// bench's thousand iterations per epoch would take minutes. A few whole
// walks per epoch are plenty; `batch` turns the time into a per-archetype
// (or per-chunk) figure.
static ankerl::nanobench::Bench walks(const ankerl::nanobench::Bench& bench, const usz batch) {
    ankerl::nanobench::Bench local = bench;
    local.warmup(2).minEpochIterations(20).batch(batch);
    return local;
}

// `count` entities, each in its own archetype: Position + Velocity + a
// (Likes, target) pair with a target of its own. `targets[i]` receives the
// target of entity i so the pairs can be found again.
static void populate_archetypes(World& world, const usz count, EntityId* targets) {
    for (usz i = 0; i < count; i++) {
        targets[i] = world.new_entity();
        const EntityId e = world.new_entity();
        world.set<Position>(e, { static_cast<f32>(i), 0 });
        world.set<Velocity>(e, { 1, 1 });
        world.add<Likes>(e, targets[i]);
    }
}

BENCH_CASE("ecs/dynamic_query: plain terms per archetype, VM against the scan") {
    constexpr usz count = 10000;

    World world;
    world.init();
    DynamicArray<EntityId> targets;
    targets.resize(count);
    populate_archetypes(world, count, targets.data);

    Query<Position, Velocity> scan = world.query<Position, Velocity>();
    Query<Position, Velocity> scan_cached = world.query<Position, Velocity>(QUERY_CACHED);
    QueryBuilder builder = world.query_build();
    builder.term<Position>().term<Velocity>();
    DynamicQuery vm = builder.build();
    QueryBuilder builder_cached = world.query_build(QUERY_CACHED);
    builder_cached.term<Position>().term<Velocity>();
    DynamicQuery vm_cached = builder_cached.build();

    walks(bench, count).run("Query<>::each, 1 row per archetype", [&] {
        scan.each([](Position& position, const Velocity& velocity) { position.x += velocity.dx; });
    });
    walks(bench, count).run("Query<>::each, cached", [&] {
        scan_cached.each([](Position& position, const Velocity& velocity) { position.x += velocity.dx; });
    });
    walks(bench, count).run("DynamicQuery::each, 1 row per archetype", [&] {
        vm.each<Position, Velocity>([](EntityId, Position& position, Velocity& velocity) { position.x += velocity.dx; });
    });
    walks(bench, count).run("DynamicQuery::each, cached", [&] {
        vm_cached.each<Position, Velocity>([](EntityId, Position& position, Velocity& velocity) { position.x += velocity.dx; });
    });
    walks(bench, count).run("DynamicQuery::count", [&] {
        ankerl::nanobench::doNotOptimizeAway(vm.count());
    });
    walks(bench, count).run("DynamicQuery::count, cached", [&] {
        ankerl::nanobench::doNotOptimizeAway(vm_cached.count());
    });
    walks(bench, 1).run("DynamicQuery cache build + cleanup", [&] {
        vm_cached.ensure_cache();
        vm_cached.cleanup();
    });

    scan_cached.cleanup();
    vm.free();
    vm_cached.free();
    builder.free();
    builder_cached.free();
    targets.free();
    world.free();
}

BENCH_CASE("ecs/dynamic_query: matcher rejections per candidate archetype") {
    constexpr usz count = 10000;

    World world;
    world.init();
    DynamicArray<EntityId> targets;
    targets.resize(count);
    populate_archetypes(world, count, targets.data);
    // Half the archetypes get TagB, so a without<TagB>() query walks every
    // Position candidate and the matcher rejects every other one.
    for (usz i = 0; i < count; i += 2) {
        world.add<TagB>(targets[i] + 1);
    }

    QueryBuilder builder = world.query_build();
    builder.term<Position>().without<TagB>();
    DynamicQuery half = builder.build();
    QueryBuilder builder2 = world.query_build();
    builder2.term<Position>().with<Health2>();
    DynamicQuery none = builder2.build();

    walks(bench, count).run("without<TagB>, half rejected, per candidate", [&] {
        half.each<Position>([](EntityId, Position& position) { position.x += 1; });
    });
    walks(bench, 1).run("with<Health2>, no archetype holds it (no candidates)", [&] {
        ankerl::nanobench::doNotOptimizeAway(none.count());
    });

    half.free();
    none.free();
    builder.free();
    builder2.free();
    targets.free();
    world.free();
}

BENCH_CASE("ecs/dynamic_query: variable binding per chunk") {
    constexpr usz count = 10000;
    constexpr usz pairs_per_entity = 4;

    World world;
    world.init();
    DynamicArray<EntityId> targets;
    targets.resize(count);
    populate_archetypes(world, count, targets.data);
    for (usz i = 0; i < count; i++) {
        world.set<Health2>(targets[i], { static_cast<i32>(i) });
    }

    QueryBuilder builder = world.query_build();
    const QueryVar food = builder.var("food");
    builder.term<Position>().with<Likes>(food);
    DynamicQuery bind = builder.build();
    QueryBuilder builder_cached = world.query_build(QUERY_CACHED);
    const QueryVar food_cached = builder_cached.var("food");
    builder_cached.term<Position>().with<Likes>(food_cached);
    DynamicQuery bind_cached = builder_cached.build();

    QueryBuilder builder2 = world.query_build();
    const QueryVar food2 = builder2.var("food");
    builder2.term<Position>().term<Health2>().src(food2).with<Likes>(food2);
    DynamicQuery source = builder2.build();

    walks(bench, count).run("bind $food, 1 pair per archetype", [&] {
        bind.each<Position>([](EntityId, Position& position) { position.x += 1; });
    });
    walks(bench, count).run("bind $food, 1 pair per archetype, cached", [&] {
        bind_cached.each<Position>([](EntityId, Position& position) { position.x += 1; });
    });
    walks(bench, count).run("bind $food, read Health from $food, 1 pair per archetype", [&] {
        source.each<Position, Health2>([](EntityId, Position& position, const Health2& health) { position.x += static_cast<f32>(health.value); });
    });

    // Every entity likes three more targets: four chunks per archetype.
    for (usz i = 0; i < count; i++) {
        for (usz p = 1; p < pairs_per_entity; p++) {
            world.add<Likes>(targets[i] + 1, targets[(i + p) % count]);
        }
    }
    walks(bench, count * pairs_per_entity).run("bind $food, 4 pairs per archetype, per chunk", [&] {
        bind.each<Position>([](EntityId, Position& position) { position.x += 1; });
    });
    walks(bench, count * pairs_per_entity).run("bind $food, read Health from $food, 4 pairs, per chunk", [&] {
        source.each<Position, Health2>([](EntityId, Position& position, const Health2& health) { position.x += static_cast<f32>(health.value); });
    });
    walks(bench, 1).run("matches(entity), bind $food, read Health from $food", [&] {
        ankerl::nanobench::doNotOptimizeAway(source.matches(targets[0] + 1));
    });

    bind.free();
    bind_cached.free();
    source.free();
    builder.free();
    builder_cached.free();
    builder2.free();
    targets.free();
    world.free();
}

BENCH_CASE("ecs/dynamic_query: up(CHILD_OF) per archetype") {
    constexpr usz count = 10000;

    World world;
    world.init();
    // Each child is alone in its archetype (its own parent), parents hold
    // Position, and grandparents three levels up hold Velocity.
    DynamicArray<EntityId> children;
    children.resize(count);
    for (usz i = 0; i < count; i++) {
        const EntityId top = world.new_entity();
        world.set<Velocity>(top, { 1, 1 });
        const EntityId middle = world.new_entity();
        world.add(middle, ECS::PAIR(ECS::CHILD_OF, top));
        const EntityId parent = world.new_entity();
        world.add(parent, ECS::PAIR(ECS::CHILD_OF, middle));
        world.set<Position>(parent, { 1, 1 });
        children[i] = world.new_entity();
        world.add(children[i], ECS::PAIR(ECS::CHILD_OF, parent));
        world.set<Health2>(children[i], { 0 });
    }

    QueryBuilder builder = world.query_build();
    builder.term<Health2>().term<Position>().up();
    DynamicQuery one_level = builder.build();
    QueryBuilder builder2 = world.query_build();
    builder2.term<Health2>().term<Velocity>().up();
    DynamicQuery three_levels = builder2.build();
    QueryBuilder builder3 = world.query_build();
    builder3.term<Health2>().without<TagA>().up();
    DynamicQuery absent = builder3.build();

    walks(bench, count).run("term<Position>().up(), 1 level", [&] {
        one_level.each<Health2, Position>([](EntityId, Health2& health, const Position& position) { health.value += static_cast<i32>(position.x); });
    });
    walks(bench, count).run("term<Velocity>().up(), 3 levels", [&] {
        three_levels.each<Health2, Velocity>([](EntityId, Health2& health, const Velocity& velocity) { health.value += static_cast<i32>(velocity.dx); });
    });
    walks(bench, count).run("without<TagA>().up(), walks to the top, 3 levels", [&] {
        absent.each<Health2>([](EntityId, Health2& health) { health.value += 1; });
    });

    one_level.free();
    three_levels.free();
    absent.free();
    builder.free();
    builder2.free();
    builder3.free();
    children.free();
    world.free();
}
