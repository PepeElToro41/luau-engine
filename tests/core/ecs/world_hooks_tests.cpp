#include "support/test_support.hpp"

// Added / removed / changed hooks: registration per id and per pattern, what
// each operation fires, unhook and the world's hook_counts bookkeeping.

namespace {

// user_data target for capture-less lambdas that need to report something
// other than a HookEvent.
struct Probe {
    i32 calls = 0;
    f32 seen_x = 0;
    EntityId entity = 0;
};

} // namespace

TEST_CASE("ecs/hooks: added fires once with the entity and id, set and add alike") {
    World world;
    world.init();
    HookLog log;
    const HookId hook = world.hook_added<Position>(HookLog::record_added, &log);
    CHECK(hook != 0);

    const EntityId e = world.new_entity();
    world.set(e, Position { 1, 2 });
    REQUIRE(log.events.count == 1);
    CHECK(log.events[0].kind == HOOK_ADDED);
    CHECK(log.events[0].entity == e);
    CHECK(log.events[0].id == world.id<Position>());

    SUBCASE("a set that overwrites does not fire added again") {
        world.set(e, Position { 3, 4 });
        CHECK(log.count_of(HOOK_ADDED) == 1);
    }
    SUBCASE("add fires added once and is silent when already held") {
        const EntityId other = world.new_entity();
        world.add<Position>(other);
        world.add<Position>(other);
        CHECK(log.count_of(HOOK_ADDED) == 2);
        CHECK(log.events[1].entity == other);
    }
    SUBCASE("other ids do not reach a per-id hook") {
        world.set(e, Velocity { 1, 1 });
        world.add<TagA>(e);
        CHECK(log.count_of(HOOK_ADDED) == 1);
    }
    SUBCASE("the added hook runs after the data is written") {
        Probe probe;
        world.hook_added<Position>([](World* w, EntityId entity, Id id, void* user_data) {
            Probe* p = static_cast<Probe*>(user_data);
            p->calls += 1;
            p->seen_x = static_cast<Position*>(w->get(entity, id))->x;
        }, &probe);
        const EntityId other = world.new_entity();
        world.set(other, Position { 42, 0 });
        CHECK(probe.calls == 1);
        CHECK(probe.seen_x == 42);
    }

    world.free();
    log.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hooks: changed fires when set overwrites or modified is called") {
    World world;
    world.init();
    HookLog log;
    world.hook_changed<Health>(HookLog::record_changed, &log);
    world.hook_added<Health>(HookLog::record_added, &log);

    const EntityId e = world.new_entity();
    world.set(e, Health { 1 });
    // The set that adds the id reports only added.
    CHECK(log.count_of(HOOK_ADDED) == 1);
    CHECK(log.count_of(HOOK_CHANGED) == 0);

    world.set(e, Health { 2 });
    REQUIRE(log.count_of(HOOK_CHANGED) == 1);
    CHECK(log.events[1].kind == HOOK_CHANGED);
    CHECK(log.events[1].entity == e);
    CHECK(log.events[1].id == world.id<Health>());
    CHECK(log.count_of(HOOK_ADDED) == 1);

    SUBCASE("modified fires changed for a held component") {
        world.get<Health>(e)->value = 9;
        world.modified<Health>(e);
        world.modified(e, world.id<Health>());
        CHECK(log.count_of(HOOK_CHANGED) == 3);
    }
    SUBCASE("modified is a no-op for ids the entity does not hold") {
        const EntityId other = world.new_entity();
        world.modified<Health>(other);
        world.remove<Health>(e);
        world.modified<Health>(e);
        CHECK(log.count_of(HOOK_CHANGED) == 1);
    }
    SUBCASE("the changed hook sees the new value") {
        Probe probe;
        world.hook_changed<Health>([](World* w, EntityId entity, Id id, void* user_data) {
            Probe* p = static_cast<Probe*>(user_data);
            p->calls += 1;
            p->seen_x = static_cast<f32>(static_cast<Health*>(w->get(entity, id))->value);
        }, &probe);
        world.set(e, Health { 77 });
        CHECK(probe.calls == 1);
        CHECK(probe.seen_x == 77);
    }

    world.free();
    log.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hooks: set on a tag the entity already has fires nothing") {
    World world;
    world.init();
    HookLog log;
    world.hook_added<TagA>(HookLog::record_added, &log);
    world.hook_changed<TagA>(HookLog::record_changed, &log);

    const EntityId e = world.new_entity();
    world.add<TagA>(e);
    CHECK(log.count_of(HOOK_ADDED) == 1);
    world.set(e, world.id<TagA>(), nullptr);
    CHECK(log.count_of(HOOK_ADDED) == 1);
    CHECK(log.count_of(HOOK_CHANGED) == 0);

    world.free();
    log.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hooks: removed fires before the id goes, with the data readable") {
    World world;
    world.init();
    HookLog log;
    world.hook_removed<Position>(HookLog::record_removed, &log);

    const EntityId e = world.new_entity();
    world.set(e, Position { 5, 6 });
    CHECK(log.events.count == 0);

    SUBCASE("remove") {
        Probe probe;
        world.hook_removed<Position>([](World* w, EntityId entity, Id id, void* user_data) {
            Probe* p = static_cast<Probe*>(user_data);
            p->calls += 1;
            p->entity = entity;
            // Still held while the hook runs.
            p->seen_x = w->has(entity, id) ? static_cast<Position*>(w->get(entity, id))->x : -1.0f;
        }, &probe);
        world.remove<Position>(e);
        REQUIRE(log.events.count == 1);
        CHECK(log.events[0].kind == HOOK_REMOVED);
        CHECK(log.events[0].entity == e);
        CHECK(log.events[0].id == world.id<Position>());
        CHECK(probe.calls == 1);
        CHECK(probe.entity == e);
        CHECK(probe.seen_x == 5);
        CHECK_FALSE(world.has<Position>(e));

        // Removing an absent id fires nothing.
        world.remove<Position>(e);
        CHECK(log.events.count == 1);
    }
    SUBCASE("delete_entity") {
        REQUIRE(world.delete_entity(e));
        REQUIRE(log.events.count == 1);
        CHECK(log.events[0].entity == e);
        CHECK(log.events[0].id == world.id<Position>());
    }
    SUBCASE("clear") {
        world.clear(e);
        REQUIRE(log.events.count == 1);
        CHECK(log.events[0].entity == e);
        CHECK(world.alive(e));
    }

    world.free();
    log.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hooks: clear and delete fire removed for every id the entity holds") {
    World world;
    world.init();
    HookLog log;
    world.hook_removed(ECS::WILDCARD, HookLog::record_removed, &log);

    const EntityId bob = world.new_entity();
    const EntityId e = world.new_entity();
    world.set(e, Position { 1, 1 });
    world.add<TagA>(e);
    world.add<Likes>(e, bob);
    const Id ids[] = { world.id<Position>(), world.id<TagA>(), world.pair<Likes>(bob) };

    SUBCASE("clear") {
        world.clear(e);
    }
    SUBCASE("delete_entity") {
        REQUIRE(world.delete_entity(e));
    }

    REQUIRE(log.events.count == 3);
    for (const HookEvent& event : log.events) {
        CHECK(event.kind == HOOK_REMOVED);
        CHECK(event.entity == e);
    }
    for (const Id id : ids) {
        usz seen = 0;
        for (const HookEvent& event : log.events) {
            if (event.id == id) {
                seen += 1;
            }
        }
        CHECK(seen == 1);
    }

    world.free();
    log.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hooks: a WILDCARD hook sees every id and ANY is the same hook") {
    World world;
    world.init();
    HookLog log;
    // Registering a component type is itself a set() of ECS::COMPONENT on the
    // component entity, which a world-wide hook would see: claim the types first.
    world.id<TagA>();
    world.id<Position>();
    world.id<Likes>();
    world.id<TagB>();
    const HookId hook = world.hook_added(ECS::ANY, HookLog::record_added, &log);

    const EntityId bob = world.new_entity();
    const EntityId e = world.new_entity();
    world.add<TagA>(e);
    world.set(e, Position { 1, 1 });
    world.add<Likes>(e, bob);
    world.add(e, ECS::PAIR(ECS::CHILD_OF, bob));

    REQUIRE(log.events.count == 4);
    CHECK(log.events[0].id == world.id<TagA>());
    CHECK(log.events[1].id == world.id<Position>());
    CHECK(log.events[2].id == world.pair<Likes>(bob));
    CHECK(log.events[3].id == ECS::PAIR(ECS::CHILD_OF, bob));
    for (const HookEvent& event : log.events) {
        CHECK(event.entity == e);
    }

    // Registered through ANY, removable through WILDCARD: both fold to one list.
    CHECK(world.unhook(ECS::WILDCARD, hook));
    world.add<TagB>(e);
    CHECK(log.events.count == 4);

    world.free();
    log.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hooks: a (*, *) hook sees only pairs") {
    World world;
    world.init();
    HookLog log;
    const Id any_pair = ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD);
    const HookId added = world.hook_added(any_pair, HookLog::record_added, &log);
    world.hook_removed(any_pair, HookLog::record_removed, &log);

    const EntityId bob = world.new_entity();
    const EntityId e = world.new_entity();
    world.add<TagA>(e);
    world.set(e, Position { 1, 1 });
    world.add<Likes>(e, bob);
    world.set<Likes, Health>(e, Health { 1 });
    REQUIRE(log.events.count == 2);
    CHECK(log.events[0].id == world.pair<Likes>(bob));
    CHECK(log.events[1].id == world.pair<Likes, Health>());

    world.remove<TagA>(e);
    world.remove<Likes>(e, bob);
    REQUIRE(log.events.count == 3);
    CHECK(log.events[2].kind == HOOK_REMOVED);
    CHECK(log.events[2].id == world.pair<Likes>(bob));

    // (*, *) spelled with ANY is the same list.
    CHECK(world.unhook(ECS::PAIR(ECS::ANY, ECS::ANY), added));
    world.add<Eats>(e, bob);
    CHECK(log.events.count == 3);

    world.free();
    log.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hooks: (R, *) and (*, T) hooks receive the concrete pair") {
    World world;
    world.init();
    HookLog relation_log;
    HookLog target_log;

    const EntityId a = world.new_entity();
    const EntityId b = world.new_entity();
    const Id likes = world.id<Likes>();
    const Id eats = world.id<Eats>();
    world.hook_added(ECS::PAIR(likes, ECS::WILDCARD), HookLog::record_added, &relation_log);
    world.hook_added(ECS::PAIR(ECS::WILDCARD, b), HookLog::record_added, &target_log);

    const EntityId e = world.new_entity();
    world.add<Likes>(e, a);
    world.add<Likes>(e, b);
    world.add<Eats>(e, b);
    world.add<Eats>(e, a);
    world.add<TagA>(e);

    REQUIRE(relation_log.events.count == 2);
    CHECK(relation_log.events[0].id == ECS::PAIR(likes, a));
    CHECK(relation_log.events[1].id == ECS::PAIR(likes, b));

    REQUIRE(target_log.events.count == 2);
    CHECK(target_log.events[0].id == ECS::PAIR(likes, b));
    CHECK(target_log.events[1].id == ECS::PAIR(eats, b));

    relation_log.free();
    world.free();
    target_log.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hooks: a hook on a concrete pair fires for that pair only") {
    World world;
    world.init();
    HookLog log;

    const EntityId a = world.new_entity();
    const EntityId b = world.new_entity();
    world.hook_added<ECS::Pair<Likes, Eats>>(HookLog::record_added, &log);
    world.hook_added(world.pair<Likes>(a), HookLog::record_added, &log);

    const EntityId e = world.new_entity();
    world.add<Likes, Eats>(e);
    world.add<Likes>(e, a);
    world.add<Likes>(e, b);
    world.add<Eats>(e, a);

    REQUIRE(log.events.count == 2);
    CHECK(log.events[0].id == world.pair<Likes, Eats>());
    CHECK(log.events[1].id == world.pair<Likes>(a));

    world.free();
    log.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hooks: an exclusive swap fires removed for the old pair and added for the new") {
    World world;
    world.init();
    HookLog log;
    const Id child_of_any = ECS::PAIR(ECS::CHILD_OF, ECS::WILDCARD);
    world.hook_added(child_of_any, HookLog::record_added, &log);
    world.hook_removed(child_of_any, HookLog::record_removed, &log);

    const EntityId parent_a = world.new_entity();
    const EntityId parent_b = world.new_entity();
    const EntityId child = world.new_entity();

    world.add(child, ECS::PAIR(ECS::CHILD_OF, parent_a));
    world.add(child, ECS::PAIR(ECS::CHILD_OF, parent_b));

    REQUIRE(log.events.count == 3);
    CHECK(log.events[0].kind == HOOK_ADDED);
    CHECK(log.events[0].id == ECS::PAIR(ECS::CHILD_OF, parent_a));
    CHECK(log.events[1].kind == HOOK_REMOVED);
    CHECK(log.events[1].id == ECS::PAIR(ECS::CHILD_OF, parent_a));
    CHECK(log.events[2].kind == HOOK_ADDED);
    CHECK(log.events[2].id == ECS::PAIR(ECS::CHILD_OF, parent_b));
    for (const HookEvent& event : log.events) {
        CHECK(event.entity == child);
    }

    world.free();
    log.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hooks: unhook stops the hook and reports whether it existed") {
    World world;
    world.init();
    HookLog log;

    const HookId added = world.hook_added<Position>(HookLog::record_added, &log);
    const HookId removed = world.hook_removed<Position>(HookLog::record_removed, &log);
    REQUIRE(added != 0);
    REQUIRE(removed != 0);
    CHECK(added != removed);

    const EntityId e = world.new_entity();
    world.set(e, Position { 1, 1 });
    CHECK(log.count_of(HOOK_ADDED) == 1);

    SUBCASE("a removed hook no longer fires") {
        CHECK(world.unhook(world.id<Position>(), added));
        const EntityId other = world.new_entity();
        world.set(other, Position { 2, 2 });
        CHECK(log.count_of(HOOK_ADDED) == 1);
        // The other hook on the same id is untouched.
        world.remove<Position>(e);
        CHECK(log.count_of(HOOK_REMOVED) == 1);
    }
    SUBCASE("unhooking twice, or with the wrong id, fails") {
        CHECK(world.unhook(world.id<Position>(), added));
        CHECK_FALSE(world.unhook(world.id<Position>(), added));
        CHECK_FALSE(world.unhook(world.id<Velocity>(), removed));
        CHECK_FALSE(world.unhook(world.id<Position>(), 123456));
        CHECK_FALSE(world.unhook(0, removed));
        CHECK_FALSE(world.unhook(world.id<Position>(), 0));
        CHECK_FALSE(world.unhook(ECS::WILDCARD, removed));
        // Still registered under its own id.
        CHECK(world.unhook(world.id<Position>(), removed));
    }
    SUBCASE("hooking with id 0 or no callback returns 0") {
        CHECK(world.hook_added(0, HookLog::record_added, &log) == 0);
        CHECK(world.hook_removed(0, HookLog::record_removed, &log) == 0);
    }

    world.free();
    log.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hooks: hook_counts track every registration and removal") {
    World world;
    world.init();
    HookLog log;
    CHECK(world.hook_counts[HOOK_ADDED] == 0);
    CHECK(world.hook_counts[HOOK_REMOVED] == 0);
    CHECK(world.hook_counts[HOOK_CHANGED] == 0);

    const EntityId bob = world.new_entity();
    const HookId a1 = world.hook_added<Position>(HookLog::record_added, &log);
    const HookId a2 = world.hook_added(ECS::WILDCARD, HookLog::record_added, &log);
    const HookId r1 = world.hook_removed(ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD), HookLog::record_removed, &log);
    const HookId c1 = world.hook_changed<Position>(HookLog::record_changed, &log);
    const HookId c2 = world.hook_changed(world.pair<Likes>(bob), HookLog::record_changed, &log);
    CHECK(world.hook_counts[HOOK_ADDED] == 2);
    CHECK(world.hook_counts[HOOK_REMOVED] == 1);
    CHECK(world.hook_counts[HOOK_CHANGED] == 2);

    SUBCASE("unhook decrements the right kind") {
        CHECK(world.unhook(world.id<Position>(), a1));
        CHECK(world.hook_counts[HOOK_ADDED] == 1);
        CHECK(world.unhook(ECS::WILDCARD, a2));
        CHECK(world.hook_counts[HOOK_ADDED] == 0);
        CHECK(world.unhook(ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD), r1));
        CHECK(world.hook_counts[HOOK_REMOVED] == 0);
        CHECK(world.unhook(world.id<Position>(), c1));
        CHECK(world.unhook(world.pair<Likes>(bob), c2));
        CHECK(world.hook_counts[HOOK_CHANGED] == 0);
        // A failed unhook changes nothing.
        CHECK_FALSE(world.unhook(world.id<Position>(), a1));
        CHECK(world.hook_counts[HOOK_ADDED] == 0);
    }
    SUBCASE("deleting a pair's target drops the hooks on that pair") {
        REQUIRE(world.delete_entity(bob));
        CHECK(world.hook_counts[HOOK_CHANGED] == 1);
        CHECK_FALSE(world.unhook(world.pair<Likes>(bob), c2));
        CHECK(world.unhook(world.id<Position>(), c1));
        CHECK(world.hook_counts[HOOK_CHANGED] == 0);
    }

    world.free();
    log.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hooks: user_data is handed to each hook unchanged") {
    World world;
    world.init();
    HookLog first;
    HookLog second;
    Probe probe;

    world.hook_added<TagA>(HookLog::record_added, &first);
    world.hook_added<TagA>(HookLog::record_added, &second);
    world.hook_added<TagA>([](World*, EntityId entity, Id, void* user_data) {
        Probe* p = static_cast<Probe*>(user_data);
        p->calls += 1;
        p->entity = entity;
    }, &probe);
    world.hook_added<TagB>(HookLog::record_added, &first);

    const EntityId e = world.new_entity();
    world.add<TagA>(e);
    world.add<TagB>(e);

    CHECK(first.events.count == 2);
    CHECK(second.events.count == 1);
    CHECK(probe.calls == 1);
    CHECK(probe.entity == e);

    first.free();
    world.free();
    second.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hooks: hooks fire in registration order and may unhook themselves") {
    World world;
    world.init();
    HookLog log;
    Probe probe;

    // The self-removing hook needs its own id: stash it through user_data.
    struct SelfRemover {
        HookId id = 0;
        i32 calls = 0;
    };
    SelfRemover remover;

    world.hook_added<TagA>([](World*, EntityId, Id, void* user_data) {
        static_cast<Probe*>(user_data)->calls += 1;
    }, &probe);
    remover.id = world.hook_added<TagA>([](World* w, EntityId, Id id, void* user_data) {
        SelfRemover* self = static_cast<SelfRemover*>(user_data);
        self->calls += 1;
        w->unhook(id, self->id);
    }, &remover);
    world.hook_added<TagA>(HookLog::record_added, &log);

    const EntityId e = world.new_entity();
    const EntityId other = world.new_entity();
    world.add<TagA>(e);
    CHECK(probe.calls == 1);
    CHECK(remover.calls == 1);
    CHECK(log.events.count == 1);

    // The removed hook is gone; the ones around it keep running.
    world.add<TagA>(other);
    CHECK(probe.calls == 2);
    CHECK(remover.calls == 1);
    CHECK(log.events.count == 2);
    CHECK(world.hook_counts[HOOK_ADDED] == 2);

    world.free();
    log.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hooks: free fires removed for every id still held") {
    World world;
    world.init();
    HookLog log;
    world.hook_removed<Position>(HookLog::record_removed, &log);
    world.hook_removed<ECS::Pair<Likes, Eats>>(HookLog::record_removed, &log);
    const Id position = world.id<Position>();
    const Id likes_eats = world.pair<Likes, Eats>();

    const EntityId a = world.new_entity();
    const EntityId b = world.new_entity();
    world.set(a, Position { 1, 1 });
    world.set(b, Position { 2, 2 });
    world.add<Likes, Eats>(b);
    world.add<TagA>(b);
    CHECK(log.events.count == 0);

    world.free();
    CHECK_ARENA_CLEAN();

    REQUIRE(log.events.count == 3);
    usz positions = 0;
    usz pairs = 0;
    for (const HookEvent& event : log.events) {
        CHECK(event.kind == HOOK_REMOVED);
        if (event.id == position) {
            positions += 1;
            CHECK((event.entity == a || event.entity == b));
        } else if (event.id == likes_eats) {
            pairs += 1;
            CHECK(event.entity == b);
        }
    }
    CHECK(positions == 2);
    CHECK(pairs == 1);

    log.free();
}
