#include "support/test_support.hpp"

#include "engine/ecs/query/dynamic_query.hpp"
#include "engine/ecs/query/query_builder.hpp"
#include "engine/ecs/query/query_iter.hpp"
#include "engine/ecs/query/query_term.hpp"

#include <cstring>
#include <utility>

// The engine-evaluated query end to end: QueryBuilder::build() into a
// DynamicQuery and the VM's answers for every term shape: plain terms,
// variables bound from pair sides, variables and fixed entities as sources,
// traversal, not-terms, optionals, or-chains, and the iteration front ends.

namespace {

struct Seen {
    DynamicArray<EntityId> entities;
    DynamicArray<EntityId> vars;

    bool has(const EntityId entity) const {
        for (const EntityId e : this->entities) {
            if (e == entity) {
                return true;
            }
        }
        return false;
    }
    void free() {
        this->entities.free();
        this->vars.free();
    }
};

// Every (row entity, bound variable) pair the query yields.
Seen collect(DynamicQuery& query, const QueryVar var = QueryVar { }) {
    Seen seen;
    TemporalAllocator temp = TemporalAllocator::create();
    QueryIter it = query.begin(&temp);
    while (it.next(&it)) {
        for (usz row = 0; row < it.count; row++) {
            seen.entities.push(it.entities[row]);
            seen.vars.push(var.is_set() ? it.vars[var.index] : 0);
        }
    }
    return seen;
}

} // namespace

