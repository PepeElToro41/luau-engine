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

BENCH_CASE("ecs/hierarchy: depth lookup, exclusive relation against a non-exclusive one") {
    World world;
    world.init();

    // CHILD_OF is exclusive; `shared` is traversable only, so its pair loop
    // cannot stop at the first pair without checking the next id. Both get
    // a chain four deep, and the entity at the bottom either holds nothing
    // else or four pairs of unrelated relations. Pairs sort by relation id,
    // so where those land matters: `lower` relations (created before
    // `shared`) sort before the chain's pair and the loop ends at the end
    // of the type; `unrelated` ones (created after it, like every user
    // relation against CHILD_OF) sort right after it, and the loop has to
    // load one of them to see it is another relation.
    EntityId lower[4];
    for (usz i = 0; i < 4; i++) {
        lower[i] = world.new_entity();
    }
    const EntityId shared = world.new_entity();
    world.add(shared, ECS::TRAVERSABLE);
    EntityId unrelated[4];
    EntityId targets[4];
    for (usz i = 0; i < 4; i++) {
        unrelated[i] = world.new_entity();
        targets[i] = world.new_entity();
    }

    auto chain = [&](const Id relation, const bool with_unrelated, const bool with_lower = false) {
        EntityId parent = world.new_entity();
        for (usz level = 0; level < 3; level++) {
            const EntityId next = world.new_entity();
            world.add(next, world.pair(relation, parent));
            parent = next;
        }
        const EntityId leaf = world.new_entity();
        world.add(leaf, world.pair(relation, parent));
        world.set<Position>(leaf, { 1, 1 });
        if (with_unrelated) {
            for (usz i = 0; i < 4; i++) {
                world.add(leaf, world.pair(unrelated[i], targets[i]));
            }
        }
        if (with_lower) {
            for (usz i = 0; i < 4; i++) {
                world.add(leaf, world.pair(lower[i], targets[i]));
            }
        }
        world.depth(leaf, relation);
        return leaf;
    };

    const EntityId child_of_plain = chain(ECS::CHILD_OF, false);
    const EntityId child_of_pairs = chain(ECS::CHILD_OF, true);
    const EntityId shared_plain = chain(shared, false);
    const EntityId shared_pairs = chain(shared, true);
    const EntityId shared_lower = chain(shared, false, true);

    bench.run("depth, CHILD_OF (exclusive), no other pairs", [&] {
        ankerl::nanobench::doNotOptimizeAway(world.depth(child_of_plain, ECS::CHILD_OF));
    });
    bench.run("depth, CHILD_OF (exclusive), 4 unrelated pairs", [&] {
        ankerl::nanobench::doNotOptimizeAway(world.depth(child_of_pairs, ECS::CHILD_OF));
    });
    bench.run("depth, traversable only, no other pairs", [&] {
        ankerl::nanobench::doNotOptimizeAway(world.depth(shared_plain, shared));
    });
    bench.run("depth, traversable only, 4 unrelated pairs sorting after", [&] {
        ankerl::nanobench::doNotOptimizeAway(world.depth(shared_pairs, shared));
    });
    bench.run("depth, traversable only, 4 unrelated pairs sorting before", [&] {
        ankerl::nanobench::doNotOptimizeAway(world.depth(shared_lower, shared));
    });

    world.free();
}
