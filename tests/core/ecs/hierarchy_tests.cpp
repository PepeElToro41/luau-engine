#include "support/test_support.hpp"

#include "engine/ecs/entity_index.hpp"
#include "engine/ecs/hierarchy.hpp"

// Cached hierarchy depth on the traversable pair records: values, what a
// move of a parent invalidates (and, more importantly, what it does not),
// the node links and target flag, several parents and cycles.

namespace {

// Depth along CHILD_OF of the entity's archetype, straight from the cache
// layer, to check it agrees with World::depth.
u32 archetype_depth(World& world, const EntityId entity) {
    const EntityRecord* record = world.entity_index.get_record_alive(entity);
    REQUIRE(record != nullptr);
    return HIERARCHY::depth(&world, record->archetype, ECS::CHILD_OF);
}

// The (relation, target) record, or nullptr if it was never created.
ComponentRecord* pair_record(World& world, const EntityIdLow relation, const EntityId target) {
    return ComponentRecord::component_record_find(&world, ECS::PAIR(relation, target));
}

// The hierarchy node of (relation, target), or nullptr.
const HierarchyNode* node(World& world, const EntityIdLow relation, const EntityId target) {
    const ComponentRecord* record = pair_record(world, relation, target);
    return record != nullptr ? record->hierarchy : nullptr;
}

// Whether (relation, child) is linked under (relation, parent).
bool linked(World& world, const EntityIdLow relation, const EntityId parent, const EntityId child) {
    const HierarchyNode* parent_node = node(world, relation, parent);
    REQUIRE(parent_node != nullptr);
    return parent_node->children.contains(ECS::PAIR(relation, child));
}

bool is_target(World& world, const EntityId entity) {
    const EntityRecord* record = world.entity_index.get_record_alive(entity);
    REQUIRE(record != nullptr);
    return (record->flags & ENTITY_RECORD_TRAVERSABLE_TARGET) != 0;
}

// A fresh traversable relation, with the trait set before first use.
EntityId make_traversable(World& world) {
    const EntityId relation = world.new_entity();
    world.add(relation, ECS::TRAVERSABLE);
    return relation;
}

} // namespace

