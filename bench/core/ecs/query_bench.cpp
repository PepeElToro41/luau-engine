#include "support/bench.hpp"

#include "engine/ecs/ecs.hpp"
#include "engine/ecs/monitor.hpp"
#include "engine/ecs/observer.hpp"
#include "engine/ecs/world.hpp"

static void noop_monitor(World* world, EntityId entity, MonitorEvent event, void* user_data) {
    (void)world;
    (void)entity;
    (void)event;
    (void)user_data;
}

static void noop_observer(World* world, EntityId entity, ObserverEvent event, Id id, void* user_data) {
    (void)world;
    (void)entity;
    (void)event;
    (void)id;
    (void)user_data;
}

// Spreads `count` entities over 2^tag_bits archetypes: entity i gets
// Position + Velocity plus the subset of the four tags its low bits select.
static void populate(World& world, const usz count, const u32 tag_bits) {
    for (usz i = 0; i < count; i++) {
        const EntityId e = world.new_entity();
        world.set<Position>(e, { static_cast<f32>(i), 0 });
        world.set<Velocity>(e, { 1, 1 });
        const usz bits = tag_bits == 0 ? 0 : i & ((1u << tag_bits) - 1);
        if (bits & 1) world.add<TagA>(e);
        if (bits & 2) world.add<TagB>(e);
        if (bits & 4) world.add<Likes>(e);
        if (bits & 8) world.set<Health>(e, { 1 });
    }
}

// Per-entity cost of each() over a single archetype and over many, next to
// the bare loop it has to compete with.
BENCH_CASE("ecs/query: each over N entities") {
    constexpr usz count = 100000;

    {
        World world;
        world.init();
        populate(world, count, 0);
        Query<Position, Velocity> movers = world.query<Position, Velocity>();

        bench.batch(count).run("each, 1 archetype", [&] {
            movers.each([](Position& position, const Velocity& velocity) {
                position.x += velocity.dx;
            });
        });
        bench.batch(count).run("iter, 1 archetype", [&] {
            movers.iter([](const QueryIter& it, Position* positions, Velocity* velocities) {
                for (usz row = 0; row < it.count; row++) {
                    positions[row].x += velocities[row].dx;
                }
            });
        });
        world.free();
    }

    {
        World world;
        world.init();
        populate(world, count, 4);
        Query<Position, Velocity> movers = world.query<Position, Velocity>();
        Query<Position, Velocity> tagged = world.query<Position, Velocity>().with<TagA>().without<TagB>();

        bench.batch(count).run("each, 16 archetypes", [&] {
            movers.each([](Position& position, const Velocity& velocity) {
                position.x += velocity.dx;
            });
        });
        // A quarter of the entities match; cost is per entity visited.
        bench.batch(count / 4).run("each, 16 archetypes, with + without", [&] {
            tagged.each([](Position& position, const Velocity& velocity) {
                position.x += velocity.dx;
            });
        });
        world.free();
    }
}

// The fixed cost of a run: building the matcher and scanning the archetype
// list, with no rows to visit. One empty table per tag subset.
BENCH_CASE("ecs/query: scan overhead") {
    World world;
    world.init();
    populate(world, 16, 4);
    for (usz i = 0; i < world.entity_index.count(); i++) {
        // Empty every archetype so only the scan itself is measured.
        const EntityId e = world.entity_index.get_alive_id(i);
        if (e > ECS::REST) {
            world.clear(e);
        }
    }
    Query<Position> query = world.query<Position>().with<TagA>();

    bench.run("count over 16 empty archetypes", [&] {
        ankerl::nanobench::doNotOptimizeAway(query.count());
    });
    const EntityId e = world.new_entity();
    world.set<Position>(e, { 0, 0 });
    bench.run("matches(entity)", [&] {
        ankerl::nanobench::doNotOptimizeAway(query.matches(e));
    });

    world.free();
}

// A rare term next to a common one over many tables: the walk starts from
// the rare term's record, so the common term's tables are never tested.
// Compared against the same query with only the common term, which has to
// visit every table.
BENCH_CASE("ecs/query: scan narrowed by the rarest term") {
    World world;
    world.init();
    // 256 tables of Position + some subset of 8 tag-like ids, one row each,
    // and Health on a single one of them.
    const EntityId tags[8] = {
        world.new_entity(), world.new_entity(), world.new_entity(), world.new_entity(),
        world.new_entity(), world.new_entity(), world.new_entity(), world.new_entity(),
    };
    for (usz i = 0; i < 256; i++) {
        const EntityId e = world.new_entity();
        world.set<Position>(e, { 0, 0 });
        for (usz bit = 0; bit < 8; bit++) {
            if (i & (1u << bit)) {
                world.add(e, tags[bit]);
            }
        }
        if (i == 200) {
            world.set<Health>(e, { 1 });
        }
    }

    Query<Position> common = world.query<Position>();
    Query<Position> rare = world.query<Position>().with<Health>();

    bench.run("count, Position over 256 tables", [&] {
        ankerl::nanobench::doNotOptimizeAway(common.count());
    });
    bench.run("count, Position + Health (1 table)", [&] {
        ankerl::nanobench::doNotOptimizeAway(rare.count());
    });

    // Monitors tag their archetypes at creation and untag them at destroy
    // through the same narrowing.
    bench.run("monitor + unmonitor, Position over 256 tables", [&] {
        world.unmonitor(common.monitor(noop_monitor));
    });
    bench.run("monitor + unmonitor, Position + Health (1 table)", [&] {
        world.unmonitor(rare.monitor(noop_monitor));
    });

    world.free();
}

