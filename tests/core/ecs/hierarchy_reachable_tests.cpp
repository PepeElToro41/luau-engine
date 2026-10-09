#include "support/test_support.hpp"

#include "engine/ecs/entity_index.hpp"
#include "engine/ecs/hierarchy.hpp"

// The reachable set cached on traversable pair records: its contents and
// ranks, the parent links and target archetype it is built from, what a
// move invalidates, several bases, cycles, and find_reachable's patterns.

namespace {

ComponentRecord* pair_record(World& world, const Id relation, const EntityId target) {
    return ComponentRecord::component_record_find(&world, ECS::PAIR(relation, target));
}

const HierarchyNode* node(World& world, const Id relation, const EntityId target) {
    const ComponentRecord* record = pair_record(world, relation, target);
    return record != nullptr ? record->hierarchy : nullptr;
}

// The reachable set of (relation, target); fails the test if the record
// has no node.
const ReachableSet& reach(World& world, const Id relation, const EntityId target) {
    ComponentRecord* record = pair_record(world, relation, target);
    REQUIRE(record != nullptr);
    const ReachableSet* set = HIERARCHY::reachable(&world, record);
    REQUIRE(set != nullptr);
    return *set;
}

// The entry for a concrete id, or nullptr.
const ReachableEntry* entry(const ReachableSet& set, const Id id) {
    const usz index = HIERARCHY::find_reachable(set, id, 0);
    return index < set.count() ? &set.entries[index] : nullptr;
}

// Whether `set` is sorted by id without duplicates and every entry's column
// really holds its id on its source.
bool consistent(World& world, const ReachableSet& set) {
    for (usz i = 0; i < set.count(); i++) {
        if (i > 0 && set.ids[i - 1] >= set.ids[i]) {
            return false;
        }
        const EntityRecord* record = world.entity_index.get_record_alive(set.entries[i].source);
        if (record == nullptr || record->archetype == nullptr) {
            return false;
        }
        const ArchetypeType& type = record->archetype->type;
        if (set.entries[i].column >= type.id_count || type.ids[set.entries[i].column] != set.ids[i]) {
            return false;
        }
    }
    return true;
}

Archetype* archetype_of(World& world, const EntityId entity) {
    const EntityRecord* record = world.entity_index.get_record_alive(entity);
    REQUIRE(record != nullptr);
    return record->archetype;
}

} // namespace

