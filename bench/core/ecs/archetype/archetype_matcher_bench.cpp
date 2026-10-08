#include "support/bench.hpp"

#include "engine/ecs/archetype/archetype_matcher.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/world.hpp"

// Baseline for queries: matching a with/without filter against every
// archetype in a world with 255 distinct archetypes (every non-empty subset
// of 8 tags).
BENCH_CASE("ecs/archetype_matcher: matches over 255 archetypes") {
    World world;
    world.init();

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

    const Id with[] = {tags[0], tags[1]};
    const Id without[] = {tags[7]};
    ArchetypeMatcher matcher = ArchetypeMatcher::create(world.allocator, with, 2, without, 1);

    const usz archetype_count = world.archetypes.alive_count;
    bench.batch(archetype_count).run("with 2, without 1", [&] {
        usz hits = 0;
        for (usz i = 0; i < archetype_count; ++i) {
            const Archetype* archetype = world.archetypes.get_element_alive(world.archetypes.get_alive_id(i));
            hits += matcher.matches(archetype) ? 1 : 0;
        }
        ankerl::nanobench::doNotOptimizeAway(hits);
    });

    const Id wildcard_with[] = {ECS::PAIR(ECS::CHILD_OF, ECS::WILDCARD)};
    ArchetypeMatcher wildcard = ArchetypeMatcher::create(world.allocator, wildcard_with, 1, nullptr, 0);
    bench.batch(archetype_count).run("with (CHILD_OF, *)", [&] {
        usz hits = 0;
        for (usz i = 0; i < archetype_count; ++i) {
            const Archetype* archetype = world.archetypes.get_element_alive(world.archetypes.get_alive_id(i));
            hits += wildcard.matches(archetype) ? 1 : 0;
        }
        ankerl::nanobench::doNotOptimizeAway(hits);
    });

    wildcard.free();
    matcher.free();
    world.free();
}
