#include "support/test_support.hpp"

#include "engine/ecs/query/query.hpp"
#include "engine/ecs/query/query_iter.hpp"
#include "engine/ecs/query/query_term.hpp"

#include <type_traits>

// The trivial query handle: what world.query<Ts...>() returns, how with() /
// without() accumulate ids, the term list it describes, and iteration over
// the world's archetypes (each / iter / begin and the counting utilities).
// The producer underneath has its own tests in query_scan_tests.cpp.

TEST_CASE("ecs/query: the handle is a plain value bound to its world") {
    static_assert(std::is_trivially_copyable_v<Query<Position, Velocity>>);
    static_assert(Query<Position, Velocity>::output_count == 2);
    static_assert(Query<>::output_count == 0);
    static_assert(Query<Position>::max_term_count == 1 + 2 * QUERY_MAX_EXTRA_TERMS);

    World world;
    world.init();

    Query<Position, Velocity> query = world.query<Position, Velocity>();
    CHECK(query.world == &world);
    CHECK(query.flags == QUERY_NONE);
    CHECK(query.with_count == 0);
    CHECK(query.without_count == 0);

    SUBCASE("flags are stored as given") {
        Query<Position> cached = world.query<Position>(QUERY_CACHED);
        CHECK(cached.flags == QUERY_CACHED);
    }

    SUBCASE("copies are independent") {
        Query<Position, Velocity> copy = query;
        copy.with<TagA>();
        CHECK(copy.with_count == 1);
        CHECK(query.with_count == 0);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query: terms lists the outputs, then with(), then without()") {
    World world;
    world.init();

    const EntityId bob = world.new_entity();
    const Id likes_bob = world.pair<Likes>(bob);

    Query<Position, Velocity> query = world.query<Position, Velocity>();
    query.with<TagA, Health>().with(likes_bob).without<TagB>();

    CHECK(query.with_count == 3);
    CHECK(query.without_count == 1);

    QueryTerm terms[Query<Position, Velocity>::max_term_count];
    const usz count = query.terms(terms);
    REQUIRE(count == 6);

    CHECK(terms[0].id == world.id<Position>());
    CHECK(terms[0].flags == TERM_OUTPUT);
    CHECK(terms[1].id == world.id<Velocity>());
    CHECK(terms[1].flags == TERM_OUTPUT);

    CHECK(terms[2].id == world.id<TagA>());
    CHECK(terms[2].flags == 0);
    CHECK(terms[3].id == world.id<Health>());
    CHECK(terms[3].flags == 0);
    CHECK(terms[4].id == likes_bob);
    CHECK(terms[4].flags == 0);

    CHECK(terms[5].id == world.id<TagB>());
    CHECK(terms[5].flags == TERM_EXCLUDE);

    for (usz i = 0; i < count; i++) {
        CHECK(terms[i].is_on_this());
        CHECK(terms[i].src_var == QUERY_THIS);
        CHECK_FALSE(terms[i].binds());
        CHECK_FALSE(terms[i].is_optional());
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query: constraint shapes mirror the World entity operations") {
    World world;
    world.init();

    const EntityId bob = world.new_entity();
    const EntityId apple = world.new_entity();
    const Id likes = world.id<Likes>();
    const Id eats = world.id<Eats>();

    Query<> query = world.query<>();

    SUBCASE("raw ids, several at once") {
        query.with(likes, eats);
        CHECK(query.with_count == 2);
        CHECK(query.with_ids[0] == likes);
        CHECK(query.with_ids[1] == eats);
    }

    SUBCASE("typed relation with a runtime target") {
        query.with<Likes>(bob).without<Eats>(apple);
        CHECK(query.with_ids[0] == ECS::PAIR(likes, bob));
        CHECK(query.without_ids[0] == ECS::PAIR(eats, apple));
    }

    SUBCASE("typed pairs") {
        query.with<ECS::Pair<Likes, Eats>>();
        CHECK(query.with_ids[0] == ECS::PAIR(likes, eats));
    }

    SUBCASE("a 0 id is ignored") {
        query.with(Id { 0 }).without(Id { 0 });
        CHECK(query.with_count == 0);
        CHECK(query.without_count == 0);
    }

    SUBCASE("the output list may be empty") {
        query.with<TagA>();
        QueryTerm terms[Query<>::max_term_count];
        REQUIRE(query.terms(terms) == 1);
        CHECK(terms[0].id == world.id<TagA>());
        CHECK(terms[0].flags == 0);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query: ids past the fixed capacity are dropped with an error") {
    World world;
    world.init();

    Query<Position> query = world.query<Position>();
    Id ids[QUERY_MAX_EXTRA_TERMS + 2];
    for (Id& id : ids) {
        id = world.new_entity();
    }
    for (const Id id : ids) {
        query.with(id);
    }
    CHECK(query.with_count == QUERY_MAX_EXTRA_TERMS);
    for (usz i = 0; i < QUERY_MAX_EXTRA_TERMS; i++) {
        CHECK(query.with_ids[i] == ids[i]);
    }

    // without() has its own capacity.
    query.without(ids[0]);
    CHECK(query.without_count == 1);

    world.free();
    CHECK_ARENA_CLEAN();
}

// --- Iteration ---------------------------------------------------------------

namespace {

// Four entities spread over four archetypes: Position only, Position +
// Velocity, Position + TagA, Velocity only.
struct Sample {
    EntityId position_only;
    EntityId mover;
    EntityId tagged;
    EntityId velocity_only;
};

Sample make_sample(World& world) {
    Sample s { };
    s.position_only = world.new_entity();
    world.set(s.position_only, Position { 1, 10 });

    s.mover = world.new_entity();
    world.set(s.mover, Position { 2, 20 });
    world.set(s.mover, Velocity { 1, 2 });

    s.tagged = world.new_entity();
    world.set(s.tagged, Position { 3, 30 });
    world.add<TagA>(s.tagged);

    s.velocity_only = world.new_entity();
    world.set(s.velocity_only, Velocity { 5, 5 });
    return s;
}

} // namespace

TEST_CASE("ecs/query: each visits every matched entity with its data") {
    World world;
    world.init();
    const Sample s = make_sample(world);

    SUBCASE("one output") {
        usz visited = 0;
        f32 sum = 0;
        world.query<Position>().each([&](const EntityId entity, const Position& position) {
            visited++;
            sum += position.x;
            CHECK(world.has<Position>(entity));
            CHECK(entity != s.velocity_only);
        });
        CHECK(visited == 3);
        CHECK(sum == 6);
    }

    SUBCASE("two outputs, in template order") {
        usz visited = 0;
        world.query<Position, Velocity>().each([&](const EntityId entity, const Position& position, const Velocity& velocity) {
            visited++;
            CHECK(entity == s.mover);
            CHECK(position.x == 2);
            CHECK(velocity.dy == 2);
        });
        CHECK(visited == 1);

        visited = 0;
        world.query<Velocity, Position>().each([&](const EntityId entity, const Velocity& velocity, const Position& position) {
            visited++;
            CHECK(entity == s.mover);
            CHECK(velocity.dx == 1);
            CHECK(position.y == 20);
        });
        CHECK(visited == 1);
    }

    SUBCASE("the callback may take only the data") {
        usz visited = 0;
        world.query<Velocity>().each([&](Velocity& velocity) {
            visited++;
            velocity.dx = 0;
        });
        CHECK(visited == 2);
        CHECK(world.get<Velocity>(s.mover)->dx == 0);
        CHECK(world.get<Velocity>(s.velocity_only)->dx == 0);
    }

    SUBCASE("writes go through to the components") {
        world.query<Position, Velocity>().each([](Position& position, const Velocity& velocity) {
            position.x += velocity.dx;
            position.y += velocity.dy;
        });
        CHECK(world.get<Position>(s.mover)->x == 3);
        CHECK(world.get<Position>(s.mover)->y == 22);
        CHECK(world.get<Position>(s.position_only)->x == 1);
    }

    SUBCASE("no output types") {
        usz visited = 0;
        world.query<>().with<TagA>().each([&](const EntityId entity) {
            visited++;
            CHECK(entity == s.tagged);
        });
        CHECK(visited == 1);
    }

    SUBCASE("nothing matches") {
        usz visited = 0;
        world.query<Health>().each([&](const EntityId, const Health&) { visited++; });
        CHECK(visited == 0);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query: with and without constrain the match") {
    World world;
    world.init();
    const Sample s = make_sample(world);
    const EntityId bob = world.new_entity();
    world.add<Likes>(s.mover, bob);

    usz visited = 0;
    EntityId last = 0;
    const auto visit = [&](const EntityId entity, const Position&) {
        visited++;
        last = entity;
    };

    SUBCASE("with a tag") {
        world.query<Position>().with<TagA>().each(visit);
        CHECK(visited == 1);
        CHECK(last == s.tagged);
    }

    SUBCASE("without a tag") {
        world.query<Position>().without<TagA>().each(visit);
        CHECK(visited == 2);
        CHECK(last != s.tagged);
    }

    SUBCASE("with a component that is not an output") {
        world.query<Position>().with<Velocity>().each(visit);
        CHECK(visited == 1);
        CHECK(last == s.mover);
    }

    SUBCASE("with a pair, concrete and wildcard") {
        world.query<Position>().with<Likes>(bob).each(visit);
        CHECK(visited == 1);
        CHECK(last == s.mover);

        visited = 0;
        world.query<Position>().with(world.pair<Likes>(ECS::WILDCARD)).each(visit);
        CHECK(visited == 1);

        visited = 0;
        world.query<Position>().without(world.pair<Likes>(ECS::WILDCARD)).each(visit);
        CHECK(visited == 2);
    }

    SUBCASE("contradictory constraints match nothing") {
        world.query<Position>().with<TagA>().without<TagA>().each(visit);
        CHECK(visited == 0);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query: a pair output delivers the pair's data") {
    World world;
    world.init();

    // Likes is a tag, so (Likes, Position) stores a Position.
    const EntityId e = world.new_entity();
    world.set<Likes, Position>(e, Position { 4, 5 });
    const EntityId plain = world.new_entity();
    world.set(plain, Position { 1, 1 });

    usz visited = 0;
    world.query<ECS::Pair<Likes, Position>>().each([&](const EntityId entity, Position& position) {
        visited++;
        CHECK(entity == e);
        CHECK(position.y == 5);
        position.y = 6;
    });
    CHECK(visited == 1);
    CHECK(world.get<Likes, Position>(e)->y == 6);
    CHECK(world.get<Position>(plain)->y == 1);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query: iter delivers one chunk per matched archetype") {
    World world;
    world.init();
    const Sample s = make_sample(world);
    const EntityId second_mover = world.new_entity();
    world.set(second_mover, Position { 8, 8 });
    world.set(second_mover, Velocity { 0, 0 });

    usz chunks = 0;
    usz rows = 0;
    world.query<Position>().iter([&](QueryIter& it, Position* positions) {
        chunks++;
        rows += it.count;
        CHECK(it.field_count == 1);
        CHECK(it.term_count == 1);
        CHECK(positions == it.field<0, Position>());
        CHECK(it.archetype != nullptr);
        CHECK(it.archetype->data.entity_count == it.count);
        for (usz row = 0; row < it.count; row++) {
            CHECK(world.get<Position>(it.entities[row]) == &positions[row]);
        }
    });
    CHECK(chunks == 3);
    CHECK(rows == 4);

    SUBCASE("the chunk of two movers") {
        world.query<Position, Velocity>().iter([&](QueryIter& it, Position* positions, Velocity* velocities) {
            CHECK(it.count == 2);
            CHECK(it.ids[0] == world.id<Position>());
            CHECK(it.ids[1] == world.id<Velocity>());
            CHECK((it.entities[0] == s.mover || it.entities[1] == s.mover));
            for (usz row = 0; row < it.count; row++) {
                positions[row].x += velocities[row].dx;
            }
        });
        CHECK(world.get<Position>(s.mover)->x == 3);
        CHECK(world.get<Position>(second_mover)->x == 8);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query: begin hands out a cursor on the caller's allocator") {
    World world;
    world.init();
    const Sample s = make_sample(world);
    (void)s;

    Query<Position> query = world.query<Position>().without<TagA>();

    {
        TemporalAllocator temp = TemporalAllocator::create();
        QueryIter it = query.begin(&temp);
        CHECK(it.world == &world);
        CHECK(it.field_count == 1);
        CHECK(it.term_count == 2);

        usz rows = 0;
        while (it.next(&it)) {
            CHECK(it.ids[0] == world.id<Position>());
            CHECK(it.ids[1] == 0);
            rows += it.count;
        }
        CHECK(rows == 2);
        CHECK_FALSE(it.next(&it));

        // The handle is untouched by iterating and can be reused.
        QueryIter again = query.begin(&temp);
        CHECK(QUERY::count(again) == 2);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query: count, empty, first and random") {
    World world;
    world.init();
    const Sample s = make_sample(world);

    Query<Position> positions = world.query<Position>();
    Query<Health> none = world.query<Health>();

    CHECK(positions.count() == 3);
    CHECK_FALSE(positions.empty());
    CHECK(none.count() == 0);
    CHECK(none.empty());
    CHECK(none.first() == 0);

    const EntityId first = positions.first();
    CHECK(first != 0);
    CHECK(world.has<Position>(first));

    u64 rng = 42;
    bool seen[3] = {};
    for (usz i = 0; i < 200; i++) {
        const EntityId picked = positions.random(rng);
        REQUIRE(picked != 0);
        CHECK(world.has<Position>(picked));
        if (picked == s.position_only) seen[0] = true;
        if (picked == s.mover) seen[1] = true;
        if (picked == s.tagged) seen[2] = true;
    }
    CHECK(seen[0]);
    CHECK(seen[1]);
    CHECK(seen[2]);
    CHECK(none.random(rng) == 0);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query: matches tells whether one entity is in the result set") {
    World world;
    world.init();
    const Sample s = make_sample(world);
    const EntityId empty = world.new_entity();

    Query<Position> moving = world.query<Position>().with<Velocity>();
    CHECK(moving.matches(s.mover));
    CHECK_FALSE(moving.matches(s.position_only));
    CHECK_FALSE(moving.matches(s.velocity_only));
    CHECK_FALSE(moving.matches(empty));
    CHECK_FALSE(moving.matches(0));

    world.remove<Velocity>(s.mover);
    CHECK_FALSE(moving.matches(s.mover));

    // An entity with no ids is never matched, not even by a pure exclusion.
    CHECK_FALSE(world.query<>().without<TagA>().matches(empty));
    CHECK(world.query<>().without<TagA>().matches(s.position_only));

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query: the handle keeps matching as the world changes") {
    World world;
    world.init();

    Query<Position> positions = world.query<Position>();
    CHECK(positions.count() == 0);

    const EntityId a = world.new_entity();
    world.set(a, Position { });
    CHECK(positions.count() == 1);

    // A new archetype after the first run is picked up by the next one.
    const EntityId b = world.new_entity();
    world.set(b, Position { });
    world.set(b, Health { });
    CHECK(positions.count() == 2);

    world.delete_entity(a);
    CHECK(positions.count() == 1);
    CHECK(positions.first() == b);

    world.free();
    CHECK_ARENA_CLEAN();
}