TEST_CASE("ecs/hierarchy: reachable lists the target's ids and its ancestors', nearest first") {
    World world;
    world.init();

    // grandparent (Health) <- parent (Velocity, Position) <- target (Position) <- child
    const EntityId grandparent = world.new_entity();
    world.set<Health>(grandparent, { 3 });
    const EntityId parent = world.new_entity();
    world.add(parent, world.pair(ECS::CHILD_OF, grandparent));
    world.set<Velocity>(parent, { 2, 0 });
    world.set<Position>(parent, { 2, 0 });
    const EntityId target = world.new_entity();
    world.add(target, world.pair(ECS::CHILD_OF, parent));
    world.set<Position>(target, { 1, 0 });
    const EntityId child = world.new_entity();
    world.add(child, world.pair(ECS::CHILD_OF, target));

    const HierarchyNode* target_node = node(world, ECS::CHILD_OF, target);
    REQUIRE(target_node != nullptr);
    CHECK(target_node->reachable_dirty);
    CHECK(target_node->target_archetype == archetype_of(world, target));
    REQUIRE(target_node->parents.count == 1);
    CHECK(target_node->parents[0] == pair_record(world, ECS::CHILD_OF, parent));

    const ReachableSet& set = reach(world, ECS::CHILD_OF, target);
    CHECK_FALSE(target_node->reachable_dirty);
    CHECK(consistent(world, set));
    // Position, (CHILD_OF, parent) from the target; Velocity, (CHILD_OF,
    // grandparent) from the parent; Health from the grandparent.
    CHECK(set.count() == 5);
    CHECK(target_node->reachable_ranks == 3);

    const ReachableEntry* position = entry(set, world.id<Position>());
    REQUIRE(position != nullptr);
    CHECK(position->source == target);
    CHECK(position->rank == 0);
    const ReachableEntry* own_pair = entry(set, world.pair(ECS::CHILD_OF, parent));
    REQUIRE(own_pair != nullptr);
    CHECK(own_pair->source == target);
    CHECK(own_pair->rank == 0);
    const ReachableEntry* velocity = entry(set, world.id<Velocity>());
    REQUIRE(velocity != nullptr);
    CHECK(velocity->source == parent);
    CHECK(velocity->rank == 1);
    const ReachableEntry* parent_pair = entry(set, world.pair(ECS::CHILD_OF, grandparent));
    REQUIRE(parent_pair != nullptr);
    CHECK(parent_pair->source == parent);
    CHECK(parent_pair->rank == 1);
    const ReachableEntry* health = entry(set, world.id<Health>());
    REQUIRE(health != nullptr);
    CHECK(health->source == grandparent);
    CHECK(health->rank == 2);
    CHECK(entry(set, world.id<TagA>()) == nullptr);

    // Computing the target's set computed its parents' on the way.
    CHECK_FALSE(node(world, ECS::CHILD_OF, parent)->reachable_dirty);
    CHECK_FALSE(node(world, ECS::CHILD_OF, grandparent)->reachable_dirty);
    CHECK(reach(world, ECS::CHILD_OF, grandparent).count() == 1);
    CHECK(reach(world, ECS::CHILD_OF, parent).count() == 4);

    // A root with no parent: own ids only, rank 0 throughout.
    const ReachableSet& top = reach(world, ECS::CHILD_OF, grandparent);
    CHECK(top.count() == 1);
    CHECK(top.ids[0] == world.id<Health>());
    CHECK(top.entries[0].source == grandparent);
    CHECK(node(world, ECS::CHILD_OF, grandparent)->reachable_ranks == 1);
    CHECK(node(world, ECS::CHILD_OF, grandparent)->parents.count == 0);

    // An entity nothing points at has no node and no set.
    CHECK(pair_record(world, ECS::CHILD_OF, child) == nullptr);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hierarchy: a move of a target dirties its node and everything below, nothing else") {
    World world;
    world.init();

    const EntityId grandparent = world.new_entity();
    world.set<Health>(grandparent, { 3 });
    const EntityId parent = world.new_entity();
    world.add(parent, world.pair(ECS::CHILD_OF, grandparent));
    world.set<Velocity>(parent, { 2, 0 });
    const EntityId target = world.new_entity();
    world.add(target, world.pair(ECS::CHILD_OF, parent));
    world.set<Position>(target, { 1, 0 });
    const EntityId child = world.new_entity();
    world.add(child, world.pair(ECS::CHILD_OF, target));
    const EntityId other = world.new_entity();
    world.set<Position>(other, { 9, 0 });
    const EntityId other_child = world.new_entity();
    world.add(other_child, world.pair(ECS::CHILD_OF, other));

    reach(world, ECS::CHILD_OF, target);
    reach(world, ECS::CHILD_OF, other);
    REQUIRE_FALSE(node(world, ECS::CHILD_OF, grandparent)->reachable_dirty);
    REQUIRE_FALSE(node(world, ECS::CHILD_OF, parent)->reachable_dirty);
    REQUIRE_FALSE(node(world, ECS::CHILD_OF, target)->reachable_dirty);
    REQUIRE_FALSE(node(world, ECS::CHILD_OF, other)->reachable_dirty);

    SUBCASE("the grandparent gaining a component reaches the whole chain") {
        world.add<TagA>(grandparent);
        CHECK(node(world, ECS::CHILD_OF, grandparent)->reachable_dirty);
        CHECK(node(world, ECS::CHILD_OF, parent)->reachable_dirty);
        CHECK(node(world, ECS::CHILD_OF, target)->reachable_dirty);
        CHECK_FALSE(node(world, ECS::CHILD_OF, other)->reachable_dirty);
        CHECK(node(world, ECS::CHILD_OF, grandparent)->target_archetype == archetype_of(world, grandparent));

        const ReachableSet& set = reach(world, ECS::CHILD_OF, target);
        CHECK(consistent(world, set));
        const ReachableEntry* tag = entry(set, world.id<TagA>());
        REQUIRE(tag != nullptr);
        CHECK(tag->source == grandparent);
        CHECK(tag->rank == 2);

        world.remove<TagA>(grandparent);
        CHECK(entry(reach(world, ECS::CHILD_OF, target), world.id<TagA>()) == nullptr);
    }

    SUBCASE("the target itself changing dirties only its own node, and its ids win over the ancestors'") {
        world.set<Velocity>(target, { 7, 0 });
        CHECK(node(world, ECS::CHILD_OF, target)->reachable_dirty);
        CHECK_FALSE(node(world, ECS::CHILD_OF, parent)->reachable_dirty);
        CHECK_FALSE(node(world, ECS::CHILD_OF, grandparent)->reachable_dirty);

        const ReachableSet& set = reach(world, ECS::CHILD_OF, target);
        CHECK(consistent(world, set));
        const ReachableEntry* velocity = entry(set, world.id<Velocity>());
        REQUIRE(velocity != nullptr);
        CHECK(velocity->source == target);
        CHECK(velocity->rank == 0);
    }

    SUBCASE("a move of an entity that is no target changes nothing") {
        world.add<TagB>(child);
        world.add<TagB>(other_child);
        CHECK_FALSE(node(world, ECS::CHILD_OF, target)->reachable_dirty);
        CHECK_FALSE(node(world, ECS::CHILD_OF, other)->reachable_dirty);
    }

    SUBCASE("a reparent relinks parents and rebuilds the set from the new chain") {
        world.add(target, world.pair(ECS::CHILD_OF, other));
        const HierarchyNode* target_node = node(world, ECS::CHILD_OF, target);
        CHECK(target_node->reachable_dirty);
        REQUIRE(target_node->parents.count == 1);
        CHECK(target_node->parents[0] == pair_record(world, ECS::CHILD_OF, other));
        CHECK(node(world, ECS::CHILD_OF, other)->children.contains(ECS::PAIR(ECS::CHILD_OF, target)));
        CHECK_FALSE(node(world, ECS::CHILD_OF, parent)->children.contains(ECS::PAIR(ECS::CHILD_OF, target)));

        const ReachableSet& set = reach(world, ECS::CHILD_OF, target);
        CHECK(consistent(world, set));
        CHECK(entry(set, world.id<Velocity>()) == nullptr);
        CHECK(entry(set, world.id<Health>()) == nullptr);
        const ReachableEntry* position = entry(set, world.id<Position>());
        REQUIRE(position != nullptr);
        CHECK(position->source == target);
        CHECK(target_node->reachable_ranks == 2);

        // Dropping the parent altogether leaves the own ids.
        world.remove(target, world.pair(ECS::CHILD_OF, other));
        CHECK(target_node->parents.count == 0);
        const ReachableSet& alone = reach(world, ECS::CHILD_OF, target);
        CHECK(alone.count() == 1);
        CHECK(alone.ids[0] == world.id<Position>());
        CHECK(target_node->reachable_ranks == 1);
    }

    SUBCASE("deleting an ancestor takes the chain with it under CHILD_OF") {
        world.delete_entity(parent);
        CHECK_FALSE(world.alive(target));
        CHECK(pair_record(world, ECS::CHILD_OF, target) == nullptr);
        CHECK(node(world, ECS::CHILD_OF, grandparent)->children.count == 0);
        CHECK(reach(world, ECS::CHILD_OF, grandparent).count() == 1);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hierarchy: reachable over several bases follows the depth-first order of the walk") {
    World world;
    world.init();

    // base_root (Health, Position) <- first (Position); second (Health, Velocity);
    // e derives from first and second; derived from e so (IS_A, e) has a node.
    const EntityId base_root = world.new_entity();
    world.set<Health>(base_root, { 5 });
    world.set<Position>(base_root, { 9, 0 });
    const EntityId first = world.new_entity();
    world.add(first, world.pair(ECS::IS_A, base_root));
    world.set<Position>(first, { 1, 0 });
    const EntityId second = world.new_entity();
    world.set<Health>(second, { 7 });
    world.set<Velocity>(second, { 2, 0 });
    const EntityId e = world.new_entity();
    world.add(e, world.pair(ECS::IS_A, first));
    world.add(e, world.pair(ECS::IS_A, second));
    const EntityId derived = world.new_entity();
    world.add(derived, world.pair(ECS::IS_A, e));

    const HierarchyNode* e_node = node(world, ECS::IS_A, e);
    REQUIRE(e_node != nullptr);
    REQUIRE(e_node->parents.count == 2);
    CHECK(e_node->parents[0] == pair_record(world, ECS::IS_A, first));
    CHECK(e_node->parents[1] == pair_record(world, ECS::IS_A, second));

    const ReachableSet& set = reach(world, ECS::IS_A, e);
    CHECK(consistent(world, set));
    // Walk: e (0), first (1), base_root (2), second (3).
    CHECK(e_node->reachable_ranks == 4);
    const ReachableEntry* position = entry(set, world.id<Position>());
    REQUIRE(position != nullptr);
    CHECK(position->source == first);
    CHECK(position->rank == 1);
    const ReachableEntry* health = entry(set, world.id<Health>());
    REQUIRE(health != nullptr);
    CHECK(health->source == base_root);
    CHECK(health->rank == 2);
    const ReachableEntry* velocity = entry(set, world.id<Velocity>());
    REQUIRE(velocity != nullptr);
    CHECK(velocity->source == second);
    CHECK(velocity->rank == 3);

    // A change on the second base dirties e's node but not first's chain.
    world.add<TagA>(second);
    CHECK(e_node->reachable_dirty);
    CHECK_FALSE(node(world, ECS::IS_A, first)->reachable_dirty);
    CHECK(entry(reach(world, ECS::IS_A, e), world.id<TagA>())->rank == 3);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hierarchy: a cycle still yields a set and reports it") {
    World world;
    world.init();

    const EntityId a = world.new_entity();
    world.set<Position>(a, { 1, 0 });
    const EntityId b = world.new_entity();
    world.set<Velocity>(b, { 1, 0 });
    world.add(a, world.pair(ECS::CHILD_OF, b));
    world.add(b, world.pair(ECS::CHILD_OF, a));

    // Asking returns; the link closing the cycle contributes nothing, so
    // the set of each holds what the chain up to the repeat holds.
    const ReachableSet& set = reach(world, ECS::CHILD_OF, a);
    CHECK(set.count() >= 2);
    CHECK(entry(set, world.id<Position>()) != nullptr);
    CHECK(consistent(world, set));
    CHECK_FALSE(node(world, ECS::CHILD_OF, a)->reachable_dirty);
    CHECK_FALSE(node(world, ECS::CHILD_OF, b)->reachable_dirty);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hierarchy: find_reachable answers concrete ids, relation ranges and wildcards") {
    World world;
    world.init();

    const EntityId apple = world.new_entity();
    const EntityId pear = world.new_entity();
    const EntityId target = world.new_entity();
    world.set<Position>(target, { 1, 0 });
    world.add<Likes>(target, apple);
    world.add<Likes>(target, pear);
    world.add<Eats>(target, apple);
    const EntityId child = world.new_entity();
    world.add(child, world.pair(ECS::CHILD_OF, target));

    const ReachableSet& set = reach(world, ECS::CHILD_OF, target);
    CHECK(set.count() == 4);
    CHECK(consistent(world, set));

    // Concrete: one hit, and nothing after it.
    const usz position = HIERARCHY::find_reachable(set, world.id<Position>(), 0);
    REQUIRE(position < set.count());
    CHECK(set.ids[position] == world.id<Position>());
    CHECK(HIERARCHY::find_reachable(set, world.id<Position>(), position + 1) == set.count());
    CHECK(HIERARCHY::find_reachable(set, world.id<Health>(), 0) == set.count());
    CHECK(HIERARCHY::find_reachable(set, world.pair<Likes>(pear), 0) < set.count());
    CHECK(HIERARCHY::find_reachable(set, world.pair<Eats>(pear), 0) == set.count());

    // (Likes, *): both pairs, in id order, then the end.
    const Id likes_any = ECS::PAIR(world.id<Likes>(), ECS::WILDCARD);
    const usz first = HIERARCHY::find_reachable(set, likes_any, 0);
    REQUIRE(first < set.count());
    CHECK(set.ids[first] == world.pair<Likes>(apple));
    const usz second = HIERARCHY::find_reachable(set, likes_any, first + 1);
    REQUIRE(second < set.count());
    CHECK(set.ids[second] == world.pair<Likes>(pear));
    CHECK(HIERARCHY::find_reachable(set, likes_any, second + 1) == set.count());
    CHECK(HIERARCHY::find_reachable(set, ECS::PAIR(world.id<Likes>(), ECS::ANY), 0) == first);

    // (*, apple): two pairs through a scan.
    const Id any_apple = ECS::PAIR(ECS::WILDCARD, apple);
    usz hits = 0;
    for (usz i = HIERARCHY::find_reachable(set, any_apple, 0); i < set.count(); i = HIERARCHY::find_reachable(set, any_apple, i + 1)) {
        CHECK(ECS::PAIR_SECOND(set.ids[i]) == ECS::ENTITY_LOW(apple));
        hits++;
    }
    CHECK(hits == 2);

    // Wildcard: everything.
    hits = 0;
    for (usz i = HIERARCHY::find_reachable(set, ECS::WILDCARD, 0); i < set.count(); i = HIERARCHY::find_reachable(set, ECS::WILDCARD, i + 1)) {
        hits++;
    }
    CHECK(hits == set.count());
    CHECK(HIERARCHY::find_reachable(set, ECS::WILDCARD, set.count()) == set.count());

    world.free();
    CHECK_ARENA_CLEAN();
}