// What monitors cost the operations that move entities, whether or not the
// move changes membership.
BENCH_CASE("ecs/monitor: add + remove with monitors watching") {
    World world;
    world.init();
    const EntityId e = world.new_entity();
    world.set<Position>(e, { 0, 0 });

    bench.run("no monitors", [&] {
        world.add<TagA>(e);
        world.remove<TagA>(e);
    });

    // Membership changes on every add / remove: two callbacks per iteration.
    const ObserverId firing = world.query<Position>().with<TagA>().monitor(noop_monitor);
    bench.run("1 monitor, enter + leave each time", [&] {
        world.add<TagA>(e);
        world.remove<TagA>(e);
    });
    world.unmonitor(firing);

    // Membership never changes: only the id-list diff is paid.
    const ObserverId silent = world.query<Position>().monitor(noop_monitor);
    bench.run("1 monitor, never fires", [&] {
        world.add<TagA>(e);
        world.remove<TagA>(e);
    });
    world.unmonitor(silent);

    ObserverId many[8];
    for (ObserverId& id : many) {
        id = world.query<Position>().monitor(noop_monitor);
    }
    bench.run("8 monitors, none fire", [&] {
        world.add<TagA>(e);
        world.remove<TagA>(e);
    });
    for (const ObserverId id : many) {
        world.unmonitor(id);
    }

    world.free();
}

// Creating a monitor tests every archetype; creating an archetype tests
// every monitor.
BENCH_CASE("ecs/monitor: registration") {
    World world;
    world.init();
    populate(world, 64, 4);

    bench.run("monitor + unmonitor, 16 archetypes", [&] {
        const ObserverId id = world.query<Position>().with<TagA>().monitor(noop_monitor);
        world.unmonitor(id);
    });

    ObserverId many[8];
    for (ObserverId& id : many) {
        id = world.query<Position>().with<TagA>().monitor(noop_monitor);
    }
    const EntityId e = world.new_entity();
    world.set<Position>(e, { 0, 0 });
    world.set<Health>(e, { 1 });
    bench.run("add + remove a pair, 8 monitors", [&] {
        // (Likes, e) and back: the archetypes exist after the first run, so
        // this measures the steady state, not creation.
        world.add<Likes>(e, e);
        world.remove<Likes>(e, e);
    });
    for (const ObserverId id : many) {
        world.unmonitor(id);
    }

    world.free();
}

// What observers cost set() on a matching entity and the operations that
// move entities, whether or not an event fires.
BENCH_CASE("ecs/observer: set + move with observers watching") {
    World world;
    world.init();
    const EntityId e = world.new_entity();
    world.set<Position>(e, { 0, 0 });
    world.set<Velocity>(e, { 1, 1 });

    bench.run("set, no observers", [&] {
        world.set<Position>(e, { 1, 1 });
    });

    // Position is an output: every set() fires CHANGED.
    const ObserverId changed = world.query<Position>().observe(noop_observer);
    bench.run("set, 1 observer fires CHANGED", [&] {
        world.set<Position>(e, { 1, 1 });
    });
    world.unobserve(changed);

    // Position is a with() term: the term entry is found but not an output.
    const ObserverId with_only = world.query<Velocity>().with<Position>().observe(noop_observer);
    bench.run("set, 1 observer with Position as with(), never fires", [&] {
        world.set<Position>(e, { 1, 1 });
    });
    world.unobserve(with_only);

    ObserverId many[8];
    for (ObserverId& id : many) {
        id = world.query<Velocity>().observe(noop_observer);
    }
    bench.run("set, 8 observers on another output, none fire", [&] {
        world.set<Position>(e, { 1, 1 });
    });
    for (const ObserverId id : many) {
        world.unobserve(id);
    }

    bench.run("add + remove, no observers", [&] {
        world.add<TagA>(e);
        world.remove<TagA>(e);
    });

    // The add makes the entity match: one MOVED per iteration.
    const ObserverId moving = world.query<Position>().with<TagA>().observe(noop_observer);
    bench.run("add + remove, 1 observer fires MOVED on the add", [&] {
        world.add<TagA>(e);
        world.remove<TagA>(e);
    });
    world.unobserve(moving);

    // TagA is not a term: only the list walks are paid.
    const ObserverId silent = world.query<Position>().observe(noop_observer);
    bench.run("add + remove, 1 observer, never fires", [&] {
        world.add<TagA>(e);
        world.remove<TagA>(e);
    });
    world.unobserve(silent);

    for (ObserverId& id : many) {
        id = world.query<Position>().observe(noop_observer);
    }
    bench.run("add + remove, 8 observers, none fire", [&] {
        world.add<TagA>(e);
        world.remove<TagA>(e);
    });
    for (const ObserverId id : many) {
        world.unobserve(id);
    }

    world.free();
}

// Creating an observer tests every candidate archetype and lists its term
// entries; creating an archetype tests every observer under its key.
BENCH_CASE("ecs/observer: registration") {
    World world;
    world.init();
    populate(world, 64, 4);

    bench.run("observe + unobserve, 16 archetypes", [&] {
        const ObserverId id = world.query<Position>().with<TagA>().observe(noop_observer);
        world.unobserve(id);
    });

    ObserverId many[8];
    for (ObserverId& id : many) {
        id = world.query<Position>().with<TagA>().observe(noop_observer);
    }
    const EntityId e = world.new_entity();
    world.set<Position>(e, { 0, 0 });
    world.set<Health>(e, { 1 });
    bench.run("add + remove a pair, 8 observers", [&] {
        // (Likes, e) and back: the archetypes exist after the first run, so
        // this measures the steady state, not creation.
        world.add<Likes>(e, e);
        world.remove<Likes>(e, e);
    });
    for (const ObserverId id : many) {
        world.unobserve(id);
    }

    world.free();
}
