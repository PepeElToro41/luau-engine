#include "support/test_support.hpp"

// Component / tag registration and the has / add / remove / set / get family
// on plain ids, in both the raw and the templated forms.

namespace {

struct Big {
    f64 values[4] = {};
};

struct Counter {
    i32 hits = 0;
};

} // namespace

TEST_CASE("ecs/components: component<T> claims one id per type and keeps it") {
    World world;
    world.init();

    const EntityId position = world.component<Position>();
    const EntityId velocity = world.component<Velocity>();

    CHECK(position != 0);
    CHECK(velocity != 0);
    CHECK(position != velocity);
    CHECK(position >= 1);
    CHECK(position <= ECS::MAX_COMPONENT_ID);
    CHECK(velocity <= ECS::MAX_COMPONENT_ID);
    CHECK(world.alive(position));
    CHECK(world.alive(velocity));

    // Repeated calls, and id<T>(), return the same entity.
    CHECK(world.component<Position>() == position);
    CHECK(world.component<Velocity>() == velocity);
    CHECK(world.id<Position>() == position);
    CHECK(world.id<Velocity>() == velocity);

    // Ids are handed out in order from next_component_id.
    const Id next = world.next_component_id;
    const EntityId health = world.component<Health>();
    CHECK(health == next);
    CHECK(world.next_component_id == next + 1);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/components: component<T> stores T's TypeInfo as ECS::COMPONENT data") {
    World world;
    world.init();

    const EntityId position = world.component<Position>();
    const TypeInfo* info = static_cast<const TypeInfo*>(world.get(position, ECS::COMPONENT));
    REQUIRE(info != nullptr);
    CHECK(info->length == sizeof(Position));
    CHECK(info->alignment == alignof(Position));

    const EntityId big = world.component<Big>();
    info = static_cast<const TypeInfo*>(world.get(big, ECS::COMPONENT));
    REQUIRE(info != nullptr);
    CHECK(info->length == sizeof(Big));
    CHECK(info->alignment == alignof(Big));

    // get_type_info only knows ids registered in type_info_index: COMPONENT
    // itself. Everything else reads its size off the entity.
    const TypeInfo* component_info = world.get_type_info(ECS::COMPONENT);
    REQUIRE(component_info != nullptr);
    CHECK(component_info->length == sizeof(TypeInfo));
    CHECK(component_info->alignment == alignof(TypeInfo));
    CHECK(world.get_type_info(position) == nullptr);
    CHECK(world.get_type_info(world.tag<TagA>()) == nullptr);
    CHECK(world.get_type_info(ECS::REST + 50) == nullptr);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/components: empty types register as tags") {
    World world;
    world.init();

    SUBCASE("id<T> routes empty types to tag<T>") {
        const EntityId tag_a = world.id<TagA>();
        CHECK(tag_a != 0);
        CHECK(tag_a <= ECS::MAX_COMPONENT_ID);
        CHECK(world.tag<TagA>() == tag_a);
        // A tag stores no COMPONENT data at all.
        CHECK(world.get(tag_a, ECS::COMPONENT) == nullptr);
        // The first registration decides: component<TagA>() now returns the tag.
        CHECK(world.component<TagA>() == tag_a);
        CHECK(world.get(tag_a, ECS::COMPONENT) == nullptr);
    }
    SUBCASE("component<T> on an empty type stores a zero-length TypeInfo") {
        const EntityId tag_b = world.component<TagB>();
        const TypeInfo* info = static_cast<const TypeInfo*>(world.get(tag_b, ECS::COMPONENT));
        REQUIRE(info != nullptr);
        CHECK(info->length == 0);
        CHECK(world.tag<TagB>() == tag_b);
        CHECK(world.id<TagB>() == tag_b);
    }
    SUBCASE("tag<T> on a type with data makes it carry none") {
        const EntityId counter = world.tag<Counter>();
        CHECK(world.component<Counter>() == counter);
        CHECK(world.get(counter, ECS::COMPONENT) == nullptr);

        const EntityId e = world.new_entity();
        world.add(e, counter);
        CHECK(world.has(e, counter));
        CHECK(world.get(e, counter) == nullptr);
    }
    SUBCASE("a tag on an entity has no data") {
        const EntityId e = world.new_entity();
        world.add<TagA>(e);
        CHECK(world.has<TagA>(e));
        CHECK(world.get(e, world.id<TagA>()) == nullptr);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/components: a typed query on a new type claims its component id") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    const Id next = world.next_component_id;

    CHECK_FALSE(world.has<Health>(e));
    CHECK(world.next_component_id == next + 1);
    CHECK(world.id<Health>() == next);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/components: raw has / add / remove on tags") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    const EntityId tag = world.tag<TagA>();

    CHECK_FALSE(world.has(e, tag));
    world.add(e, tag);
    CHECK(world.has(e, tag));
    CHECK(world.has<TagA>(e));

    // Adding twice is a no-op; removing takes it away once.
    world.add(e, tag);
    CHECK(world.has(e, tag));
    world.remove(e, tag);
    CHECK_FALSE(world.has(e, tag));
    CHECK_FALSE(world.has<TagA>(e));

    SUBCASE("any alive entity can be used as a tag") {
        const EntityId marker = world.new_entity();
        world.add(e, marker);
        CHECK(world.has(e, marker));
        CHECK(world.get(e, marker) == nullptr);
        world.remove(e, marker);
        CHECK_FALSE(world.has(e, marker));
    }
    SUBCASE("a dead or stale id is refused") {
        const EntityId dead = world.new_entity();
        REQUIRE(world.delete_entity(dead));
        world.add(e, dead);
        CHECK_FALSE(world.has(e, dead));
        // The slot's new owner is not bound through the stale id either.
        const EntityId recycled = world.new_entity();
        CHECK_FALSE(world.has(e, recycled));
    }
    SUBCASE("id 0 is ignored") {
        world.add(e, 0);
        world.remove(e, 0);
        CHECK_FALSE(world.has(e, 0));
    }
    SUBCASE("operations on a dead entity are no-ops") {
        const EntityId gone = world.new_entity();
        REQUIRE(world.delete_entity(gone));
        world.add(gone, tag);
        CHECK_FALSE(world.has(gone, tag));
        world.remove(gone, tag);
        CHECK(world.get(gone, tag) == nullptr);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/components: set adds a missing component and get reads it back") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    CHECK(world.get<Position>(e) == nullptr);
    CHECK_FALSE(world.has<Position>(e));

    world.set(e, Position { 1.5f, -2.5f });
    CHECK(world.has<Position>(e));
    Position* position = world.get<Position>(e);
    REQUIRE(position != nullptr);
    CHECK(position->x == 1.5f);
    CHECK(position->y == -2.5f);

    SUBCASE("set on an existing component overwrites in place") {
        world.set(e, Position { 7, 8 });
        CHECK(world.has<Position>(e));
        position = world.get<Position>(e);
        REQUIRE(position != nullptr);
        CHECK(position->x == 7);
        CHECK(position->y == 8);
    }
    SUBCASE("writing through the pointer sticks") {
        position->x = 42;
        CHECK(world.get<Position>(e)->x == 42);
    }
    SUBCASE("other components stay missing") {
        CHECK(world.get<Velocity>(e) == nullptr);
        CHECK_FALSE(world.has<Velocity>(e));
        CHECK(world.get<Health>(e) == nullptr);
    }
    SUBCASE("set and get on a dead entity do nothing") {
        const EntityId gone = world.new_entity();
        REQUIRE(world.delete_entity(gone));
        world.set(gone, Position { 1, 1 });
        CHECK(world.get<Position>(gone) == nullptr);
        CHECK_FALSE(world.has<Position>(gone));
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/components: raw set / get work through the component id") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    const Id health = world.component<Health>();

    const Health value { 77 };
    world.set(e, health, &value);
    CHECK(world.has(e, health));

    const Health* raw = static_cast<const Health*>(world.get(e, health));
    REQUIRE(raw != nullptr);
    CHECK(raw->value == 77);
    // Same storage either way.
    CHECK(world.get<Health>(e) == raw);

    world.remove(e, health);
    CHECK(world.get(e, health) == nullptr);
    CHECK_FALSE(world.has<Health>(e));

    // set with id 0 is ignored.
    world.set(e, 0, &value);
    CHECK_FALSE(world.has(e, 0));

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/components: add on a component zeroes its value") {
    World world;
    world.init();

    // Put a value into the row a later entity will reuse, so leftover bytes
    // would be visible if add() did not zero them.
    const EntityId first = world.new_entity();
    world.set(first, Position { 9, 9 });
    REQUIRE(world.delete_entity(first));

    const EntityId e = world.new_entity();
    world.add<Position>(e);
    CHECK(world.has<Position>(e));
    const Position* position = world.get<Position>(e);
    REQUIRE(position != nullptr);
    CHECK(position->x == 0);
    CHECK(position->y == 0);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/components: data survives moves between archetypes") {
    World world;
    world.init();

    constexpr usz COUNT = 5;
    EntityId entities[COUNT];
    for (usz i = 0; i < COUNT; ++i) {
        entities[i] = world.new_entity();
        world.set(entities[i], Position { static_cast<f32>(i), static_cast<f32>(i * 10) });
        world.set(entities[i], Velocity { 1, static_cast<f32>(i) });
    }

    // Every entity moves twice; the second and fourth move again.
    for (usz i = 0; i < COUNT; ++i) {
        world.add<TagA>(entities[i]);
        world.set(entities[i], Health { static_cast<i32>(100 + i) });
    }
    world.remove<Velocity>(entities[1]);
    world.remove<TagA>(entities[3]);
    world.add<TagB>(entities[3]);

    for (usz i = 0; i < COUNT; ++i) {
        const EntityId e = entities[i];
        const Position* position = world.get<Position>(e);
        REQUIRE(position != nullptr);
        CHECK(position->x == static_cast<f32>(i));
        CHECK(position->y == static_cast<f32>(i * 10));

        const Health* health = world.get<Health>(e);
        REQUIRE(health != nullptr);
        CHECK(health->value == static_cast<i32>(100 + i));

        if (i == 1) {
            CHECK(world.get<Velocity>(e) == nullptr);
        } else {
            const Velocity* velocity = world.get<Velocity>(e);
            REQUIRE(velocity != nullptr);
            CHECK(velocity->dx == 1);
            CHECK(velocity->dy == static_cast<f32>(i));
        }
        CHECK(world.has<TagA>(e) == (i != 3));
        CHECK(world.has<TagB>(e) == (i == 3));
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/components: a large component keeps its bytes across moves") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    Big big;
    for (usz i = 0; i < 4; ++i) {
        big.values[i] = 0.25 * static_cast<f64>(i + 1);
    }
    world.set(e, big);
    world.add<TagA>(e);
    world.set(e, Position { 1, 1 });
    world.remove<TagA>(e);

    const Big* stored = world.get<Big>(e);
    REQUIRE(stored != nullptr);
    for (usz i = 0; i < 4; ++i) {
        CHECK(stored->values[i] == 0.25 * static_cast<f64>(i + 1));
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/components: removing an absent id is harmless") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    world.set(e, Position { 2, 3 });

    world.remove<Velocity>(e);
    world.remove<TagA>(e);
    world.remove(e, world.new_entity());
    CHECK(world.has<Position>(e));
    REQUIRE(world.get<Position>(e) != nullptr);
    CHECK(world.get<Position>(e)->x == 2);
    CHECK(world.get<Position>(e)->y == 3);

    // Removing twice only removes once.
    world.remove<Position>(e);
    world.remove<Position>(e);
    CHECK_FALSE(world.has<Position>(e));
    CHECK(world.alive(e));

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/components: entities in one archetype keep their rows straight on swap-remove") {
    World world;
    world.init();

    constexpr usz COUNT = 6;
    EntityId entities[COUNT];
    for (usz i = 0; i < COUNT; ++i) {
        entities[i] = world.new_entity();
        world.set(entities[i], Health { static_cast<i32>(i + 1) });
        world.set(entities[i], Position { static_cast<f32>(i), 0 });
    }

    SUBCASE("deleting the first entity moves the last into its row") {
        REQUIRE(world.delete_entity(entities[0]));
        for (usz i = 1; i < COUNT; ++i) {
            REQUIRE(world.get<Health>(entities[i]) != nullptr);
            CHECK(world.get<Health>(entities[i])->value == static_cast<i32>(i + 1));
            CHECK(world.get<Position>(entities[i])->x == static_cast<f32>(i));
        }
    }
    SUBCASE("removing a component from a middle entity") {
        world.remove<Position>(entities[2]);
        CHECK(world.get<Position>(entities[2]) == nullptr);
        REQUIRE(world.get<Health>(entities[2]) != nullptr);
        CHECK(world.get<Health>(entities[2])->value == 3);
        for (usz i = 0; i < COUNT; ++i) {
            if (i == 2) {
                continue;
            }
            REQUIRE(world.get<Position>(entities[i]) != nullptr);
            CHECK(world.get<Position>(entities[i])->x == static_cast<f32>(i));
            CHECK(world.get<Health>(entities[i])->value == static_cast<i32>(i + 1));
        }
    }
    SUBCASE("deleting and clearing several entities in a row") {
        REQUIRE(world.delete_entity(entities[1]));
        world.clear(entities[4]);
        REQUIRE(world.delete_entity(entities[COUNT - 1]));
        REQUIRE(world.get<Health>(entities[0]) != nullptr);
        CHECK(world.get<Health>(entities[0])->value == 1);
        CHECK(world.get<Health>(entities[2])->value == 3);
        CHECK(world.get<Health>(entities[3])->value == 4);
        CHECK(world.get<Position>(entities[3])->x == 3);
        CHECK(world.get<Health>(entities[4]) == nullptr);
        CHECK(world.alive(entities[4]));

        // The archetype shrinks to the survivors and grows again cleanly.
        const EntityId fresh = world.new_entity();
        world.set(fresh, Health { 50 });
        world.set(fresh, Position { 5, 5 });
        CHECK(world.get<Health>(fresh)->value == 50);
        CHECK(world.get<Health>(entities[0])->value == 1);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/components: tags and components mix in one archetype") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    world.add<TagA>(e);
    world.set(e, Position { 1, 2 });
    world.add<TagB>(e);
    world.set(e, Health { 3 });

    CHECK(world.has<TagA>(e));
    CHECK(world.has<TagB>(e));
    CHECK(world.has<Position>(e));
    CHECK(world.has<Health>(e));
    CHECK(world.get(e, world.id<TagA>()) == nullptr);
    CHECK(world.get<Position>(e)->y == 2);
    CHECK(world.get<Health>(e)->value == 3);

    world.remove<TagA>(e);
    CHECK_FALSE(world.has<TagA>(e));
    CHECK(world.has<TagB>(e));
    CHECK(world.get<Position>(e)->x == 1);
    CHECK(world.get<Health>(e)->value == 3);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/components: a runtime entity with COMPONENT data stores values") {
    World world;
    world.init();

    // Any entity carrying a TypeInfo as its COMPONENT data is a component.
    const EntityId component = world.new_entity();
    const TypeInfo info { sizeof(Health), alignof(Health) };
    world.set(component, ECS::COMPONENT, &info);

    const EntityId e = world.new_entity();
    const Health value { 21 };
    world.set(e, component, &value);
    CHECK(world.has(e, component));
    const Health* stored = static_cast<const Health*>(world.get(e, component));
    REQUIRE(stored != nullptr);
    CHECK(stored->value == 21);

    world.add<TagA>(e);
    stored = static_cast<const Health*>(world.get(e, component));
    REQUIRE(stored != nullptr);
    CHECK(stored->value == 21);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/components: modified fires the changed hooks for a held component") {
    World world;
    world.init();
    HookLog log;
    world.hook_changed<Position>(HookLog::record_changed, &log);

    const EntityId e = world.new_entity();
    world.set(e, Position { 1, 1 });
    CHECK(log.count_of(HOOK_CHANGED) == 0);

    world.get<Position>(e)->x = 5;
    world.modified<Position>(e);
    CHECK(log.count_of(HOOK_CHANGED) == 1);
    CHECK(log.events[0].entity == e);
    CHECK(log.events[0].id == world.id<Position>());

    // Not held: nothing to report.
    world.modified<Velocity>(e);
    world.modified(e, world.id<Position>());
    CHECK(log.count_of(HOOK_CHANGED) == 2);
    world.remove<Position>(e);
    world.modified<Position>(e);
    CHECK(log.count_of(HOOK_CHANGED) == 2);

    world.free();
    log.free();
    CHECK_ARENA_CLEAN();
}
