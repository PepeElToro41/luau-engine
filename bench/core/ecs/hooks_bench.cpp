#include "support/bench.hpp"

#include "engine/ecs/ecs.hpp"
#include "engine/ecs/world.hpp"

static void noop_hook(World* world, EntityId entity, Id id, void* user_data) {
    (void)world;
    (void)entity;
    (void)id;
    (void)user_data;
}

// How much a registered hook costs the operations that may fire it.
BENCH_CASE("ecs/hooks: set with changed hooks") {
    World world;
    world.init();
    const EntityId e = world.new_entity();
    world.set<Position>(e, {0, 0});
    const Position value {1, 2};

    bench.run("no hooks", [&] { world.set<Position>(e, value); });

    const HookId typed = world.hook_changed<Position>(noop_hook);
    bench.run("1 hook on Position", [&] { world.set<Position>(e, value); });
    world.unhook(world.id<Position>(), typed);

    const HookId any = world.hook_changed(ECS::WILDCARD, noop_hook);
    bench.run("1 wildcard hook", [&] { world.set<Position>(e, value); });
    world.unhook(ECS::WILDCARD, any);

    world.free();
}

BENCH_CASE("ecs/hooks: add + remove with hooks") {
    World world;
    world.init();
    const EntityId e = world.new_entity();

    bench.run("no hooks", [&] {
        world.add<TagA>(e);
        world.remove<TagA>(e);
    });

    const HookId added = world.hook_added<TagA>(noop_hook);
    const HookId removed = world.hook_removed<TagA>(noop_hook);
    bench.run("added + removed hooks on TagA", [&] {
        world.add<TagA>(e);
        world.remove<TagA>(e);
    });
    world.unhook(world.id<TagA>(), added);
    world.unhook(world.id<TagA>(), removed);

    world.free();
}
