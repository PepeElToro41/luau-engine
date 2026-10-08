#include "support/test_support.hpp"

#include "engine/ecs/component_record.hpp"
#include "engine/ecs/query_iter.hpp"
#include "engine/ecs/query_scan.hpp"
#include "engine/ecs/query_term.hpp"

// The uncached producer: what QUERY_SCAN::begin() yields for a raw term
// list. Query<Ts...> builds on it; its tests cover the typed surface.

TEST_CASE("ecs/query_scan: ids reports the concrete id per term and 0 for excluded ones") {
    World world;
    world.init();

    const EntityId bob = world.new_entity();
    const EntityId e = world.new_entity();
    world.set(e, Position { 1, 2 });
    world.add<Likes>(e, bob);

    const EntityId other = world.new_entity();
    world.set(other, Position { 3, 4 });
    world.add<Likes>(other, bob);
    world.add<TagB>(other);

    const QueryTerm terms[] = {
        QueryTerm::make(world.id<Position>(), TERM_OUTPUT),
        QueryTerm::make(world.pair<Likes>(ECS::WILDCARD), 0),
        QueryTerm::make(world.id<TagB>(), TERM_EXCLUDE),
    };

    {
        TemporalAllocator temp = TemporalAllocator::create();
        QueryIter it = QUERY_SCAN::begin(&world, terms, 3, &temp);
        CHECK(it.world == &world);
        CHECK(it.field_count == 1);
        CHECK(it.term_count == 3);
        CHECK(it.var_count == 1);

        REQUIRE(it.next(&it));
        CHECK(it.count == 1);
        CHECK(it.entities[0] == e);
        CHECK(it.archetype == world.entity_index.get_record_alive(e)->archetype);
        CHECK(it.ids[0] == world.id<Position>());
        CHECK(it.ids[1] == world.pair<Likes>(bob));
        CHECK(it.ids[2] == 0);
        CHECK(it.field<0, Position>()[0].x == 1);

        CHECK_FALSE(it.next(&it));
        CHECK(it.count == 0);
        CHECK(it.entities == nullptr);
        CHECK(it.archetype == nullptr);
        CHECK_FALSE(it.next(&it));
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query_scan: a wildcard output takes the first matching column") {
    World world;
    world.init();

    const EntityId low = world.new_entity();
    const EntityId high = world.new_entity();
    REQUIRE(low < high);

    // Health is a component, so (Health, target) stores a Health.
    const EntityId e = world.new_entity();
    world.set<Health>(e, high, Health { 7 });
    world.set<Health>(e, low, Health { 5 });

    const QueryTerm terms[] = {
        QueryTerm::make(world.pair<Health>(ECS::WILDCARD), TERM_OUTPUT),
    };

    {
        TemporalAllocator temp = TemporalAllocator::create();
        QueryIter it = QUERY_SCAN::begin(&world, terms, 1, &temp);
        REQUIRE(it.next(&it));
        CHECK(it.count == 1);
        // Pairs sort by target, so the lowest target comes first.
        CHECK(it.ids[0] == world.pair<Health>(low));
        CHECK(it.field<0, Health>()[0].value == 5);
        CHECK_FALSE(it.next(&it));

        SUBCASE("ANY is folded to WILDCARD") {
            const QueryTerm any[] = { QueryTerm::make(world.pair<Health>(ECS::ANY), TERM_OUTPUT) };
            QueryIter it_any = QUERY_SCAN::begin(&world, any, 1, &temp);
            REQUIRE(it_any.next(&it_any));
            CHECK(it_any.ids[0] == world.pair<Health>(low));
        }

        SUBCASE("(*, *) matches any pair and reports the first one") {
            const QueryTerm any_pair[] = { QueryTerm::make(ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD), 0) };
            QueryIter it_pair = QUERY_SCAN::begin(&world, any_pair, 1, &temp);
            usz chunks = 0;
            bool saw_e = false;
            while (it_pair.next(&it_pair)) {
                chunks++;
                for (usz row = 0; row < it_pair.count; row++) {
                    if (it_pair.entities[row] == e) {
                        saw_e = true;
                        CHECK(ECS::IS_PAIR(it_pair.ids[0]));
                    }
                }
            }
            // Built-in entities hold (ON_DELETE, PANIC), so there are other chunks.
            CHECK(chunks >= 2);
            CHECK(saw_e);
        }
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query_scan: an unsupported term yields nothing") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    world.set(e, Position { });

    {
        TemporalAllocator temp = TemporalAllocator::create();

        SUBCASE("optional") {
            const QueryTerm terms[] = { QueryTerm::make(world.id<Position>(), TERM_OUTPUT | TERM_OPTIONAL) };
            QueryIter it = QUERY_SCAN::begin(&world, terms, 1, &temp);
            CHECK_FALSE(it.next(&it));
            CHECK_FALSE(it.next(&it));
            CHECK(it.count == 0);
        }

        SUBCASE("another source") {
            QueryTerm term = QueryTerm::make(world.id<Position>(), TERM_OUTPUT);
            term.src_var = QueryVar { };
            term.src = e;
            QueryIter it = QUERY_SCAN::begin(&world, &term, 1, &temp);
            CHECK_FALSE(it.next(&it));
        }

        SUBCASE("traversal") {
            QueryTerm term = QueryTerm::make(world.id<Position>(), TERM_OUTPUT | TERM_UP);
            term.traverse = ECS::CHILD_OF;
            QueryIter it = QUERY_SCAN::begin(&world, &term, 1, &temp);
            CHECK_FALSE(it.next(&it));
        }

        SUBCASE("a 0 id") {
            const QueryTerm terms[] = { QueryTerm::make(0, TERM_OUTPUT) };
            QueryIter it = QUERY_SCAN::begin(&world, terms, 1, &temp);
            CHECK_FALSE(it.next(&it));
            CHECK_FALSE(QUERY_SCAN::matches(&world, terms, 1, e));
        }
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query_scan: empty archetypes are skipped") {
    World world;
    world.init();

    const EntityId a = world.new_entity();
    const EntityId b = world.new_entity();
    world.set(a, Position { });
    world.set(b, Position { });
    world.set(b, Velocity { });
    // [Position, Velocity] now exists but is empty.
    world.remove<Velocity>(b);

    const QueryTerm terms[] = { QueryTerm::make(world.id<Position>(), TERM_OUTPUT) };
    {
        TemporalAllocator temp = TemporalAllocator::create();
        QueryIter it = QUERY_SCAN::begin(&world, terms, 1, &temp);
        REQUIRE(it.next(&it));
        CHECK(it.count == 2);
        CHECK_FALSE(it.next(&it));
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query_scan: archetypes created after begin() are not visited") {
    World world;
    world.init();

    const EntityId a = world.new_entity();
    world.set(a, Position { });

    const QueryTerm terms[] = { QueryTerm::make(world.id<Position>(), TERM_OUTPUT) };
    {
        TemporalAllocator temp = TemporalAllocator::create();
        QueryIter it = QUERY_SCAN::begin(&world, terms, 1, &temp);

        // A new table matching the query, created mid-walk.
        const EntityId b = world.new_entity();
        world.set(b, Position { });
        world.set(b, Health { });

        usz seen = 0;
        while (it.next(&it)) {
            seen += it.count;
        }
        CHECK(seen == 1);

        // A fresh walk sees both.
        QueryIter again = QUERY_SCAN::begin(&world, terms, 1, &temp);
        CHECK(QUERY::count(again) == 2);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query_scan: matches tests a single entity's archetype") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    world.set(e, Position { });
    const EntityId empty = world.new_entity();
    const EntityId dead = world.new_entity();
    world.delete_entity(dead);

    const QueryTerm position[] = { QueryTerm::make(world.id<Position>(), TERM_OUTPUT) };
    const QueryTerm not_position[] = { QueryTerm::make(world.id<Position>(), TERM_EXCLUDE) };

    CHECK(QUERY_SCAN::matches(&world, position, 1, e));
    CHECK_FALSE(QUERY_SCAN::matches(&world, not_position, 1, e));
    // The root is never scanned, so an entity with no ids matches nothing,
    // not even a pure exclusion.
    CHECK_FALSE(QUERY_SCAN::matches(&world, not_position, 1, empty));
    CHECK_FALSE(QUERY_SCAN::matches(&world, position, 1, dead));
    CHECK_FALSE(QUERY_SCAN::matches(&world, position, 1, 0));

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query_scan: the walk is narrowed to the archetypes of the rarest with-term") {
    World world;
    world.init();

    // Many Position tables, one of which also carries Health.
    EntityId rare = 0;
    for (usz i = 0; i < 8; i++) {
        const EntityId e = world.new_entity();
        world.set(e, Position { static_cast<f32>(i), 0 });
        if (i & 1) world.add<TagA>(e);
        if (i & 2) world.add<TagB>(e);
        if (i == 5) {
            world.set(e, Health { 7 });
            rare = e;
        }
    }
    // Health is also held by an entity without Position, which must not match.
    const EntityId health_only = world.new_entity();
    world.set(health_only, Health { 1 });

    // Health is held by fewer archetypes than Position (the tables entities
    // pass through on the way count too), so it is the one narrowing the walk.
    const Id position = world.id<Position>();
    const Id health = world.id<Health>();
    const ComponentRecord* health_record = ComponentRecord::component_record_find(&world, health);
    const ComponentRecord* position_record = ComponentRecord::component_record_find(&world, position);
    REQUIRE(health_record != nullptr);
    REQUIRE(position_record != nullptr);
    REQUIRE(health_record->archetype_count() < position_record->archetype_count());

    SUBCASE("the result is the same whichever term comes first") {
        const QueryTerm health_first[] = {
            QueryTerm::make(health, TERM_OUTPUT),
            QueryTerm::make(position, 0),
        };
        const QueryTerm position_first[] = {
            QueryTerm::make(position, TERM_OUTPUT),
            QueryTerm::make(health, 0),
        };
        TemporalAllocator temp = TemporalAllocator::create();
        for (const QueryTerm* terms : { health_first, position_first }) {
            QueryIter it = QUERY_SCAN::begin(&world, terms, 2, &temp);
            REQUIRE(it.next(&it));
            CHECK(it.count == 1);
            CHECK(it.entities[0] == rare);
            CHECK_FALSE(it.next(&it));
        }
    }

    SUBCASE("a with-term held by no archetype yields nothing") {
        // Registered, so the id is valid, but never added to an entity.
        const Id velocity = world.id<Velocity>();
        CHECK(ComponentRecord::component_record_find(&world, velocity) == nullptr);
        const QueryTerm terms[] = {
            QueryTerm::make(position, TERM_OUTPUT),
            QueryTerm::make(velocity, 0),
        };
        TemporalAllocator temp = TemporalAllocator::create();
        QueryIter it = QUERY_SCAN::begin(&world, terms, 2, &temp);
        CHECK(it.term_count == 2);
        CHECK_FALSE(it.next(&it));
    }

    SUBCASE("a without-term never narrows the walk") {
        const QueryTerm terms[] = {
            QueryTerm::make(position, TERM_OUTPUT),
            QueryTerm::make(health, TERM_EXCLUDE),
        };
        TemporalAllocator temp = TemporalAllocator::create();
        QueryIter it = QUERY_SCAN::begin(&world, terms, 2, &temp);
        CHECK(QUERY::count(it) == 7);
    }

    SUBCASE("an excluded id held by no archetype changes nothing") {
        const QueryTerm terms[] = {
            QueryTerm::make(position, TERM_OUTPUT),
            QueryTerm::make(world.id<Velocity>(), TERM_EXCLUDE),
        };
        TemporalAllocator temp = TemporalAllocator::create();
        QueryIter it = QUERY_SCAN::begin(&world, terms, 2, &temp);
        CHECK(QUERY::count(it) == 8);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query_scan: wildcard terms narrow through their alias record or fall back to every archetype") {
    World world;
    world.init();

    const EntityId bob = world.new_entity();
    const EntityId alice = world.new_entity();
    const EntityId likes_bob = world.new_entity();
    world.add<Likes>(likes_bob, bob);
    const EntityId likes_alice = world.new_entity();
    world.add<Likes>(likes_alice, alice);
    world.set(likes_alice, Position { });
    const EntityId eats_bob = world.new_entity();
    world.add<Eats>(eats_bob, bob);
    const EntityId plain = world.new_entity();
    world.set(plain, Position { });

    SUBCASE("(R, *) walks the relation's alias record") {
        const QueryTerm terms[] = { QueryTerm::make(world.pair<Likes>(ECS::WILDCARD), 0) };
        TemporalAllocator temp = TemporalAllocator::create();
        QueryIter it = QUERY_SCAN::begin(&world, terms, 1, &temp);
        CHECK(QUERY::count(it) == 2);
    }

    SUBCASE("(*, T) walks the target's alias record") {
        const QueryTerm terms[] = { QueryTerm::make(ECS::PAIR(ECS::WILDCARD, bob), 0) };
        TemporalAllocator temp = TemporalAllocator::create();
        QueryIter it = QUERY_SCAN::begin(&world, terms, 1, &temp);
        CHECK(QUERY::count(it) == 2);
    }

    SUBCASE("(R, ANY) folds onto the same record") {
        const QueryTerm terms[] = { QueryTerm::make(world.pair<Likes>(ECS::ANY), 0) };
        TemporalAllocator temp = TemporalAllocator::create();
        QueryIter it = QUERY_SCAN::begin(&world, terms, 1, &temp);
        CHECK(QUERY::count(it) == 2);
    }

    SUBCASE("(*, *) alone has no record and scans every archetype") {
        // The world's built-in entities hold pairs too, so look for ours
        // instead of counting.
        const QueryTerm terms[] = { QueryTerm::make(ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD), 0) };
        TemporalAllocator temp = TemporalAllocator::create();
        QueryIter it = QUERY_SCAN::begin(&world, terms, 1, &temp);
        bool seen[3] = {};
        while (it.next(&it)) {
            for (usz row = 0; row < it.count; row++) {
                if (it.entities[row] == likes_bob) seen[0] = true;
                if (it.entities[row] == likes_alice) seen[1] = true;
                if (it.entities[row] == eats_bob) seen[2] = true;
                CHECK(it.entities[row] != plain);
            }
        }
        CHECK(seen[0]);
        CHECK(seen[1]);
        CHECK(seen[2]);
    }

    SUBCASE("(*, *) next to a concrete term is narrowed by the concrete one") {
        const QueryTerm terms[] = {
            QueryTerm::make(world.id<Position>(), TERM_OUTPUT),
            QueryTerm::make(ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD), 0),
        };
        TemporalAllocator temp = TemporalAllocator::create();
        QueryIter it = QUERY_SCAN::begin(&world, terms, 2, &temp);
        REQUIRE(it.next(&it));
        CHECK(it.count == 1);
        CHECK(it.entities[0] == likes_alice);
        CHECK_FALSE(it.next(&it));
    }

    SUBCASE("a pair held by no archetype yields nothing") {
        const QueryTerm terms[] = { QueryTerm::make(world.pair<Eats>(alice), 0) };
        TemporalAllocator temp = TemporalAllocator::create();
        QueryIter it = QUERY_SCAN::begin(&world, terms, 1, &temp);
        CHECK(QUERY::count(it) == 0);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}
