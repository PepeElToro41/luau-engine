#include "support/test_support.hpp"

#include "engine/ecs/hierarchy.hpp"
#include "engine/ecs/query/dynamic_query.hpp"
#include "engine/ecs/query/query_builder.hpp"
#include "engine/ecs/query/query_iter.hpp"

// up() through the hierarchy's reachable cache: the results follow changes
// to the ancestors, a wildcard binds the matches of one ancestor only,
// without() and or-chains read the same cache, and several bases keep the
// depth-first order. The general up() behaviour is covered next to the
// other term shapes in dynamic_query_tests.cpp.

namespace {

struct Seen {
    DynamicArray<EntityId> entities;
    DynamicArray<EntityId> vars;
    DynamicArray<EntityId> sources;

    void free() {
        this->entities.free();
        this->vars.free();
        this->sources.free();
    }
};

// Every row the query yields with the variable and the source of term `term`.
Seen collect(DynamicQuery& query, const QueryVar var, const usz term) {
    Seen seen;
    TemporalAllocator temp = TemporalAllocator::create();
    QueryIter it = query.begin(&temp);
    while (it.next(&it)) {
        for (usz row = 0; row < it.count; row++) {
            seen.entities.push(it.entities[row]);
            seen.vars.push(var.is_set() ? it.vars[var.index] : 0);
            seen.sources.push(it.sources[term]);
        }
    }
    return seen;
}

bool has(const DynamicArray<EntityId>& list, const EntityId entity) {
    for (const EntityId e : list) {
        if (e == entity) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("ecs/dynamic_query: up() follows the ancestors as they change") {
    World world;
    world.init();

    const EntityId grandparent = world.new_entity();
    const EntityId parent = world.new_entity();
    world.add(parent, world.pair(ECS::CHILD_OF, grandparent));
    const EntityId child = world.new_entity();
    world.add(child, world.pair(ECS::CHILD_OF, parent));
    world.set<Velocity>(child, { 1, 0 });

    QueryBuilder builder = world.query_build();
    builder.term<Velocity>().term<Position>().up();
    DynamicQuery query = builder.build();
    REQUIRE(query.is_ok());

    // Nobody above has a Position yet.
    CHECK(query.count() == 0);
    CHECK_FALSE(query.matches(child));

    // The grandparent gets one: found two levels up.
    world.set<Position>(grandparent, { 100, 0 });
    CHECK(query.count() == 1);
    CHECK(query.matches(child));
    Seen seen = collect(query, QueryVar { }, 1);
    REQUIRE(seen.entities.count == 1);
    CHECK(seen.sources[0] == grandparent);
    seen.free();
    query.each<Velocity, Position>([&](EntityId, Velocity& velocity, Position& position) {
        CHECK(position.x == 100);
        velocity.dx = position.x;
    });
    CHECK(world.get<Velocity>(child)->dx == 100);

    // The parent gets one too: the nearer one wins.
    world.set<Position>(parent, { 50, 0 });
    seen = collect(query, QueryVar { }, 1);
    REQUIRE(seen.entities.count == 1);
    CHECK(seen.sources[0] == parent);
    seen.free();

    // Removed from the parent again: back to the grandparent; removed from
    // both: nothing.
    world.remove<Position>(parent);
    seen = collect(query, QueryVar { }, 1);
    REQUIRE(seen.entities.count == 1);
    CHECK(seen.sources[0] == grandparent);
    seen.free();
    world.remove<Position>(grandparent);
    CHECK(query.count() == 0);

    // Reparenting the child under an entity with a Position.
    const EntityId other = world.new_entity();
    world.set<Position>(other, { 7, 0 });
    world.add(child, world.pair(ECS::CHILD_OF, other));
    seen = collect(query, QueryVar { }, 1);
    REQUIRE(seen.entities.count == 1);
    CHECK(seen.sources[0] == other);
    seen.free();

    // The source's data is read live: changing it is seen without a move.
    world.get<Position>(other)->x = 8;
    query.each<Velocity, Position>([&](EntityId, Velocity&, Position& position) {
        CHECK(position.x == 8);
    });

    query.free();
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/dynamic_query: up() with a wildcard binds every match of the nearest ancestor only") {
    World world;
    world.init();

    const EntityId apple = world.new_entity();
    const EntityId pear = world.new_entity();
    const EntityId kiwi = world.new_entity();
    const EntityId grandparent = world.new_entity();
    world.add<Likes>(grandparent, kiwi);
    const EntityId parent = world.new_entity();
    world.add(parent, world.pair(ECS::CHILD_OF, grandparent));
    world.add<Likes>(parent, apple);
    world.add<Likes>(parent, pear);
    const EntityId child = world.new_entity();
    world.add(child, world.pair(ECS::CHILD_OF, parent));
    world.set<Velocity>(child, { 1, 0 });

    QueryBuilder builder = world.query_build();
    const QueryVar food = builder.var("food");
    builder.term<Velocity>().with<Likes>(food).up();
    DynamicQuery query = builder.build();
    REQUIRE(query.is_ok());

    // The parent is the nearest ancestor liking anything: apple and pear,
    // not the grandparent's kiwi.
    Seen seen = collect(query, food, 1);
    CHECK(seen.entities.count == 2);
    CHECK(has(seen.vars, apple));
    CHECK(has(seen.vars, pear));
    CHECK_FALSE(has(seen.vars, kiwi));
    for (const EntityId source : seen.sources) {
        CHECK(source == parent);
    }
    seen.free();

    // Without the parent's pairs, the grandparent's kiwi.
    world.remove<Likes>(parent, apple);
    world.remove<Likes>(parent, pear);
    seen = collect(query, food, 1);
    REQUIRE(seen.entities.count == 1);
    CHECK(seen.vars[0] == kiwi);
    CHECK(seen.sources[0] == grandparent);
    seen.free();

    // A concrete pair on the grandparent while the parent holds another:
    // the concrete term reaches past the parent.
    world.add<Likes>(parent, apple);
    QueryBuilder b2 = world.query_build();
    b2.term<Velocity>().with<Likes>(kiwi).up();
    DynamicQuery q2 = b2.build();
    REQUIRE(q2.is_ok());
    Seen s2 = collect(q2, QueryVar { }, 1);
    REQUIRE(s2.entities.count == 1);
    CHECK(s2.sources[0] == grandparent);
    s2.free();
    q2.free();
    b2.free();

    query.free();
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/dynamic_query: without().up() and or-chains with up() read the same cache") {
    World world;
    world.init();

    const EntityId grandparent = world.new_entity();
    const EntityId parent = world.new_entity();
    world.add(parent, world.pair(ECS::CHILD_OF, grandparent));
    const EntityId child = world.new_entity();
    world.add(child, world.pair(ECS::CHILD_OF, parent));
    world.set<Velocity>(child, { 1, 0 });
    const EntityId orphan = world.new_entity();
    world.set<Velocity>(orphan, { 2, 0 });
    world.add<TagA>(orphan);

    QueryBuilder without_builder = world.query_build();
    without_builder.term<Velocity>().without<Position>().up();
    DynamicQuery without = without_builder.build();
    REQUIRE(without.is_ok());
    CHECK(without.count() == 2);
    world.set<Position>(grandparent, { 1, 0 });
    CHECK(without.count() == 1);
    CHECK(without.matches(orphan));
    CHECK_FALSE(without.matches(child));
    world.remove<Position>(grandparent);
    CHECK(without.count() == 2);

    // Position above, or TagA on the entity itself.
    QueryBuilder or_builder = world.query_build();
    or_builder.term<Velocity>().with<Position>().up().bor().with<TagA>();
    DynamicQuery either = or_builder.build();
    REQUIRE(either.is_ok());
    CHECK(either.count() == 1);
    CHECK(either.matches(orphan));
    world.set<Position>(parent, { 3, 0 });
    CHECK(either.count() == 2);
    Seen seen = collect(either, QueryVar { }, 1);
    for (usz i = 0; i < seen.entities.count; i++) {
        if (seen.entities[i] == child) {
            CHECK(seen.sources[i] == parent);
        } else {
            CHECK(seen.entities[i] == orphan);
            CHECK(seen.sources[i] == 0);
        }
    }
    seen.free();

    either.free();
    or_builder.free();
    without.free();
    without_builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/dynamic_query: up() over several bases takes the first base's chain first") {
    World world;
    world.init();

    const EntityId base_root = world.new_entity();
    world.set<Health>(base_root, { 5 });
    const EntityId first = world.new_entity();
    world.add(first, world.pair(ECS::IS_A, base_root));
    const EntityId second = world.new_entity();
    world.set<Health>(second, { 7 });
    const EntityId e = world.new_entity();
    world.add(e, world.pair(ECS::IS_A, first));
    world.add(e, world.pair(ECS::IS_A, second));
    world.set<Velocity>(e, { 1, 0 });

    QueryBuilder builder = world.query_build();
    builder.term<Velocity>().term<Health>().up(ECS::IS_A);
    DynamicQuery query = builder.build();
    REQUIRE(query.is_ok());

    // first's chain is walked before second, so base_root's Health wins
    // over second's even though second is nearer.
    Seen seen = collect(query, QueryVar { }, 1);
    REQUIRE(seen.entities.count == 1);
    CHECK(seen.sources[0] == base_root);
    seen.free();
    query.each<Velocity, Health>([](EntityId, Velocity&, Health& health) {
        CHECK(health.value == 5);
    });

    // Dropping the first base leaves second.
    world.remove(e, world.pair(ECS::IS_A, first));
    seen = collect(query, QueryVar { }, 1);
    REQUIRE(seen.entities.count == 1);
    CHECK(seen.sources[0] == second);
    seen.free();

    query.free();
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}