TEST_CASE("ecs/hierarchy: depth counts the CHILD_OF chain up to the root") {
    World world;
    world.init();

    const EntityId root = world.new_entity();
    const EntityId child = world.new_entity();
    const EntityId grandchild = world.new_entity();
    world.add(child, world.pair(ECS::CHILD_OF, root));
    world.add(grandchild, world.pair(ECS::CHILD_OF, child));

    CHECK(world.depth(root) == 0);
    CHECK(world.depth(child) == 1);
    CHECK(world.depth(grandchild) == 2);
    CHECK(archetype_depth(world, grandchild) == 2);
    // The pair record carries the depth of its holders.
    CHECK(HIERARCHY::depth(&world, pair_record(world, ECS::CHILD_OF, child)) == 2);

    // Unrelated ids do not change the depth, and a dead entity has none.
    world.set<Position>(grandchild, {1, 2});
    CHECK(world.depth(grandchild) == 2);
    const EntityId dead = world.new_entity();
    REQUIRE(world.delete_entity(dead));
    CHECK(world.depth(dead) == 0);
    CHECK(world.depth(root, 0) == 0);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hierarchy: only traversable pair records carry a node") {
    World world;
    world.init();

    const EntityId parent = world.new_entity();
    const EntityId likes = world.id<Likes>();
    const EntityId e = world.new_entity();

    // A plain component and a non-traversable pair: no nodes, depth 0.
    world.set<Position>(e, {});
    world.add(e, world.pair(likes, parent));
    CHECK(node(world, likes, parent) == nullptr);
    CHECK(world.depth(e, likes) == 0);
    CHECK_FALSE(is_target(world, parent));

    world.add(e, world.pair(ECS::CHILD_OF, parent));
    const HierarchyNode* child_of_parent = node(world, ECS::CHILD_OF, parent);
    REQUIRE(child_of_parent != nullptr);
    CHECK(child_of_parent->dirty);
    CHECK(world.depth(e) == 1);
    CHECK_FALSE(child_of_parent->dirty);
    CHECK(child_of_parent->depth == 1);
    CHECK(is_target(world, parent));

    // IS_A is traversable too and is tracked separately from CHILD_OF.
    const EntityId base = world.new_entity();
    const EntityId base_parent = world.new_entity();
    world.add(base, world.pair(ECS::IS_A, base_parent));
    world.add(e, world.pair(ECS::IS_A, base));
    CHECK(node(world, ECS::IS_A, base) != nullptr);
    CHECK(world.depth(e, ECS::IS_A) == 2);
    CHECK(world.depth(e, ECS::CHILD_OF) == 1);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hierarchy: nodes are linked under the nodes of their target's parents") {
    World world;
    world.init();

    const EntityId a = world.new_entity();
    const EntityId b = world.new_entity();
    const EntityId c = world.new_entity();
    world.add(b, world.pair(ECS::CHILD_OF, a));
    // Only (CHILD_OF, a) exists so far, with no children: b has none yet.
    REQUIRE(node(world, ECS::CHILD_OF, a) != nullptr);
    CHECK(node(world, ECS::CHILD_OF, a)->children.is_empty());
    CHECK(node(world, ECS::CHILD_OF, b) == nullptr);

    // c under b creates (CHILD_OF, b) and hangs it under (CHILD_OF, a),
    // since b holds (CHILD_OF, a).
    world.add(c, world.pair(ECS::CHILD_OF, b));
    CHECK(linked(world, ECS::CHILD_OF, a, b));
    CHECK(node(world, ECS::CHILD_OF, b)->children.is_empty());

    SUBCASE("reparenting b re-links its node") {
        const EntityId a2 = world.new_entity();
        world.add(b, world.pair(ECS::CHILD_OF, a2));
        CHECK_FALSE(linked(world, ECS::CHILD_OF, a, b));
        CHECK(linked(world, ECS::CHILD_OF, a2, b));
        world.remove(b, world.pair(ECS::CHILD_OF, a2));
        CHECK_FALSE(linked(world, ECS::CHILD_OF, a2, b));
        world.add(b, world.pair(ECS::CHILD_OF, a));
        CHECK(linked(world, ECS::CHILD_OF, a, b));
    }
    SUBCASE("unrelated moves of b keep the link") {
        world.set<Position>(b, {});
        world.add<TagA>(b);
        CHECK(linked(world, ECS::CHILD_OF, a, b));
    }
    SUBCASE("the node lives as long as the record, which outlives its holders") {
        // Deleting c empties (CHILD_OF, b) but keeps the record, so b stays
        // a target and its node stays linked, ready for the next child.
        REQUIRE(world.delete_entity(c));
        CHECK(node(world, ECS::CHILD_OF, b) != nullptr);
        CHECK(linked(world, ECS::CHILD_OF, a, b));
        CHECK(is_target(world, b));
        // Deleting b itself deletes the record: node freed and unlinked.
        REQUIRE(world.delete_entity(b));
        CHECK(pair_record(world, ECS::CHILD_OF, b) == nullptr);
        CHECK(node(world, ECS::CHILD_OF, a)->children.is_empty());
        CHECK(is_target(world, a));
    }
    SUBCASE("deleting a cascades through the whole subtree") {
        REQUIRE(world.delete_entity(a));
        CHECK_FALSE(world.alive(b));
        CHECK_FALSE(world.alive(c));
        CHECK(pair_record(world, ECS::CHILD_OF, a) == nullptr);
        CHECK(pair_record(world, ECS::CHILD_OF, b) == nullptr);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hierarchy: reparenting a subtree recomputes every level below it") {
    World world;
    world.init();

    const EntityId a = world.new_entity();
    const EntityId b = world.new_entity();
    const EntityId c = world.new_entity();
    const EntityId d = world.new_entity();
    world.add(b, world.pair(ECS::CHILD_OF, a));
    world.add(c, world.pair(ECS::CHILD_OF, b));
    world.add(d, world.pair(ECS::CHILD_OF, c));
    REQUIRE(world.depth(d) == 3);

    SUBCASE("a middle node becomes a root") {
        const u64 before = world.hierarchy_generation;
        world.remove(b, world.pair(ECS::CHILD_OF, a));
        CHECK(world.hierarchy_generation > before);
        CHECK(world.depth(b) == 0);
        CHECK(world.depth(c) == 1);
        CHECK(world.depth(d) == 2);
    }
    SUBCASE("a root gains a parent") {
        const EntityId top = world.new_entity();
        world.add(a, world.pair(ECS::CHILD_OF, top));
        CHECK(world.depth(a) == 1);
        CHECK(world.depth(b) == 2);
        CHECK(world.depth(c) == 3);
        CHECK(world.depth(d) == 4);
    }
    SUBCASE("an exclusive swap moves the subtree deeper") {
        const EntityId x = world.new_entity();
        const EntityId y = world.new_entity();
        world.add(x, world.pair(ECS::CHILD_OF, b));
        world.add(y, world.pair(ECS::CHILD_OF, x));
        world.add(c, world.pair(ECS::CHILD_OF, y));
        CHECK(world.depth(c) == 4);
        CHECK(world.depth(d) == 5);
    }
    SUBCASE("clearing a parent resets its subtree") {
        world.clear(b);
        CHECK(world.depth(b) == 0);
        CHECK(world.depth(c) == 1);
        CHECK(world.depth(d) == 2);
    }
    SUBCASE("children spread over several archetypes share the one record") {
        // Two more children of b in two other archetypes: one invalidation
        // of (CHILD_OF, b) covers all three.
        const EntityId c2 = world.new_entity();
        const EntityId c3 = world.new_entity();
        world.set<Position>(c2, {});
        world.add(c2, world.pair(ECS::CHILD_OF, b));
        world.set<Velocity>(c3, {});
        world.add(c3, world.pair(ECS::CHILD_OF, b));
        REQUIRE(world.depth(c2) == 2);
        REQUIRE(world.depth(c3) == 2);
        const u64 before = world.hierarchy_generation;
        world.remove(b, world.pair(ECS::CHILD_OF, a));
        // (CHILD_OF, b) and (CHILD_OF, c): one bump each.
        CHECK(world.hierarchy_generation == before + 2);
        CHECK(world.depth(c) == 1);
        CHECK(world.depth(c2) == 1);
        CHECK(world.depth(c3) == 1);
        CHECK(world.depth(d) == 2);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hierarchy: moves that cannot change a depth invalidate nothing") {
    World world;
    world.init();

    const EntityId p1 = world.new_entity();
    const EntityId p2 = world.new_entity();
    const EntityId mid = world.new_entity();
    const EntityId leaf = world.new_entity();
    world.add(mid, world.pair(ECS::CHILD_OF, p1));
    world.add(leaf, world.pair(ECS::CHILD_OF, mid));
    REQUIRE(world.depth(leaf) == 2);
    const HierarchyNode* mid_node = node(world, ECS::CHILD_OF, mid);
    REQUIRE(mid_node != nullptr);
    REQUIRE_FALSE(mid_node->dirty);

    SUBCASE("the parent gains an unrelated component") {
        const u64 before = world.hierarchy_generation;
        world.set<Position>(mid, {1, 1});
        world.add<TagA>(mid);
        world.remove<TagA>(mid);
        CHECK(world.hierarchy_generation == before);
        CHECK_FALSE(mid_node->dirty);
        CHECK(world.depth(leaf) == 2);
    }
    SUBCASE("the parent is reparented under a sibling at the same depth") {
        const u64 before = world.hierarchy_generation;
        world.add(mid, world.pair(ECS::CHILD_OF, p2));
        REQUIRE(world.has(mid, world.pair(ECS::CHILD_OF, p2)));
        CHECK(world.hierarchy_generation == before);
        CHECK_FALSE(mid_node->dirty);
        CHECK(linked(world, ECS::CHILD_OF, p2, mid));
        CHECK(world.depth(leaf) == 2);
    }
    SUBCASE("a leaf nothing points at moves") {
        const u64 before = world.hierarchy_generation;
        world.remove(leaf, world.pair(ECS::CHILD_OF, mid));
        world.add(leaf, world.pair(ECS::CHILD_OF, p2));
        CHECK(world.hierarchy_generation == before);
        CHECK(world.depth(leaf) == 1);
    }
    SUBCASE("the parent's move changes depth: one bump per record below") {
        const EntityId deeper = world.new_entity();
        world.add(deeper, world.pair(ECS::CHILD_OF, p2));
        const EntityId leaf2 = world.new_entity();
        world.add(leaf2, world.pair(ECS::CHILD_OF, mid));
        const u64 before = world.hierarchy_generation;
        world.add(mid, world.pair(ECS::CHILD_OF, deeper));
        CHECK(world.hierarchy_generation == before + 1);
        CHECK(mid_node->dirty);
        CHECK(world.depth(leaf) == 3);
        CHECK(world.depth(leaf2) == 3);
        CHECK_FALSE(mid_node->dirty);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hierarchy: a dirty node is not propagated through twice") {
    World world;
    world.init();

    const EntityId a = world.new_entity();
    const EntityId b = world.new_entity();
    const EntityId c = world.new_entity();
    world.add(b, world.pair(ECS::CHILD_OF, a));
    world.add(c, world.pair(ECS::CHILD_OF, b));
    REQUIRE(world.depth(c) == 2);

    // Two moves of `a` before anybody asks again: the second finds
    // (CHILD_OF, a) already dirty and stops there.
    const EntityId top = world.new_entity();
    const EntityId top2 = world.new_entity();
    world.add(top2, world.pair(ECS::CHILD_OF, top));
    const u64 before = world.hierarchy_generation;
    world.add(a, world.pair(ECS::CHILD_OF, top));
    CHECK(world.hierarchy_generation == before + 2); // (CHILD_OF, a) and (CHILD_OF, b)
    world.add(a, world.pair(ECS::CHILD_OF, top2));
    CHECK(world.hierarchy_generation == before + 2);
    CHECK(world.depth(a) == 2);
    CHECK(world.depth(b) == 3);
    CHECK(world.depth(c) == 4);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hierarchy: the target flag follows the traversable records") {
    World world;
    world.init();

    const EntityId parent = world.new_entity();
    const EntityId child = world.new_entity();
    world.set<Position>(parent, {});
    CHECK_FALSE(is_target(world, parent));

    world.add(child, world.pair(ECS::CHILD_OF, parent));
    CHECK(is_target(world, parent));
    CHECK_FALSE(is_target(world, child));

    // A non-traversable relation does not make a target.
    const EntityId likes = world.id<Likes>();
    const EntityId liked = world.new_entity();
    world.add(child, world.pair(likes, liked));
    CHECK_FALSE(is_target(world, liked));

    // The flag survives the parent's own moves (the record stays).
    world.clear(parent);
    CHECK(is_target(world, parent));
    world.set<Position>(parent, {});
    CHECK(is_target(world, parent));

    // A target through two traversable relations stays one until both
    // records are gone.
    const EntityId relation = make_traversable(world);
    const EntityId target = world.new_entity();
    const EntityId holder = world.new_entity();
    world.add(holder, world.pair(relation, target));
    world.add(holder, world.pair(ECS::IS_A, target));
    CHECK(is_target(world, target));
    CHECK(world.depth(holder, relation) == 1);
    CHECK(world.depth(holder, ECS::IS_A) == 1);
    // Deleting a holder's row does not delete records; deleting the target
    // does, under the REMOVE policy of both relations: the holder loses the
    // pairs, the records go, and the flag is cleared before the entity dies.
    REQUIRE(world.delete_entity(target));
    CHECK(world.alive(holder));
    CHECK(world.depth(holder, relation) == 0);
    CHECK(pair_record(world, relation, target) == nullptr);
    CHECK(pair_record(world, ECS::IS_A, target) == nullptr);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hierarchy: several parents through one relation take the deepest") {
    World world;
    world.init();

    // IS_A is traversable but not exclusive, so an entity may have two bases.
    const EntityId shallow = world.new_entity();
    const EntityId deep_root = world.new_entity();
    const EntityId deep = world.new_entity();
    world.add(deep, world.pair(ECS::IS_A, deep_root));

    const EntityId e = world.new_entity();
    world.add(e, world.pair(ECS::IS_A, shallow));
    CHECK(world.depth(e, ECS::IS_A) == 1);
    world.add(e, world.pair(ECS::IS_A, deep));
    REQUIRE(world.has(e, world.pair(ECS::IS_A, shallow)));
    REQUIRE(world.has(e, world.pair(ECS::IS_A, deep)));
    CHECK(world.depth(e, ECS::IS_A) == 2);

    // Something below e makes (IS_A, e) a node linked under both bases.
    const EntityId derived = world.new_entity();
    world.add(derived, world.pair(ECS::IS_A, e));
    CHECK(linked(world, ECS::IS_A, shallow, e));
    CHECK(linked(world, ECS::IS_A, deep, e));
    CHECK(world.depth(derived, ECS::IS_A) == 3);

    // Moving the deeper base deeper moves e and derived; moving the shallow
    // one does not change the maximum.
    const EntityId deeper_root = world.new_entity();
    world.add(deep_root, world.pair(ECS::IS_A, deeper_root));
    CHECK(world.depth(e, ECS::IS_A) == 3);
    CHECK(world.depth(derived, ECS::IS_A) == 4);
    world.add(shallow, world.pair(ECS::IS_A, deeper_root));
    CHECK(world.depth(e, ECS::IS_A) == 3);

    // Dropping the deep base falls back to the shallow one and unlinks.
    world.remove(e, world.pair(ECS::IS_A, deep));
    CHECK_FALSE(linked(world, ECS::IS_A, deep, e));
    CHECK(linked(world, ECS::IS_A, shallow, e));
    CHECK(world.depth(e, ECS::IS_A) == 2);
    CHECK(world.depth(derived, ECS::IS_A) == 3);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hierarchy: a cycle terminates with a reported depth") {
    World world;
    world.init();

    const EntityId a = world.new_entity();
    const EntityId b = world.new_entity();
    world.add(a, world.pair(ECS::CHILD_OF, b));
    world.add(b, world.pair(ECS::CHILD_OF, a));

    // Neither value is meaningful, but asking must return and leave the
    // nodes computed so the error is not repeated on every query.
    const u32 depth_a = world.depth(a);
    const u32 depth_b = world.depth(b);
    CHECK(depth_a >= 1);
    CHECK(depth_b >= 1);
    CHECK(world.depth(a) == depth_a);
    CHECK(world.depth(b) == depth_b);

    // Breaking the cycle gives real depths again.
    world.remove(b, world.pair(ECS::CHILD_OF, a));
    CHECK(world.depth(b) == 0);
    CHECK(world.depth(a) == 1);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hierarchy: recycled ids and deleted parents leave no stale depth") {
    World world;
    world.init();

    const EntityId parent = world.new_entity();
    const EntityId child = world.new_entity();
    world.add(child, world.pair(ECS::CHILD_OF, parent));
    REQUIRE(world.depth(child) == 1);

    // The cascade deletes the child too; a new entity recycling the parent's
    // slot starts as a plain root with no children pointing at it.
    REQUIRE(world.delete_entity(parent));
    const EntityId recycled = world.new_entity();
    CHECK(ECS::ENTITY_LOW(recycled) == ECS::ENTITY_LOW(parent));
    CHECK_FALSE(is_target(world, recycled));
    CHECK(node(world, ECS::CHILD_OF, recycled) == nullptr);
    CHECK(world.depth(recycled) == 0);

    const EntityId new_child = world.new_entity();
    world.add(new_child, world.pair(ECS::CHILD_OF, recycled));
    CHECK(world.depth(new_child) == 1);
    CHECK(is_target(world, recycled));

    world.free();
    CHECK_ARENA_CLEAN();
}