TEST_CASE("ecs/dynamic_query: build consumes the builder and plain terms match like Query<Ts...>") {
    World world;
    world.init();

    const EntityId a = world.new_entity();
    world.set<Position>(a, { 1, 1 });
    world.set<Velocity>(a, { 2, 2 });
    const EntityId b = world.new_entity();
    world.set<Position>(b, { 3, 3 });
    world.set<Velocity>(b, { 4, 4 });
    world.add<TagA>(b);
    const EntityId c = world.new_entity();
    world.set<Position>(c, { 5, 5 });

    QueryBuilder builder = world.query_build(QUERY_CACHED);
    builder.term<Position>().term<Velocity>().without<TagA>();
    DynamicQuery query = builder.build();

    CHECK(builder.terms.count == 0);
    CHECK(builder.var_count() == 1);
    REQUIRE(query.is_ok());
    CHECK(query.world == &world);
    CHECK(query.flags == QUERY_CACHED);
    CHECK(query.field_count() == 2);
    CHECK(query.term_count() == 3);
    CHECK(query.var_count() == 1);

    CHECK(query.count() == 1);
    CHECK_FALSE(query.empty());
    CHECK(query.first() == a);
    CHECK(query.matches(a));
    CHECK_FALSE(query.matches(b));
    CHECK_FALSE(query.matches(c));
    CHECK_FALSE(query.matches(world.new_entity()));

    usz calls = 0;
    query.each<Position, Velocity>([&](const EntityId entity, Position& position, Velocity& velocity) {
        CHECK(entity == a);
        CHECK(position.x == 1);
        CHECK(velocity.dx == 2);
        position.x += velocity.dx;
        calls++;
    });
    CHECK(calls == 1);
    CHECK(world.get<Position>(a)->x == 3);

    usz chunks = 0;
    query.iter<Position, Velocity>([&](QueryIter& it, Position* positions, Velocity* velocities) {
        CHECK(it.count == 1);
        CHECK_FALSE(it.shared[0]);
        CHECK_FALSE(it.shared[1]);
        CHECK(it.ids[0] == world.id<Position>());
        CHECK(it.ids[1] == world.id<Velocity>());
        CHECK(it.ids[2] == 0);
        CHECK(it.sources[0] == 0);
        CHECK(positions[0].x == 3);
        CHECK(velocities[0].dx == 2);
        chunks++;
    });
    CHECK(chunks == 1);

    SUBCASE("the same query as a Query<Ts...> agrees") {
        CHECK(world.query<Position, Velocity>().without<TagA>().count() == query.count());
    }

    SUBCASE("a query with the wrong number of output types iterates nothing") {
        usz n = 0;
        query.each<Position>([&](EntityId, Position&) { n++; });
        CHECK(n == 0);
    }

    query.free();
    CHECK_FALSE(query.is_ok());
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/dynamic_query: a pair target variable yields one chunk per matching pair") {
    World world;
    world.init();

    const EntityId apple = world.new_entity();
    const EntityId pear = world.new_entity();
    const EntityId alice = world.new_entity();
    world.set<Position>(alice, { 1, 0 });
    world.add<Likes>(alice, apple);
    world.add<Likes>(alice, pear);
    const EntityId bob = world.new_entity();
    world.set<Position>(bob, { 2, 0 });
    world.add<Likes>(bob, apple);
    const EntityId carol = world.new_entity();
    world.set<Position>(carol, { 3, 0 });

    QueryBuilder builder = world.query_build();
    const QueryVar food = builder.var("food");
    builder.term<Position>().with<Likes>(food);
    DynamicQuery query = builder.build();
    REQUIRE(query.is_ok());
    CHECK(query.var("food") == food);
    CHECK(std::strcmp(query.var_name(food), "food") == 0);
    CHECK(query.var("this") == QUERY_THIS);
    CHECK_FALSE(query.var("nope").is_set());

    Seen seen = collect(query, food);
    REQUIRE(seen.entities.count == 3);
    // alice twice (apple, pear), bob once, carol never.
    usz alice_apple = 0;
    usz alice_pear = 0;
    usz bob_apple = 0;
    for (usz i = 0; i < seen.entities.count; i++) {
        if (seen.entities[i] == alice && seen.vars[i] == apple) alice_apple++;
        if (seen.entities[i] == alice && seen.vars[i] == pear) alice_pear++;
        if (seen.entities[i] == bob && seen.vars[i] == apple) bob_apple++;
    }
    CHECK(alice_apple == 1);
    CHECK(alice_pear == 1);
    CHECK(bob_apple == 1);
    CHECK_FALSE(seen.has(carol));
    seen.free();

    // The matched id is the concrete pair.
    {
        TemporalAllocator temp = TemporalAllocator::create();
        QueryIter it = query.begin(&temp);
        while (it.next(&it)) {
            CHECK(it.ids[1] == ECS::PAIR(world.id<Likes>(), it.vars[food.index]));
            CHECK(it.sources[1] == 0);
        }
    }

    CHECK(query.count() == 3);
    CHECK(query.matches(alice));
    CHECK(query.matches(bob));
    CHECK_FALSE(query.matches(carol));

    SUBCASE("two variables are the product of their pairs") {
        world.add<Eats>(alice, apple);
        world.add<Eats>(alice, pear);
        QueryBuilder b2 = world.query_build();
        const QueryVar f = b2.var("f");
        const QueryVar g = b2.var("g");
        b2.with<Likes>(f).with<Eats>(g);
        DynamicQuery q2 = b2.build();
        REQUIRE(q2.is_ok());
        // alice: 2 likes x 2 eats; bob has no Eats.
        CHECK(q2.count() == 4);
        usz same = 0;
        TemporalAllocator t2 = TemporalAllocator::create();
        QueryIter it2 = q2.begin(&t2);
        while (it2.next(&it2)) {
            CHECK(it2.entities[0] == alice);
            if (it2.vars[f.index] == it2.vars[g.index]) {
                same++;
            }
        }
        CHECK(same == 2);
        q2.free();
        b2.free();
    }

    SUBCASE("a relation variable binds from the pair's first side") {
        QueryBuilder b2 = world.query_build();
        const QueryVar rel = b2.var("rel");
        b2.with(rel, apple);
        DynamicQuery q2 = b2.build();
        REQUIRE(q2.is_ok());
        Seen s2 = collect(q2, rel);
        CHECK(s2.entities.count == 2);
        for (usz i = 0; i < s2.entities.count; i++) {
            CHECK(s2.vars[i] == world.id<Likes>());
        }
        s2.free();
        q2.free();
        b2.free();
    }

    query.free();
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/dynamic_query: a variable as source reads the bound entity and the field is shared") {
    World world;
    world.init();

    const EntityId apple = world.new_entity();
    world.set<Health>(apple, { 10 });
    const EntityId rock = world.new_entity();
    const EntityId alice = world.new_entity();
    world.set<Position>(alice, { 1, 0 });
    world.add<Likes>(alice, apple);
    const EntityId bob = world.new_entity();
    world.set<Position>(bob, { 2, 0 });
    world.add<Likes>(bob, apple);
    const EntityId carol = world.new_entity();
    world.set<Position>(carol, { 3, 0 });
    world.add<Likes>(carol, rock);

    QueryBuilder builder = world.query_build();
    const QueryVar food = builder.var("food");
    builder.term<Position>().term<Health>().src(food).with<Likes>(food);
    DynamicQuery query = builder.build();
    REQUIRE(query.is_ok());

    usz calls = 0;
    query.each<Position, Health>([&](const EntityId entity, Position& position, Health& health) {
        CHECK((entity == alice || entity == bob));
        CHECK(position.x < 3);
        CHECK(&health == world.get<Health>(apple));
        health.value++;
        calls++;
    });
    CHECK(calls == 2);
    // Both rows wrote the same shared element.
    CHECK(world.get<Health>(apple)->value == 12);

    query.iter<Position, Health>([&](QueryIter& it, Position*, Health* health) {
        CHECK_FALSE(it.shared[0]);
        CHECK(it.shared[1]);
        CHECK(it.sources[1] == apple);
        CHECK(it.vars[food.index] == apple);
        CHECK(health == world.get<Health>(apple));
        CHECK(QUERY::field_at<1, Health>(it, 1) == health);
    });

    CHECK_FALSE(query.matches(carol));

    SUBCASE("a without() on the bound variable's source") {
        world.add<TagA>(apple);
        QueryBuilder b2 = world.query_build();
        const QueryVar f = b2.var("f");
        b2.with<Likes>(f).without<TagA>().src(f);
        DynamicQuery q2 = b2.build();
        REQUIRE(q2.is_ok());
        Seen s2 = collect(q2, f);
        CHECK(s2.entities.count == 1);
        CHECK(s2.has(carol));
        s2.free();
        q2.free();
        b2.free();
    }

    SUBCASE("a without() with a bound variable side") {
        world.add<Eats>(alice, apple);
        QueryBuilder b2 = world.query_build();
        const QueryVar f = b2.var("f");
        b2.with<Likes>(f).without<Eats>(f);
        DynamicQuery q2 = b2.build();
        REQUIRE(q2.is_ok());
        Seen s2 = collect(q2, f);
        CHECK(s2.entities.count == 2);
        CHECK_FALSE(s2.has(alice));
        s2.free();
        q2.free();
        b2.free();
    }

    query.free();
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/dynamic_query: a fixed source gates the query and a query with no THIS yields one empty chunk") {
    World world;
    world.init();

    const EntityId flag = world.new_entity();
    const EntityId a = world.new_entity();
    world.set<Position>(a, { 1, 0 });

    QueryBuilder builder = world.query_build();
    builder.term<Position>().with<TagA>().src(flag);
    DynamicQuery query = builder.build();
    REQUIRE(query.is_ok());

    CHECK(query.count() == 0);
    CHECK_FALSE(query.matches(a));
    world.add<TagA>(flag);
    CHECK(query.count() == 1);
    CHECK(query.matches(a));
    query.iter<Position>([&](QueryIter& it, Position*) {
        CHECK(it.sources[1] == flag);
        CHECK(it.ids[1] == world.id<TagA>());
    });
    query.free();

    QueryBuilder b2 = world.query_build();
    b2.with<TagA>().src(flag);
    DynamicQuery q2 = b2.build();
    REQUIRE(q2.is_ok());
    usz chunks = 0;
    usz rows = 0;
    {
        TemporalAllocator temp = TemporalAllocator::create();
        QueryIter it = q2.begin(&temp);
        while (it.next(&it)) {
            chunks++;
            rows += it.count;
            CHECK(it.archetype == nullptr);
            CHECK(it.entities == nullptr);
        }
    }
    CHECK(chunks == 1);
    CHECK(rows == 0);
    CHECK(q2.count() == 0);
    CHECK(q2.empty());
    CHECK(q2.matches(a));
    world.remove<TagA>(flag);
    CHECK_FALSE(q2.matches(a));
    chunks = 0;
    {
        TemporalAllocator temp = TemporalAllocator::create();
        QueryIter it = q2.begin(&temp);
        while (it.next(&it)) {
            chunks++;
        }
    }
    CHECK(chunks == 0);

    SUBCASE("a dead fixed source holds nothing") {
        world.add<TagA>(flag);
        CHECK(q2.matches(a));
        world.delete_entity(flag);
        CHECK_FALSE(q2.matches(a));
    }

    q2.free();
    b2.free();
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/dynamic_query: optional terms deliver nullptr, and an optional variable yields unbound when nothing matches") {
    World world;
    world.init();

    const EntityId apple = world.new_entity();
    const EntityId pear = world.new_entity();
    const EntityId a = world.new_entity();
    world.set<Position>(a, { 1, 0 });
    world.set<Velocity>(a, { 5, 0 });
    const EntityId b = world.new_entity();
    world.set<Position>(b, { 2, 0 });
    world.add<Likes>(b, apple);
    world.add<Likes>(b, pear);

    SUBCASE("optional plain output") {
        QueryBuilder builder = world.query_build();
        builder.term<Position>().term<Velocity>().optional();
        DynamicQuery query = builder.build();
        REQUIRE(query.is_ok());
        CHECK(query.count() == 2);
        usz with_velocity = 0;
        usz without_velocity = 0;
        query.each<Position, Velocity*>([&](const EntityId entity, Position&, Velocity* velocity) {
            if (velocity != nullptr) {
                CHECK(entity == a);
                CHECK(velocity->dx == 5);
                with_velocity++;
            } else {
                CHECK(entity == b);
                without_velocity++;
            }
        });
        CHECK(with_velocity == 1);
        CHECK(without_velocity == 1);

        // Taking the optional field by reference skips the chunk that lacks it.
        usz by_reference = 0;
        query.each<Position, Velocity>([&](EntityId, Position&, Velocity&) { by_reference++; });
        CHECK(by_reference == 1);
        query.free();
        builder.free();
    }

    SUBCASE("optional variable term") {
        QueryBuilder builder = world.query_build();
        const QueryVar food = builder.var("food");
        builder.term<Position>().with<Likes>(food).optional();
        DynamicQuery query = builder.build();
        REQUIRE(query.is_ok());
        Seen seen = collect(query, food);
        // a once with food unbound, b once per pair.
        REQUIRE(seen.entities.count == 3);
        usz unbound = 0;
        usz bound = 0;
        for (usz i = 0; i < seen.entities.count; i++) {
            if (seen.entities[i] == a) {
                CHECK(seen.vars[i] == 0);
                unbound++;
            } else {
                CHECK(seen.entities[i] == b);
                CHECK((seen.vars[i] == apple || seen.vars[i] == pear));
                bound++;
            }
        }
        CHECK(unbound == 1);
        CHECK(bound == 2);
        seen.free();

        {
            TemporalAllocator temp = TemporalAllocator::create();
            QueryIter it = query.begin(&temp);
            while (it.next(&it)) {
                if (it.entities[0] == a) {
                    CHECK(it.ids[1] == 0);
                } else {
                    CHECK(it.ids[1] != 0);
                }
            }
        }
        query.free();
        builder.free();
    }

    SUBCASE("optional term on an unbound-then-missing source still binds later terms") {
        world.set<Health>(apple, { 3 });
        QueryBuilder builder = world.query_build();
        const QueryVar food = builder.var("food");
        builder.term<Position>().with<Likes>(food).optional().term<Health>().src(food).optional();
        DynamicQuery query = builder.build();
        REQUIRE(query.is_ok());
        usz with_health = 0;
        usz without_health = 0;
        query.each<Position, Health*>([&](EntityId, Position&, Health* health) {
            if (health != nullptr) {
                CHECK(health->value == 3);
                with_health++;
            } else {
                without_health++;
            }
        });
        CHECK(with_health == 1);  // b with apple
        CHECK(without_health == 2); // a (unbound food), b with pear
        query.free();
        builder.free();
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/dynamic_query: up() finds the id on the nearest ancestor and shares its data") {
    World world;
    world.init();

    const EntityId root = world.new_entity();
    world.set<Position>(root, { 100, 0 });
    const EntityId parent = world.new_entity();
    world.add(parent, ECS::PAIR(ECS::CHILD_OF, root));
    const EntityId child = world.new_entity();
    world.add(child, ECS::PAIR(ECS::CHILD_OF, parent));
    world.set<Velocity>(child, { 1, 0 });
    const EntityId orphan = world.new_entity();
    world.set<Velocity>(orphan, { 2, 0 });
    const EntityId near = world.new_entity();
    world.set<Position>(near, { 7, 0 });
    const EntityId near_child = world.new_entity();
    world.add(near_child, ECS::PAIR(ECS::CHILD_OF, near));
    world.set<Velocity>(near_child, { 3, 0 });

    QueryBuilder builder = world.query_build();
    builder.term<Velocity>().term<Position>().up();
    DynamicQuery query = builder.build();
    REQUIRE(query.is_ok());

    // child reaches root through parent; near_child reaches near; orphan has
    // no parent; parent has no Velocity; root is not its own ancestor.
    CHECK(query.count() == 2);
    CHECK(query.matches(child));
    CHECK(query.matches(near_child));
    CHECK_FALSE(query.matches(orphan));
    CHECK_FALSE(query.matches(parent));
    CHECK_FALSE(query.matches(root));

    query.iter<Velocity, Position>([&](QueryIter& it, Velocity*, Position* position) {
        CHECK(it.count == 1);
        CHECK_FALSE(it.shared[0]);
        CHECK(it.shared[1]);
        CHECK(it.ids[1] == world.id<Position>());
        if (it.entities[0] == child) {
            CHECK(it.sources[1] == root);
            CHECK(position->x == 100);
        } else {
            CHECK(it.entities[0] == near_child);
            CHECK(it.sources[1] == near);
            CHECK(position->x == 7);
        }
    });

    usz calls = 0;
    query.each<Velocity, Position>([&](EntityId, Velocity& velocity, Position& position) {
        velocity.dx += position.x;
        calls++;
    });
    CHECK(calls == 2);
    CHECK(world.get<Velocity>(child)->dx == 101);
    CHECK(world.get<Velocity>(near_child)->dx == 10);

    SUBCASE("the nearest ancestor with the id wins") {
        world.set<Position>(parent, { 50, 0 });
        query.iter<Velocity, Position>([&](QueryIter& it, Velocity*, Position* position) {
            if (it.entities[0] == child) {
                CHECK(it.sources[1] == parent);
                CHECK(position->x == 50);
            }
        });
    }

    SUBCASE("without().up() holds when no ancestor has the id") {
        QueryBuilder b2 = world.query_build();
        b2.term<Velocity>().without<Position>().up();
        DynamicQuery q2 = b2.build();
        REQUIRE(q2.is_ok());
        // orphan: no parent at all, so no ancestor holds Position.
        CHECK(q2.count() == 1);
        CHECK(q2.matches(orphan));
        CHECK_FALSE(q2.matches(child));
        q2.free();
        b2.free();
    }

    SUBCASE("up() with a variable binds from the ancestor's pair") {
        const EntityId apple = world.new_entity();
        world.add<Likes>(root, apple);
        QueryBuilder b2 = world.query_build();
        const QueryVar food = b2.var("food");
        b2.term<Velocity>().with<Likes>(food).up();
        DynamicQuery q2 = b2.build();
        REQUIRE(q2.is_ok());
        Seen s2 = collect(q2, food);
        REQUIRE(s2.entities.count == 1);
        CHECK(s2.entities[0] == child);
        CHECK(s2.vars[0] == apple);
        s2.free();
        q2.free();
        b2.free();
    }

    SUBCASE("up() from a variable source") {
        QueryBuilder b2 = world.query_build();
        const QueryVar kid = b2.var("kid");
        // Entities liked by someone: bind $kid from (Likes, $kid) on THIS,
        // then look for Position above $kid.
        world.add<Likes>(orphan, child);
        world.add<Likes>(orphan, near_child);
        world.add<Likes>(orphan, root);
        b2.with<Likes>(kid).with<Position>().src(kid).up();
        DynamicQuery q2 = b2.build();
        REQUIRE(q2.is_ok());
        Seen s2 = collect(q2, kid);
        CHECK(s2.entities.count == 2);
        for (usz i = 0; i < s2.entities.count; i++) {
            CHECK(s2.entities[i] == orphan);
            CHECK((s2.vars[i] == child || s2.vars[i] == near_child));
        }
        s2.free();
        q2.free();
        b2.free();
    }

    query.free();
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/dynamic_query: or-chains hold on the first alternative that matches") {
    World world;
    world.init();

    const EntityId only_a = world.new_entity();
    world.set<Position>(only_a, { 1, 0 });
    world.add<TagA>(only_a);
    const EntityId only_b = world.new_entity();
    world.set<Position>(only_b, { 2, 0 });
    world.add<TagB>(only_b);
    const EntityId both = world.new_entity();
    world.set<Position>(both, { 3, 0 });
    world.add<TagA>(both);
    world.add<TagB>(both);
    const EntityId neither = world.new_entity();
    world.set<Position>(neither, { 4, 0 });

    QueryBuilder builder = world.query_build();
    builder.term<Position>().with<TagA>().bor().with<TagB>();
    DynamicQuery query = builder.build();
    REQUIRE(query.is_ok());

    CHECK(query.count() == 3);
    CHECK_FALSE(query.matches(neither));
    query.iter<Position>([&](QueryIter& it, Position*) {
        if (it.entities[0] == only_b) {
            CHECK(it.ids[1] == 0);
            CHECK(it.ids[2] == world.id<TagB>());
        } else {
            CHECK(it.ids[1] == world.id<TagA>());
            CHECK(it.ids[2] == 0);
        }
    });

    SUBCASE("output alternatives deliver the one that matched") {
        world.set<Velocity>(only_b, { 9, 0 });
        world.set<Health>(neither, { 9 });
        QueryBuilder b2 = world.query_build();
        b2.term<Velocity>().bor().term<Health>();
        DynamicQuery q2 = b2.build();
        REQUIRE(q2.is_ok());
        usz velocities = 0;
        usz healths = 0;
        q2.each<Velocity*, Health*>([&](const EntityId entity, Velocity* velocity, Health* health) {
            if (entity == only_b) {
                CHECK(velocity != nullptr);
                CHECK(health == nullptr);
                velocities++;
            } else {
                CHECK(entity == neither);
                CHECK(velocity == nullptr);
                CHECK(health != nullptr);
                healths++;
            }
        });
        CHECK(velocities == 1);
        CHECK(healths == 1);
        q2.free();
        b2.free();
    }

    SUBCASE("an excluded alternative") {
        QueryBuilder b2 = world.query_build();
        b2.term<Position>().with<TagA>().bor().without<TagB>();
        DynamicQuery q2 = b2.build();
        REQUIRE(q2.is_ok());
        // only_a (TagA), both (TagA), neither (no TagB); only_b fails both.
        CHECK(q2.count() == 3);
        CHECK_FALSE(q2.matches(only_b));
        q2.free();
        b2.free();
    }

    query.free();
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/dynamic_query: the walk resumes across archetypes and the utilities consume it") {
    World world;
    world.init();

    const EntityId apple = world.new_entity();
    EntityId entities[12];
    for (usz i = 0; i < 12; i++) {
        entities[i] = world.new_entity();
        world.set<Position>(entities[i], { static_cast<f32>(i), 0 });
        if (i % 2 == 0) world.add<TagA>(entities[i]);
        if (i % 3 == 0) world.add<TagB>(entities[i]);
        world.add<Likes>(entities[i], apple);
    }

    QueryBuilder builder = world.query_build();
    const QueryVar food = builder.var("food");
    builder.term<Position>().with<Likes>(food).without<TagB>();
    DynamicQuery query = builder.build();
    REQUIRE(query.is_ok());

    // 12 minus the multiples of 3 (0, 3, 6, 9).
    CHECK(query.count() == 8);
    CHECK_FALSE(query.empty());
    CHECK(query.first() != 0);
    u64 rng = 0;
    const EntityId pick = query.random(rng);
    CHECK(pick != 0);
    CHECK(query.matches(pick));

    Seen seen = collect(query, food);
    CHECK(seen.entities.count == 8);
    for (usz i = 0; i < seen.entities.count; i++) {
        CHECK(seen.vars[i] == apple);
    }
    seen.free();

    SUBCASE("a moved query keeps working and the source is empty") {
        DynamicQuery moved = std::move(query);
        CHECK_FALSE(query.is_ok());
        CHECK(query.count() == 0);
        CHECK(moved.is_ok());
        CHECK(moved.count() == 8);
        CHECK(moved.var("food") == food);
        moved.free();
    }

    query.free();
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/dynamic_query: a query that failed to build matches nothing") {
    World world;
    world.init();

    const EntityId a = world.new_entity();
    world.set<Position>(a, { 1, 0 });

    QueryBuilder builder = world.query_build();
    const QueryVar ghost = builder.var("ghost");
    builder.term<Position>().with<TagA>().src(ghost);
    DynamicQuery query = builder.build();
    CHECK_FALSE(query.is_ok());
    CHECK(query.count() == 0);
    CHECK(query.empty());
    CHECK_FALSE(query.matches(a));
    usz calls = 0;
    query.each<Position>([&](EntityId, Position&) { calls++; });
    CHECK(calls == 0);
    {
        TemporalAllocator temp = TemporalAllocator::create();
        QueryIter it = query.begin(&temp);
        CHECK_FALSE(it.next(&it));
        CHECK_FALSE(it.next(&it));
    }

    query.free();
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/dynamic_query: the trivial mode walks every archetype when no term narrows the candidates") {
    World world;
    world.init();

    const EntityId apple = world.new_entity();
    const EntityId a = world.new_entity();
    world.set<Position>(a, { 1, 0 });
    world.add<Likes>(a, apple);
    const EntityId b = world.new_entity();
    world.set<Position>(b, { 2, 0 });
    const EntityId c = world.new_entity();
    world.add<Eats>(c, apple);

    // (*, *) has no record of its own, so SELECT walks the world's archetype
    // list and the matcher picks the ones holding any pair. The built-in
    // ids hold (ON_DELETE, PANIC) and would match too; a without() term
    // keeps them out without narrowing the walk.
    QueryBuilder builder = world.query_build();
    builder.with(ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD))
        .without(ECS::PAIR(ECS::ON_DELETE, ECS::WILDCARD))
        .term<Position>()
        .optional();
    DynamicQuery query = builder.build();
    REQUIRE(query.is_ok());

    CHECK(query.count() == 2);
    CHECK(query.matches(a));
    CHECK(query.matches(c));
    CHECK_FALSE(query.matches(b));

    usz with_position = 0;
    usz without_position = 0;
    query.iter<Position*>([&](QueryIter& it, Position* positions) {
        CHECK(it.count == 1);
        CHECK(ECS::IS_PAIR(it.ids[0]));
        CHECK(it.ids[1] == 0);
        CHECK(it.sources[0] == 0);
        CHECK_FALSE(it.shared[0]);
        if (it.entities[0] == a) {
            CHECK(it.ids[0] == ECS::PAIR(world.id<Likes>(), apple));
            CHECK(it.ids[2] == world.id<Position>());
            CHECK(positions[0].x == 1);
            with_position++;
        } else {
            CHECK(it.entities[0] == c);
            CHECK(it.ids[0] == ECS::PAIR(world.id<Eats>(), apple));
            CHECK(it.ids[2] == 0);
            CHECK(positions == nullptr);
            without_position++;
        }
    });
    CHECK(with_position == 1);
    CHECK(without_position == 1);

    query.free();
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}
