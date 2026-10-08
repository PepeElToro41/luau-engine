#include "support/bench.hpp"

#include "engine/ecs/ecs.hpp"
#include "engine/ecs/hierarchy.hpp"
#include "engine/ecs/world.hpp"

// Cached hierarchy depth: the cost of asking, of a move that cannot change
// any depth (the common case: a parent gaining a component, or being
// reparented at the same depth), and of reparenting a whole subtree.

namespace {

// A CHILD_OF tree `depth` levels deep with `fanout` children per node, as a
// chain of entities below `root`. Returns the deepest entity created.
EntityId build_tree(World& world, const EntityId root, const u32 depth, const u32 fanout, DynamicArray<EntityId>& all) {
    EntityId last = root;
    DynamicArray<EntityId> level;
    DynamicArray<EntityId> next;
    level.push(root);
    for (u32 d = 0; d < depth; d++) {
        next.clear();
        for (const EntityId parent : level) {
            for (u32 i = 0; i < fanout; i++) {
                const EntityId child = world.new_entity();
                world.add(child, world.pair(ECS::CHILD_OF, parent));
                next.push(child);
                all.push(child);
                last = child;
            }
        }
        level.clear();
        for (const EntityId child : next) {
            level.push(child);
        }
    }
    level.free();
    next.free();
    return last;
}

} // namespace

BENCH_CASE("ecs/hierarchy: depth lookup") {
    World world;
    world.init();
    DynamicArray<EntityId> all;
    const EntityId root = world.new_entity();
    const EntityId leaf = build_tree(world, root, 8, 2, all);

    bench.run("depth, cached, 8 levels", [&] {
        ankerl::nanobench::doNotOptimizeAway(world.depth(leaf));
    });

    // Every lookup after an invalidation walks the chain once; dirty the
    // whole tree each time by moving the root in and out of a parent.
    const EntityId top = world.new_entity();
    bool under = false;
    bench.run("depth, recompute after root moved, 8 levels", [&] {
        if (under) {
            world.remove(root, world.pair(ECS::CHILD_OF, top));
        } else {
            world.add(root, world.pair(ECS::CHILD_OF, top));
        }
        under = !under;
        ankerl::nanobench::doNotOptimizeAway(world.depth(leaf));
    });

    all.free();
    world.free();
}

BENCH_CASE("ecs/hierarchy: moves of a parent") {
    World world;
    world.init();
    DynamicArray<EntityId> all;
    const EntityId root = world.new_entity();
    build_tree(world, root, 8, 2, all);
    world.depth(all.last());

    // The root is a target (it has children); adding and removing a
    // component to it goes through on_move, which sees the same parents on
    // both sides and stops.
    bench.run("add/remove component on a parent, no depth change", [&] {
        world.add<TagA>(root);
        world.remove<TagA>(root);
    });

    // Two parents at the same depth: swapping between them is detected as a
    // depth-neutral move after computing both depths (cached).
    const EntityId p1 = world.new_entity();
    const EntityId p2 = world.new_entity();
    world.add(root, world.pair(ECS::CHILD_OF, p1));
    bool on_first = true;
    bench.run("reparent under a sibling, no depth change", [&] {
        world.add(root, world.pair(ECS::CHILD_OF, on_first ? p2 : p1));
        on_first = !on_first;
    });

    // Reparenting between depths 1 and 2 dirties every pair record below the
    // root (one per parent entity: 2^8 - 1 here) and the next lookup
    // recomputes the chain.
    const EntityId deep = world.new_entity();
    world.add(deep, world.pair(ECS::CHILD_OF, p2));
    bool is_deep = false;
    bench.run("reparent to another depth, 8 levels x 2 fanout tree", [&] {
        world.add(root, world.pair(ECS::CHILD_OF, is_deep ? p1 : deep));
        is_deep = !is_deep;
        ankerl::nanobench::doNotOptimizeAway(world.depth(all.last()));
    });

    all.free();
    world.free();
}
