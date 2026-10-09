#include "support/bench.hpp"

#include "engine/ecs/world.hpp"

// Cost of World::cleanup: the scan over every archetype slot when nothing is
// reclaimable, and destroying one empty table per iteration (which the next
// iteration rebuilds, so that run is the full destroy + create round trip).
BENCH_CASE("ecs/world: cleanup") {
    World world;
    world.init();

    // 255 populated archetypes (every non-empty subset of 8 tags), so the
    // scan has something to walk past.
    constexpr usz tag_count = 8;
    EntityId tags[tag_count];
    for (usz i = 0; i < tag_count; ++i) {
        tags[i] = world.new_entity();
    }
    for (usz mask = 1; mask < (1u << tag_count); ++mask) {
        const EntityId e = world.new_entity();
        for (usz bit = 0; bit < tag_count; ++bit) {
            if (mask & (1u << bit)) {
                world.add(e, tags[bit]);
            }
        }
    }
    world.cleanup();

    bench.run("nothing to reclaim, 255+ archetypes", [&] { ankerl::nanobench::doNotOptimizeAway(world.cleanup()); });

    const EntityId churn = world.new_entity();
    bench.run("empty + destroy + rebuild one {Position} table", [&] {
        world.set<Position>(churn, { 1, 2 });
        world.remove<Position>(churn);
        ankerl::nanobench::doNotOptimizeAway(world.cleanup());
    });

    world.free();
}
