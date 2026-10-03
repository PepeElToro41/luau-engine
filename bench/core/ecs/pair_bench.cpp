#include "support/bench.hpp"

#include "engine/ecs/ecs.hpp"
#include "engine/ecs/world.hpp"

BENCH_CASE("ecs/pair: add + remove") {
    World world;
    world.init();
    const EntityId e = world.new_entity();
    const EntityId target = world.new_entity();

    bench.run("tag pair (Likes, target)", [&] {
        world.add<Likes>(e, target);
        world.remove<Likes>(e, target);
    });
    bench.run("data pair (Position, target)", [&] {
        world.set<Position>(e, target, {1, 2});
        world.remove<Position>(e, target);
    });
    bench.run("exclusive (CHILD_OF, target)", [&] {
        world.add(e, ECS::PAIR(ECS::CHILD_OF, target));
        world.remove(e, ECS::PAIR(ECS::CHILD_OF, target));
    });
    world.free();
}

BENCH_CASE("ecs/pair: reparent (exclusive swap)") {
    World world;
    world.init();
    const EntityId e = world.new_entity();
    const EntityId a = world.new_entity();
    const EntityId b = world.new_entity();
    world.add(e, ECS::PAIR(ECS::CHILD_OF, a));
    bool to_b = true;

    bench.run("CHILD_OF a -> b -> a", [&] {
        world.add(e, ECS::PAIR(ECS::CHILD_OF, to_b ? b : a));
        to_b = !to_b;
    });
    world.free();
}

BENCH_CASE("ecs/pair: CHILD_OF cascade delete") {
    World world;
    world.init();
    constexpr usz child_count = 64;

    bench.run("create parent + 64 children, delete parent", [&] {
        const EntityId parent = world.new_entity();
        for (usz i = 0; i < child_count; ++i) {
            const EntityId child = world.new_entity();
            world.add(child, ECS::PAIR(ECS::CHILD_OF, parent));
        }
        world.delete_entity(parent);
    });
    bench.run("create parent + 64 children, no delete (baseline)", [&] {
        const EntityId parent = world.new_entity();
        for (usz i = 0; i < child_count; ++i) {
            const EntityId child = world.new_entity();
            world.add(child, ECS::PAIR(ECS::CHILD_OF, parent));
        }
    });
    world.free();
}
