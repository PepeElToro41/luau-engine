#include "support/test_support.hpp"

#include "engine/ecs/entity_cleanup.hpp"
#include "engine/ecs/entity_index.hpp"

// Deletion policies: what happens to the entities that hold an id referring
// to a deleted entity (REMOVE / DELETE / PANIC, as component, relation or
// target), the CHILD_OF cascade, and the world's consistency afterwards.

namespace {

// Number of alive archetypes, so a test can check that a cascade destroyed
// everything it emptied.
usz archetype_count(const World& world) {
    return world.archetypes.alive_count;
}

} // namespace

TEST_CASE("ecs/cleanup: deleting a tag entity removes it from every holder") {
    World world;
    world.init();

    const EntityId tag = world.new_entity();
    const EntityId e1 = world.new_entity();
    const EntityId e2 = world.new_entity();
    const EntityId e3 = world.new_entity();
    const usz baseline = archetype_count(world);

    // Holders spread over several archetypes, one with data alongside.
    world.add(e1, tag);
    world.add(e2, tag);
    world.add<TagA>(e2);
    world.add(e3, tag);
    world.set(e3, Position { 3, 3 });
    CHECK(archetype_count(world) == baseline + 3);

    CHECK(world.delete_entity(tag));
    CHECK_ARENA_CLEAN();
    CHECK_FALSE(world.alive(tag));
    CHECK(world.alive(e1));
    CHECK(world.alive(e2));
    CHECK(world.alive(e3));
    CHECK_FALSE(world.has(e1, tag));
    CHECK_FALSE(world.has(e2, tag));
    CHECK_FALSE(world.has(e3, tag));
    CHECK(world.has<TagA>(e2));
    REQUIRE(world.get<Position>(e3) != nullptr);
    CHECK(world.get<Position>(e3)->x == 3);

    // The archetypes that mentioned the tag are gone; [TagA] and [Position] remain.
    CHECK(archetype_count(world) == baseline + 2);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/cleanup: deleting a component entity removes it and keeps other data") {
    World world;
    world.init();

    // A runtime component: an entity carrying a TypeInfo.
    const EntityId component = world.new_entity();
    const TypeInfo info { sizeof(Health), alignof(Health) };
    world.set(component, ECS::COMPONENT, &info);

    const EntityId e = world.new_entity();
    const Health value { 4 };
    world.set(e, Position { 1, 2 });
    world.set(e, component, &value);
    REQUIRE(world.get(e, component) != nullptr);

    CHECK(world.delete_entity(component));
    CHECK_ARENA_CLEAN();
    CHECK(world.alive(e));
    CHECK_FALSE(world.has(e, component));
    CHECK(world.get(e, component) == nullptr);
    REQUIRE(world.get<Position>(e) != nullptr);
    CHECK(world.get<Position>(e)->y == 2);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/cleanup: deleting a pair's target or relation removes the pair") {
    World world;
    world.init();

    const EntityId bob = world.new_entity();
    const EntityId relation = world.new_entity();
    const EntityId e = world.new_entity();
    world.add<TagA>(e);
    world.set<Position>(e, bob, Position { 1, 1 });

    SUBCASE("the target") {
        world.add<Likes>(e, bob);
        world.add<Eats>(e, bob);
        world.add(e, World::pair(relation, bob));
        CHECK(world.delete_entity(bob));
        CHECK_ARENA_CLEAN();

        CHECK(world.alive(e));
        CHECK_FALSE(world.has<Likes>(e, bob));
        CHECK_FALSE(world.has<Eats>(e, bob));
        CHECK_FALSE(world.has<Position>(e, bob));
        CHECK_FALSE(world.has(e, World::pair(relation, bob)));
        CHECK_FALSE(world.has(e, ECS::PAIR(world.id<Likes>(), ECS::WILDCARD)));
        CHECK_FALSE(world.has(e, ECS::PAIR(ECS::WILDCARD, bob)));
        CHECK(world.has<TagA>(e));
    }
    SUBCASE("the relation") {
        const EntityId alice = world.new_entity();
        world.add(e, World::pair(relation, bob));
        world.add(e, World::pair(relation, alice));
        CHECK(world.delete_entity(relation));
        CHECK_ARENA_CLEAN();

        CHECK(world.alive(e));
        CHECK(world.alive(bob));
        CHECK(world.alive(alice));
        CHECK_FALSE(world.has(e, World::pair(relation, bob)));
        CHECK_FALSE(world.has(e, World::pair(relation, alice)));
        CHECK_FALSE(world.has(e, ECS::PAIR(relation, ECS::WILDCARD)));
        // Pairs with other relations pointing at the same targets stay.
        CHECK(world.has<Position>(e, bob));
        CHECK(world.get<Position>(e, bob)->x == 1);
    }
    SUBCASE("other targets of the same relation stay") {
        const EntityId alice = world.new_entity();
        world.add<Likes>(e, bob);
        world.add<Likes>(e, alice);
        CHECK(world.delete_entity(bob));
        CHECK_ARENA_CLEAN();
        CHECK(world.has<Likes>(e, alice));
        CHECK_FALSE(world.has<Likes>(e, bob));
        CHECK(world.has(e, ECS::PAIR(world.id<Likes>(), ECS::WILDCARD)));
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/cleanup: (ON_DELETE, DELETE) deletes the holders") {
    World world;
    world.init();

    const EntityId trait = world.new_entity();
    world.add(trait, ECS::PAIR(ECS::ON_DELETE, ECS::DELETE));
    const EntityId bystander = world.new_entity();
    const usz baseline = archetype_count(world);

    const EntityId e1 = world.new_entity();
    const EntityId e2 = world.new_entity();
    const EntityId e3 = world.new_entity();

    SUBCASE("held as a tag") {
        world.add(e1, trait);
        world.add(e2, trait);
        world.set(e2, Position { 1, 1 });
        world.add<TagA>(e3);
    }
    SUBCASE("held as the relation of a pair") {
        world.add(e1, World::pair(trait, bystander));
        world.add(e2, World::pair(trait, e3));
        world.add<TagA>(e3);
    }

    CHECK(world.delete_entity(trait));
    CHECK_ARENA_CLEAN();
    CHECK_FALSE(world.alive(trait));
    CHECK_FALSE(world.alive(e1));
    CHECK_FALSE(world.alive(e2));
    CHECK(world.alive(e3));
    CHECK(world.alive(bystander));
    CHECK(world.has<TagA>(e3));
    // Only the [TagA] archetype survives the cascade.
    CHECK(archetype_count(world) == baseline + 1);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/cleanup: (ON_DELETE, PANIC) refuses the deletion until the trait goes") {
    World world;
    world.init();

    const EntityId protected_entity = world.new_entity();
    world.add(protected_entity, ECS::PAIR(ECS::ON_DELETE, ECS::PANIC));
    const EntityId holder = world.new_entity();
    world.add(holder, protected_entity);
    world.add<TagA>(holder);

    // Refused, and nothing changed.
    CHECK_FALSE(world.delete_entity(protected_entity));
    CHECK_ARENA_CLEAN();
    CHECK(world.alive(protected_entity));
    CHECK(world.has(holder, protected_entity));
    CHECK(world.has<TagA>(holder));

    // Still refused with no holders: the policy is on the entity itself.
    world.remove(holder, protected_entity);
    CHECK_FALSE(world.delete_entity(protected_entity));
    CHECK(world.alive(protected_entity));

    world.remove(protected_entity, ECS::PAIR(ECS::ON_DELETE, ECS::PANIC));
    world.add(holder, protected_entity);
    CHECK(world.delete_entity(protected_entity));
    CHECK_ARENA_CLEAN();
    CHECK_FALSE(world.alive(protected_entity));
    CHECK_FALSE(world.has(holder, protected_entity));
    CHECK(world.has<TagA>(holder));

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/cleanup: (ON_DELETE_TARGET, PANIC) protects targets while pairs are held") {
    World world;
    world.init();

    const EntityId relation = world.new_entity();
    world.add(relation, ECS::PAIR(ECS::ON_DELETE_TARGET, ECS::PANIC));
    const EntityId target = world.new_entity();
    const EntityId holder = world.new_entity();
    world.add(holder, World::pair(relation, target));
    world.add<Likes>(holder, target);

    CHECK_FALSE(world.delete_entity(target));
    CHECK_ARENA_CLEAN();
    CHECK(world.alive(target));
    CHECK(world.has(holder, World::pair(relation, target)));
    CHECK(world.has<Likes>(holder, target));

    SUBCASE("deletable once the pair is dropped") {
        world.remove(holder, World::pair(relation, target));
        CHECK(world.delete_entity(target));
        CHECK_ARENA_CLEAN();
        CHECK_FALSE(world.alive(target));
        CHECK(world.alive(holder));
        CHECK_FALSE(world.has<Likes>(holder, target));
    }
    SUBCASE("deletable once the holder is gone") {
        REQUIRE(world.delete_entity(holder));
        CHECK(world.delete_entity(target));
        CHECK_ARENA_CLEAN();
        CHECK_FALSE(world.alive(target));
    }
    SUBCASE("deletable once the relation drops the policy") {
        world.remove(relation, ECS::PAIR(ECS::ON_DELETE_TARGET, ECS::PANIC));
        CHECK(world.delete_entity(target));
        CHECK_ARENA_CLEAN();
        CHECK_FALSE(world.alive(target));
        CHECK(world.alive(holder));
        CHECK_FALSE(world.has(holder, ECS::PAIR(relation, ECS::WILDCARD)));
    }
    SUBCASE("the relation itself can still be deleted") {
        CHECK(world.delete_entity(relation));
        CHECK_ARENA_CLEAN();
        CHECK(world.alive(target));
        CHECK_FALSE(world.has(holder, World::pair(relation, target)));
        CHECK(world.has<Likes>(holder, target));
        CHECK(world.delete_entity(target));
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/cleanup: (ON_DELETE_TARGET, DELETE) deletes the holders of the pair") {
    World world;
    world.init();

    const EntityId owns = world.new_entity();
    world.add(owns, ECS::PAIR(ECS::ON_DELETE_TARGET, ECS::DELETE));
    const EntityId target = world.new_entity();
    const EntityId owner = world.new_entity();
    const EntityId fan = world.new_entity();
    const EntityId both = world.new_entity();

    world.add(owner, World::pair(owns, target));
    world.add<Likes>(fan, target);
    // DELETE wins when an archetype holds a REMOVE and a DELETE relation.
    world.add<Likes>(both, target);
    world.add(both, World::pair(owns, target));
    world.add<TagA>(both);

    CHECK(world.delete_entity(target));
    CHECK_ARENA_CLEAN();
    CHECK_FALSE(world.alive(target));
    CHECK_FALSE(world.alive(owner));
    CHECK_FALSE(world.alive(both));
    CHECK(world.alive(fan));
    CHECK_FALSE(world.has<Likes>(fan, target));
    CHECK(world.alive(owns));

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/cleanup: deleting a CHILD_OF parent deletes children and grandchildren") {
    World world;
    world.init();

    const EntityId parent = world.new_entity();
    const EntityId child_a = world.new_entity();
    const EntityId child_b = world.new_entity();
    const EntityId grandchild = world.new_entity();
    const EntityId great_grandchild = world.new_entity();
    const EntityId unrelated = world.new_entity();
    const usz baseline = archetype_count(world);

    world.add(child_a, ECS::PAIR(ECS::CHILD_OF, parent));
    world.add(child_b, ECS::PAIR(ECS::CHILD_OF, parent));
    world.set(child_b, Position { 1, 1 });
    world.add(grandchild, ECS::PAIR(ECS::CHILD_OF, child_a));
    world.add(great_grandchild, ECS::PAIR(ECS::CHILD_OF, grandchild));
    world.add<Likes>(unrelated, parent);

    CHECK(world.delete_entity(parent));
    CHECK_ARENA_CLEAN();
    CHECK_FALSE(world.alive(parent));
    CHECK_FALSE(world.alive(child_a));
    CHECK_FALSE(world.alive(child_b));
    CHECK_FALSE(world.alive(grandchild));
    CHECK_FALSE(world.alive(great_grandchild));
    CHECK(world.alive(unrelated));
    CHECK_FALSE(world.has<Likes>(unrelated, parent));
    CHECK(archetype_count(world) == baseline);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/cleanup: deleting a child leaves its parent and siblings alone") {
    World world;
    world.init();

    const EntityId parent = world.new_entity();
    const EntityId child = world.new_entity();
    const EntityId sibling = world.new_entity();
    const EntityId grandchild = world.new_entity();
    world.add(child, ECS::PAIR(ECS::CHILD_OF, parent));
    world.add(sibling, ECS::PAIR(ECS::CHILD_OF, parent));
    world.add(grandchild, ECS::PAIR(ECS::CHILD_OF, child));

    CHECK(world.delete_entity(child));
    CHECK_ARENA_CLEAN();
    CHECK(world.alive(parent));
    CHECK(world.alive(sibling));
    CHECK(world.has(sibling, ECS::PAIR(ECS::CHILD_OF, parent)));
    CHECK_FALSE(world.alive(child));
    CHECK_FALSE(world.alive(grandchild));

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/cleanup: hooks fire for everything a cascade removes, children first") {
    World world;
    world.init();
    HookLog log;
    world.hook_removed<TagA>(HookLog::record_removed, &log);
    world.hook_removed(ECS::PAIR(ECS::CHILD_OF, ECS::WILDCARD), HookLog::record_removed, &log);
    world.hook_removed(ECS::PAIR(world.id<Likes>(), ECS::WILDCARD), HookLog::record_removed, &log);

    const EntityId parent = world.new_entity();
    const EntityId child = world.new_entity();
    const EntityId grandchild = world.new_entity();
    const EntityId fan = world.new_entity();
    world.add<TagA>(parent);
    world.add<TagA>(child);
    world.add(child, ECS::PAIR(ECS::CHILD_OF, parent));
    world.add<TagA>(grandchild);
    world.add(grandchild, ECS::PAIR(ECS::CHILD_OF, child));
    world.add<Likes>(fan, parent);
    world.add<TagA>(fan);

    REQUIRE(world.delete_entity(parent));
    CHECK_ARENA_CLEAN();

    // grandchild: TagA + (CHILD_OF, child); child: TagA + (CHILD_OF, parent);
    // fan: (Likes, parent) only, it keeps TagA; parent: TagA.
    REQUIRE(log.events.count == 6);
    for (const HookEvent& event : log.events) {
        CHECK(event.kind == HOOK_REMOVED);
    }

    usz grandchild_last = 0;
    usz child_first = 0;
    usz parent_first = 0;
    usz fan_first = 0;
    usz tag_events = 0;
    for (usz i = 0; i < log.events.count; ++i) {
        const HookEvent& event = log.events[i];
        if (event.id == world.id<TagA>()) {
            tag_events += 1;
        }
        if (event.entity == grandchild) {
            grandchild_last = i;
        } else if (event.entity == child && child_first == 0) {
            child_first = i;
        } else if (event.entity == parent) {
            parent_first = i;
        } else if (event.entity == fan) {
            fan_first = i;
            CHECK(event.id == world.pair<Likes>(parent));
        }
    }
    CHECK(tag_events == 3);
    // Children die before their parent's own ids go, grandchildren before children.
    CHECK(grandchild_last < child_first);
    CHECK(child_first < parent_first);
    CHECK(fan_first < parent_first);
    CHECK(parent_first == log.events.count - 1);

    world.free();
    log.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/cleanup: an entity refusing deletion inside a cascade only loses the pair") {
    World world;
    world.init();

    const EntityId parent = world.new_entity();
    const EntityId child = world.new_entity();
    const EntityId stubborn = world.new_entity();
    world.add(child, ECS::PAIR(ECS::CHILD_OF, parent));
    world.add(stubborn, ECS::PAIR(ECS::CHILD_OF, parent));
    world.add(stubborn, ECS::PAIR(ECS::ON_DELETE, ECS::PANIC));
    world.add<TagA>(stubborn);

    CHECK(world.delete_entity(parent));
    CHECK_ARENA_CLEAN();
    CHECK_FALSE(world.alive(parent));
    CHECK_FALSE(world.alive(child));
    CHECK(world.alive(stubborn));
    CHECK_FALSE(world.has(stubborn, ECS::PAIR(ECS::CHILD_OF, ECS::WILDCARD)));
    CHECK(world.has<TagA>(stubborn));
    CHECK(world.has(stubborn, ECS::PAIR(ECS::ON_DELETE, ECS::PANIC)));

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/cleanup: cycles and self-references delete cleanly") {
    World world;
    world.init();

    SUBCASE("two entities that are each other's parent") {
        const EntityId a = world.new_entity();
        const EntityId b = world.new_entity();
        world.add(a, ECS::PAIR(ECS::CHILD_OF, b));
        world.add(b, ECS::PAIR(ECS::CHILD_OF, a));
        CHECK(world.delete_entity(a));
        CHECK_ARENA_CLEAN();
        CHECK_FALSE(world.alive(a));
        CHECK_FALSE(world.alive(b));
    }
    SUBCASE("an entity that is its own parent") {
        const EntityId e = world.new_entity();
        world.add(e, ECS::PAIR(ECS::CHILD_OF, e));
        world.add<TagA>(e);
        CHECK(world.delete_entity(e));
        CHECK_ARENA_CLEAN();
        CHECK_FALSE(world.alive(e));
    }
    SUBCASE("an entity holding a pair that points at itself") {
        const EntityId e = world.new_entity();
        world.add<Likes>(e, e);
        world.set<Position>(e, e, Position { 1, 1 });
        CHECK(world.delete_entity(e));
        CHECK_ARENA_CLEAN();
        CHECK_FALSE(world.alive(e));
    }
    SUBCASE("an entity used as its own tag") {
        const EntityId e = world.new_entity();
        world.add(e, e);
        CHECK(world.has(e, e));
        CHECK(world.delete_entity(e));
        CHECK_ARENA_CLEAN();
        CHECK_FALSE(world.alive(e));
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/cleanup: the world is consistent after a cascade") {
    World world;
    world.init();

    const EntityId parent = world.new_entity();
    const EntityId child = world.new_entity();
    const EntityId fan = world.new_entity();
    const usz baseline = archetype_count(world);
    world.add(child, ECS::PAIR(ECS::CHILD_OF, parent));
    world.set(child, Position { 1, 1 });
    world.add<Likes>(fan, parent);
    world.add<Likes>(fan, child);
    world.set(fan, Health { 2 });
    const EntityIdLow parent_low = ECS::ENTITY_LOW(parent);
    const EntityIdLow child_low = ECS::ENTITY_LOW(child);

    REQUIRE(world.delete_entity(parent));
    CHECK_ARENA_CLEAN();
    CHECK(world.alive(fan));
    CHECK_FALSE(world.has(fan, ECS::PAIR(world.id<Likes>(), ECS::WILDCARD)));
    REQUIRE(world.get<Health>(fan) != nullptr);
    CHECK(world.get<Health>(fan)->value == 2);
    // Everything that mentioned the parent or child is gone; [Health] stays.
    CHECK(archetype_count(world) == baseline + 1);
    CHECK(world.entity_index.count() == ECS::REST + 1);

    // Both low ids come back, at a new generation, and start clean.
    const EntityId first = world.new_entity();
    const EntityId second = world.new_entity();
    CHECK((ECS::ENTITY_LOW(first) == parent_low || ECS::ENTITY_LOW(first) == child_low));
    CHECK((ECS::ENTITY_LOW(second) == parent_low || ECS::ENTITY_LOW(second) == child_low));
    CHECK(ECS::ENTITY_LOW(first) != ECS::ENTITY_LOW(second));
    CHECK(ENTITY_INDEX::entity_generation(first) == 1);
    CHECK(ENTITY_INDEX::entity_generation(second) == 1);
    CHECK_FALSE(world.has<Likes>(fan, first));
    CHECK_FALSE(world.has<Likes>(fan, second));
    CHECK_FALSE(world.has(first, ECS::PAIR(ECS::CHILD_OF, ECS::WILDCARD)));
    CHECK(world.get<Position>(first) == nullptr);
    CHECK(world.get<Position>(second) == nullptr);

    // The recycled ids work as pair sides and as a parent again.
    world.add(second, ECS::PAIR(ECS::CHILD_OF, first));
    world.add<Likes>(fan, first);
    world.set<Position>(fan, second, Position { 5, 5 });
    CHECK(world.has(second, ECS::PAIR(ECS::CHILD_OF, first)));
    CHECK(world.has<Likes>(fan, first));
    CHECK(world.get<Position>(fan, second)->x == 5);

    REQUIRE(world.delete_entity(first));
    CHECK_ARENA_CLEAN();
    CHECK_FALSE(world.alive(second));
    CHECK(world.alive(fan));
    CHECK_FALSE(world.has<Likes>(fan, first));
    CHECK_FALSE(world.has<Position>(fan, second));
    CHECK(world.get<Health>(fan)->value == 2);
    CHECK(archetype_count(world) == baseline + 1);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/cleanup: deleting a target used by many holders in many archetypes") {
    World world;
    world.init();

    const EntityId target = world.new_entity();
    constexpr usz COUNT = 40;
    EntityId holders[COUNT];
    for (usz i = 0; i < COUNT; ++i) {
        holders[i] = world.new_entity();
        world.set(holders[i], Health { static_cast<i32>(i) });
        if (i % 2 == 0) {
            world.add<Likes>(holders[i], target);
        }
        if (i % 3 == 0) {
            world.set<Position>(holders[i], target, Position { static_cast<f32>(i), 0 });
        }
        if (i % 5 == 0) {
            world.add<TagA>(holders[i]);
        }
    }

    CHECK(world.delete_entity(target));
    CHECK_ARENA_CLEAN();
    for (usz i = 0; i < COUNT; ++i) {
        CHECK(world.alive(holders[i]));
        CHECK_FALSE(world.has<Likes>(holders[i], target));
        CHECK_FALSE(world.has<Position>(holders[i], target));
        CHECK(world.has<TagA>(holders[i]) == (i % 5 == 0));
        REQUIRE(world.get<Health>(holders[i]) != nullptr);
        CHECK(world.get<Health>(holders[i])->value == static_cast<i32>(i));
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/cleanup: ENTITY_CLEANUP::can_delete reports the PANIC policies") {
    World world;
    world.init();

    const EntityId plain = world.new_entity();
    CHECK(ENTITY_CLEANUP::can_delete(&world, plain));
    CHECK_FALSE(ENTITY_CLEANUP::can_delete(&world, ECS::COMPONENT));
    CHECK_FALSE(ENTITY_CLEANUP::can_delete(&world, world.component<Position>()));

    const EntityId relation = world.new_entity();
    world.add(relation, ECS::PAIR(ECS::ON_DELETE_TARGET, ECS::PANIC));
    const EntityId holder = world.new_entity();
    world.add(holder, World::pair(relation, plain));
    CHECK_FALSE(ENTITY_CLEANUP::can_delete(&world, plain));
    world.remove(holder, World::pair(relation, plain));
    CHECK(ENTITY_CLEANUP::can_delete(&world, plain));
    CHECK(world.alive(plain));

    world.free();
    CHECK_ARENA_CLEAN();
}
