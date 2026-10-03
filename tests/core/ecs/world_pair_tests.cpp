#include "support/test_support.hpp"

#include "engine/ecs/entity_index.hpp"

#include <type_traits>

// Pairs (relation, target): id construction, the typed and raw operation
// shapes, which side stores data, wildcards and exclusive relations.

TEST_CASE("ecs/pairs: ECS::Pair stores the relation's data, or the target's when the relation is a tag") {
    static_assert(std::is_same_v<ECS::Pair<Position, TagA>::type, Position>);
    static_assert(std::is_same_v<ECS::Pair<Position, Health>::type, Position>);
    static_assert(std::is_same_v<ECS::Pair<Likes, Health>::type, Health>);
    static_assert(std::is_same_v<ECS::Pair<Likes, Eats>::type, Likes>);
    static_assert(ECS::PairTraits<ECS::Pair<Likes, Eats>>::is_pair);
    static_assert(!ECS::PairTraits<Position>::is_pair);
    static_assert(std::is_same_v<ECS::StorageType<ECS::Pair<Likes, Health>>, Health>);
    static_assert(std::is_same_v<ECS::StorageType<Position>, Position>);
    CHECK(true);
}

TEST_CASE("ecs/pairs: pair ids are built from the low ids of both sides") {
    World world;
    world.init();

    const Id likes = world.id<Likes>();
    const Id eats = world.id<Eats>();

    // Recycle an id so the target carries a generation PAIR() must strip.
    const EntityId stale = world.new_entity();
    REQUIRE(world.delete_entity(stale));
    const EntityId bob = world.new_entity();
    REQUIRE(ENTITY_INDEX::entity_generation(bob) == 1);

    const Id typed = world.pair<Likes, Eats>();
    CHECK(ECS::IS_PAIR(typed));
    CHECK(typed == ECS::PAIR(likes, eats));
    CHECK(typed == World::pair(likes, eats));
    CHECK(world.id<ECS::Pair<Likes, Eats>>() == typed);
    CHECK(ECS::PAIR_FIRST(typed) == likes);
    CHECK(ECS::PAIR_SECOND(typed) == eats);

    const Id runtime = world.pair<Likes>(bob);
    CHECK(runtime == ECS::PAIR(likes, bob));
    CHECK(ECS::PAIR_FIRST(runtime) == likes);
    CHECK(ECS::PAIR_SECOND(runtime) == ECS::ENTITY_LOW(bob));
    CHECK(ECS::PAIR_SECOND(runtime) != bob);
    // PAIR_SECOND is the low id; pair_second brings the generation back.
    CHECK(world.pair_second(runtime) == bob);
    CHECK(world.pair_first(runtime) == likes);
    CHECK(World::pair(likes, bob) == World::pair(likes, stale));

    // A 0 side yields no pair.
    CHECK(world.pair<Likes>(0) == 0);
    CHECK_FALSE(ECS::PAIR_HAS_WILDCARD(runtime));
    CHECK(ECS::PAIR_HAS_WILDCARD(ECS::PAIR(likes, ECS::WILDCARD)));

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/pairs: tag-only pairs in every shape") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    const EntityId bob = world.new_entity();
    const EntityId relation = world.new_entity();

    SUBCASE("two types") {
        CHECK_FALSE(world.has<Likes, Eats>(e));
        world.add<Likes, Eats>(e);
        CHECK(world.has<Likes, Eats>(e));
        CHECK(world.has<ECS::Pair<Likes, Eats>>(e));
        CHECK(world.has(e, world.pair<Likes, Eats>()));
        CHECK(world.get(e, world.pair<Likes, Eats>()) == nullptr);
        world.remove<Likes, Eats>(e);
        CHECK_FALSE(world.has<Likes, Eats>(e));
    }
    SUBCASE("typed relation, runtime target") {
        world.add<Likes>(e, bob);
        CHECK(world.has<Likes>(e, bob));
        CHECK(world.has(e, ECS::PAIR(world.id<Likes>(), bob)));
        CHECK_FALSE(world.has<Eats>(e, bob));
        CHECK_FALSE(world.has<Likes>(e, relation));
        world.remove<Likes>(e, bob);
        CHECK_FALSE(world.has<Likes>(e, bob));
    }
    SUBCASE("runtime relation, typed target") {
        world.add_second<Eats>(e, relation);
        CHECK(world.has_second<Eats>(e, relation));
        CHECK(world.has(e, ECS::PAIR(relation, world.id<Eats>())));
        CHECK_FALSE(world.has_second<Likes>(e, relation));
        world.remove_second<Eats>(e, relation);
        CHECK_FALSE(world.has_second<Eats>(e, relation));
    }
    SUBCASE("raw") {
        const Id pair = World::pair(relation, bob);
        world.add(e, pair);
        CHECK(world.has(e, pair));
        CHECK(world.get(e, pair) == nullptr);
        world.add(e, pair);
        world.remove(e, pair);
        CHECK_FALSE(world.has(e, pair));
        world.remove(e, pair);
        CHECK(world.alive(e));
    }
    SUBCASE("a pair whose target is dead, or 0, is refused") {
        const EntityId gone = world.new_entity();
        REQUIRE(world.delete_entity(gone));
        world.add<Likes>(e, gone);
        CHECK_FALSE(world.has<Likes>(e, gone));
        CHECK_FALSE(world.has(e, ECS::PAIR(world.id<Likes>(), ECS::WILDCARD)));
        world.add<Likes>(e, 0);
        world.add(e, World::pair(gone, bob));
        CHECK_FALSE(world.has(e, World::pair(gone, bob)));
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/pairs: a relation with data stores its type on the pair") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    const EntityId bob = world.new_entity();

    SUBCASE("two types") {
        CHECK(world.get<Position, TagA>(e) == nullptr);
        world.set<Position, TagA>(e, Position { 1, 2 });
        CHECK(world.has<Position, TagA>(e));
        Position* position = world.get<Position, TagA>(e);
        REQUIRE(position != nullptr);
        CHECK(position->x == 1);
        CHECK(position->y == 2);
        // get<T> with a Pair type reads the same storage.
        CHECK(world.get<ECS::Pair<Position, TagA>>(e) == position);
        // The relation's data wins even when the target has data too.
        world.set<Position, Health>(e, Position { 3, 4 });
        REQUIRE(world.get<Position, Health>(e) != nullptr);
        CHECK(world.get<Position, Health>(e)->x == 3);
        // The plain component is untouched by its pairs.
        CHECK_FALSE(world.has<Position>(e));
    }
    SUBCASE("typed relation, runtime target") {
        world.set<Position>(e, bob, Position { 5, 6 });
        CHECK(world.has<Position>(e, bob));
        Position* position = world.get<Position>(e, bob);
        REQUIRE(position != nullptr);
        CHECK(position->y == 6);
        world.set<Position>(e, bob, Position { 7, 8 });
        CHECK(world.get<Position>(e, bob)->x == 7);
        // Raw access through the same pair id.
        const Position* raw = static_cast<const Position*>(world.get(e, world.pair<Position>(bob)));
        CHECK(raw == position);
        world.remove<Position>(e, bob);
        CHECK(world.get<Position>(e, bob) == nullptr);
    }
    SUBCASE("raw set with the relation's size") {
        const Id pair = World::pair(world.component<Health>(), bob);
        const Health value { 11 };
        world.set(e, pair, &value);
        const Health* stored = static_cast<const Health*>(world.get(e, pair));
        REQUIRE(stored != nullptr);
        CHECK(stored->value == 11);
        CHECK(world.get_second<Health>(e, world.component<Health>()) == nullptr);
    }
    SUBCASE("every entity holds its own value for the same pair") {
        const EntityId other = world.new_entity();
        world.set<Position>(e, bob, Position { 1, 0 });
        world.set<Position>(other, bob, Position { 2, 0 });
        CHECK(world.get<Position>(e, bob)->x == 1);
        CHECK(world.get<Position>(other, bob)->x == 2);
        world.add<TagA>(e);
        CHECK(world.get<Position>(e, bob)->x == 1);
        CHECK(world.get<Position>(other, bob)->x == 2);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/pairs: a tag relation stores the target's data on the pair") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    const EntityId likes = world.id<Likes>();
    const EntityId relation = world.new_entity();

    SUBCASE("two types") {
        world.set<Likes, Health>(e, Health { 5 });
        CHECK(world.has<Likes, Health>(e));
        Health* health = world.get<Likes, Health>(e);
        REQUIRE(health != nullptr);
        CHECK(health->value == 5);
        CHECK(world.get<ECS::Pair<Likes, Health>>(e) == health);
        CHECK_FALSE(world.has<Health>(e));
    }
    SUBCASE("runtime relation, typed target") {
        world.set_second<Health>(e, relation, Health { 7 });
        CHECK(world.has_second<Health>(e, relation));
        Health* health = world.get_second<Health>(e, relation);
        REQUIRE(health != nullptr);
        CHECK(health->value == 7);
        world.set_second<Health>(e, relation, Health { 8 });
        CHECK(world.get_second<Health>(e, relation)->value == 8);

        // Through the typed tag relation too.
        world.set_second<Health>(e, likes, Health { 9 });
        CHECK(world.get_second<Health>(e, likes)->value == 9);
        CHECK(world.get<Likes, Health>(e)->value == 9);

        world.remove_second<Health>(e, relation);
        CHECK(world.get_second<Health>(e, relation) == nullptr);
        CHECK(world.get_second<Health>(e, likes)->value == 9);
    }
    SUBCASE("set_second is refused when the relation carries data") {
        const EntityId position = world.component<Position>();
        world.set_second<Health>(e, position, Health { 1 });
        CHECK_FALSE(world.has(e, World::pair(position, world.id<Health>())));
        CHECK(world.get_second<Health>(e, position) == nullptr);

        // The pair still exists through the relation's side.
        world.set<Position, Health>(e, Position { 2, 2 });
        CHECK(world.has_second<Health>(e, position));
        CHECK(world.get_second<Health>(e, position) == nullptr);
        CHECK(world.get<Position, Health>(e)->x == 2);
    }
    SUBCASE("add on a data pair zeroes the value") {
        world.add<Likes, Health>(e);
        REQUIRE(world.get<Likes, Health>(e) != nullptr);
        CHECK(world.get<Likes, Health>(e)->value == 0);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/pairs: wildcards match held pairs but cannot be held or written") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    const EntityId bob = world.new_entity();
    const EntityId alice = world.new_entity();
    const Id likes = world.id<Likes>();
    const Id likes_any = ECS::PAIR(likes, ECS::WILDCARD);
    const Id any_bob = ECS::PAIR(ECS::WILDCARD, bob);

    CHECK_FALSE(world.has(e, likes_any));
    world.add<Likes>(e, bob);
    CHECK(world.has(e, likes_any));
    CHECK(world.has(e, any_bob));
    CHECK_FALSE(world.has(e, ECS::PAIR(ECS::WILDCARD, alice)));
    CHECK_FALSE(world.has(e, ECS::PAIR(world.id<Eats>(), ECS::WILDCARD)));

    SUBCASE("a wildcard stays matched while any pair with the relation remains") {
        world.add<Likes>(e, alice);
        world.remove<Likes>(e, bob);
        CHECK(world.has(e, likes_any));
        CHECK_FALSE(world.has(e, any_bob));
        world.remove<Likes>(e, alice);
        CHECK_FALSE(world.has(e, likes_any));
    }
    SUBCASE("adding or setting through a wildcard is a no-op") {
        world.add(e, ECS::PAIR(world.id<Eats>(), ECS::WILDCARD));
        CHECK_FALSE(world.has(e, ECS::PAIR(world.id<Eats>(), ECS::WILDCARD)));

        world.set<Position>(e, bob, Position { 1, 1 });
        const Position value { 9, 9 };
        world.set(e, ECS::PAIR(world.id<Position>(), ECS::WILDCARD), &value);
        CHECK(world.get<Position>(e, bob)->x == 1);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/pairs: non-exclusive relations hold several targets at once") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    const EntityId a = world.new_entity();
    const EntityId b = world.new_entity();
    const EntityId c = world.new_entity();

    world.add<Likes>(e, a);
    world.add<Likes>(e, b);
    world.add<Likes>(e, c);
    CHECK(world.has<Likes>(e, a));
    CHECK(world.has<Likes>(e, b));
    CHECK(world.has<Likes>(e, c));

    world.remove<Likes>(e, b);
    CHECK(world.has<Likes>(e, a));
    CHECK_FALSE(world.has<Likes>(e, b));
    CHECK(world.has<Likes>(e, c));

    // The same for data pairs: each target keeps its own value.
    world.set<Position>(e, a, Position { 1, 0 });
    world.set<Position>(e, b, Position { 2, 0 });
    CHECK(world.get<Position>(e, a)->x == 1);
    CHECK(world.get<Position>(e, b)->x == 2);
    CHECK(world.has<Likes>(e, a));

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/pairs: CHILD_OF is exclusive so a new parent replaces the old") {
    World world;
    world.init();

    const EntityId child = world.new_entity();
    const EntityId parent_a = world.new_entity();
    const EntityId parent_b = world.new_entity();
    world.add<TagA>(child);
    world.set(child, Position { 4, 4 });

    world.add(child, ECS::PAIR(ECS::CHILD_OF, parent_a));
    CHECK(world.has(child, ECS::PAIR(ECS::CHILD_OF, parent_a)));

    world.add(child, ECS::PAIR(ECS::CHILD_OF, parent_b));
    CHECK(world.has(child, ECS::PAIR(ECS::CHILD_OF, parent_b)));
    CHECK_FALSE(world.has(child, ECS::PAIR(ECS::CHILD_OF, parent_a)));
    CHECK(world.has(child, ECS::PAIR(ECS::CHILD_OF, ECS::WILDCARD)));

    // Everything else on the entity survives the swap.
    CHECK(world.has<TagA>(child));
    REQUIRE(world.get<Position>(child) != nullptr);
    CHECK(world.get<Position>(child)->x == 4);

    // Re-adding the current parent changes nothing; swapping back works.
    world.add(child, ECS::PAIR(ECS::CHILD_OF, parent_b));
    CHECK(world.has(child, ECS::PAIR(ECS::CHILD_OF, parent_b)));
    world.add(child, ECS::PAIR(ECS::CHILD_OF, parent_a));
    CHECK(world.has(child, ECS::PAIR(ECS::CHILD_OF, parent_a)));
    CHECK_FALSE(world.has(child, ECS::PAIR(ECS::CHILD_OF, parent_b)));

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/pairs: a relation tagged EXCLUSIVE replaces its target") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    const EntityId a = world.new_entity();
    const EntityId b = world.new_entity();

    SUBCASE("a runtime relation") {
        const EntityId relation = world.new_entity();
        world.add(relation, ECS::EXCLUSIVE);

        world.add(e, World::pair(relation, a));
        world.add(e, World::pair(relation, b));
        CHECK(world.has(e, World::pair(relation, b)));
        CHECK_FALSE(world.has(e, World::pair(relation, a)));
    }
    SUBCASE("a typed tag relation") {
        world.add(world.id<Eats>(), ECS::EXCLUSIVE);

        world.add<Eats>(e, a);
        world.add<Eats>(e, b);
        CHECK(world.has<Eats>(e, b));
        CHECK_FALSE(world.has<Eats>(e, a));
        CHECK_FALSE(world.has<Likes>(e, a));
    }
    SUBCASE("a data relation swaps its target and value") {
        world.add(world.component<Velocity>(), ECS::EXCLUSIVE);

        world.set<Velocity>(e, a, Velocity { 1, 1 });
        world.set<Velocity>(e, b, Velocity { 2, 2 });
        CHECK_FALSE(world.has<Velocity>(e, a));
        CHECK(world.get<Velocity>(e, a) == nullptr);
        REQUIRE(world.get<Velocity>(e, b) != nullptr);
        CHECK(world.get<Velocity>(e, b)->dx == 2);

        // A set on the current target overwrites without swapping.
        world.set<Velocity>(e, b, Velocity { 3, 3 });
        CHECK(world.get<Velocity>(e, b)->dy == 3);
    }
    SUBCASE("other entities are unaffected by one entity's swap") {
        const EntityId relation = world.new_entity();
        world.add(relation, ECS::EXCLUSIVE);
        const EntityId other = world.new_entity();

        world.add(e, World::pair(relation, a));
        world.add(other, World::pair(relation, a));
        world.add(e, World::pair(relation, b));
        CHECK(world.has(other, World::pair(relation, a)));
        CHECK_FALSE(world.has(other, World::pair(relation, b)));
        CHECK(world.has(e, World::pair(relation, b)));
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/pairs: pairs and plain ids coexist and move together") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    const EntityId bob = world.new_entity();

    world.set(e, Position { 1, 1 });
    world.add<Likes>(e, bob);
    world.set<Likes, Health>(e, Health { 3 });
    world.add(e, ECS::PAIR(ECS::CHILD_OF, bob));
    world.add<TagA>(e);

    CHECK(world.has<Position>(e));
    CHECK(world.has<Likes>(e, bob));
    CHECK(world.has<Likes, Health>(e));
    CHECK(world.has(e, ECS::PAIR(ECS::CHILD_OF, bob)));
    CHECK(world.has<TagA>(e));
    CHECK(world.get<Position>(e)->x == 1);
    CHECK(world.get<Likes, Health>(e)->value == 3);

    world.remove<Position>(e);
    world.remove<Likes>(e, bob);
    CHECK(world.has<Likes, Health>(e));
    CHECK(world.get<Likes, Health>(e)->value == 3);
    CHECK(world.has(e, ECS::PAIR(ECS::CHILD_OF, bob)));
    CHECK(world.has<TagA>(e));
    CHECK_FALSE(world.has<Position>(e));
    CHECK_FALSE(world.has(e, ECS::PAIR(world.id<Likes>(), bob)));
    // (Likes, Health) still counts as a (Likes, *) pair.
    CHECK(world.has(e, ECS::PAIR(world.id<Likes>(), ECS::WILDCARD)));

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/pairs: a pair may point at the entity holding it") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    world.add<Likes>(e, e);
    CHECK(world.has<Likes>(e, e));
    CHECK(world.pair_second(world.pair<Likes>(e)) == e);

    world.set<Position>(e, e, Position { 2, 3 });
    CHECK(world.get<Position>(e, e)->y == 3);

    world.remove<Likes>(e, e);
    CHECK_FALSE(world.has<Likes>(e, e));
    CHECK(world.has<Position>(e, e));

    world.free();
    CHECK_ARENA_CLEAN();
}
