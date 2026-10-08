#include "support/test_support.hpp"

#include "engine/ecs/query/query_builder.hpp"
#include "engine/ecs/query/query_term.hpp"

#include <cstring>

// The builder behind world.query_build(): term shapes, modifiers on the last
// term, variables and ownership. build() does not exist yet, so the tests
// inspect the recorded terms directly.

TEST_CASE("ecs/query_builder: starts empty and stores its flags") {
    World world;
    world.init();

    QueryBuilder builder = world.query_build(QUERY_CACHED);
    CHECK(builder.world == &world);
    CHECK(builder.flags == QUERY_CACHED);
    CHECK(builder.terms.count == 0);
    CHECK(builder.var_count() == 1);
    CHECK(builder.field_count() == 0);
    CHECK(std::strcmp(builder.var_name(QUERY_THIS), "this") == 0);

    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query_builder: term() adds outputs, with() and without() add constraints") {
    World world;
    world.init();

    const EntityId bob = world.new_entity();
    const Id likes = world.id<Likes>();
    const Id eats = world.id<Eats>();

    QueryBuilder builder = world.query_build();
    builder.term<Position>()
        .term<ECS::Pair<Likes, Health>>()
        .with<TagA, TagB>()
        .with(likes, eats)
        .with<Likes>(bob)
        .without<Eats>(bob)
        .without<Health>();

    REQUIRE(builder.terms.count == 9);
    CHECK(builder.field_count() == 2);

    CHECK(builder.terms.data[0].id == world.id<Position>());
    CHECK(builder.terms.data[0].flags == TERM_OUTPUT);
    CHECK(builder.terms.data[1].id == ECS::PAIR(likes, world.id<Health>()));
    CHECK(builder.terms.data[1].flags == TERM_OUTPUT);

    CHECK(builder.terms.data[2].id == world.id<TagA>());
    CHECK(builder.terms.data[2].flags == 0);
    CHECK(builder.terms.data[3].id == world.id<TagB>());
    CHECK(builder.terms.data[4].id == likes);
    CHECK(builder.terms.data[5].id == eats);
    CHECK(builder.terms.data[6].id == ECS::PAIR(likes, bob));
    CHECK(builder.terms.data[6].flags == 0);

    CHECK(builder.terms.data[7].id == ECS::PAIR(eats, bob));
    CHECK(builder.terms.data[7].flags == TERM_EXCLUDE);
    CHECK(builder.terms.data[8].id == world.id<Health>());
    CHECK(builder.terms.data[8].flags == TERM_EXCLUDE);

    for (const QueryTerm& term : builder.terms) {
        CHECK(term.is_on_this());
        CHECK_FALSE(term.binds());
    }

    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query_builder: raw id terms") {
    World world;
    world.init();

    const Id position = world.id<Position>();
    const Id likes = world.id<Likes>();

    QueryBuilder builder = world.query_build();
    builder.term(position).with(likes).without(Id { 0 });
    REQUIRE(builder.terms.count == 3);
    CHECK(builder.terms.data[0].id == position);
    CHECK(builder.terms.data[0].is_output());
    CHECK(builder.terms.data[1].id == likes);
    CHECK_FALSE(builder.terms.data[1].is_output());
    // A 0 id is recorded for build() to complain about, not dropped silently.
    CHECK(builder.terms.data[2].id == 0);
    CHECK(builder.terms.data[2].is_excluded());

    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query_builder: modifiers apply to the last term only") {
    World world;
    world.init();

    const EntityId parent = world.new_entity();
    const Id likes = world.id<Likes>();

    QueryBuilder builder = world.query_build();

    SUBCASE("optional") {
        builder.term<Position>().term<Velocity>().optional();
        CHECK_FALSE(builder.terms.data[0].is_optional());
        CHECK(builder.terms.data[1].is_optional());
        CHECK(builder.terms.data[1].is_output());
    }

    SUBCASE("or") {
        builder.with<TagA>().bor().with<TagB>();
        CHECK(builder.terms.data[0].is_or());
        CHECK_FALSE(builder.terms.data[1].is_or());
    }

    SUBCASE("src with an entity") {
        builder.with<TagA>().src(parent);
        CHECK(builder.terms.data[0].src == parent);
        CHECK_FALSE(builder.terms.data[0].src_var.is_set());
        CHECK_FALSE(builder.terms.data[0].is_on_this());
    }

    SUBCASE("src with a variable, and back to this") {
        const QueryVar other = builder.var("other");
        builder.with<TagA>().src(other);
        CHECK(builder.terms.data[0].src_var == other);
        CHECK(builder.terms.data[0].src == 0);
        builder.src(QUERY_THIS);
        CHECK(builder.terms.data[0].is_on_this());
    }

    SUBCASE("up defaults to CHILD_OF") {
        builder.term<Position>().up();
        CHECK(builder.terms.data[0].traverses());
        CHECK(builder.terms.data[0].traverse == ECS::CHILD_OF);
        CHECK_FALSE(builder.terms.data[0].is_on_this());
    }

    SUBCASE("up with an explicit relation") {
        builder.term<Position>().up(likes);
        CHECK(builder.terms.data[0].traverse == likes);
    }

    SUBCASE("a variadic with() modifies its last id") {
        builder.with<TagA, TagB>().optional();
        CHECK_FALSE(builder.terms.data[0].is_optional());
        CHECK(builder.terms.data[1].is_optional());
    }

    SUBCASE("modifiers without a term are ignored") {
        builder.optional().bor().src(parent).up();
        CHECK(builder.terms.count == 0);
    }

    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query_builder: variables are named, deduplicated and bound to pair sides") {
    World world;
    world.init();

    const EntityId apple = world.new_entity();
    const Id likes = world.id<Likes>();
    const Id eats = world.id<Eats>();

    QueryBuilder builder = world.query_build();

    const QueryVar food = builder.var("food");
    const QueryVar who = builder.var("who");
    CHECK(food.is_set());
    CHECK(food != QUERY_THIS);
    CHECK(food != who);
    CHECK(builder.var("food") == food);
    CHECK(builder.var("this") == QUERY_THIS);
    CHECK_FALSE(builder.var(nullptr).is_set());
    CHECK_FALSE(builder.var("").is_set());
    CHECK(builder.var_count() == 3);
    CHECK(std::strcmp(builder.var_name(food), "food") == 0);
    CHECK(std::strcmp(builder.var_name(who), "who") == 0);
    CHECK(builder.var_name(QueryVar {}) == nullptr);
    CHECK(builder.var_name(QueryVar { 42 }) == nullptr);

    SUBCASE("typed relation, variable target") {
        builder.term<Likes>(food).with<Eats>(food).optional();
        REQUIRE(builder.terms.count == 2);
        CHECK(builder.terms.data[0].id == ECS::PAIR(likes, ECS::WILDCARD));
        CHECK(builder.terms.data[0].second_var == food);
        CHECK_FALSE(builder.terms.data[0].first_var.is_set());
        CHECK(builder.terms.data[0].binds());
        CHECK(builder.terms.data[0].is_output());
        CHECK(builder.terms.data[1].id == ECS::PAIR(eats, ECS::WILDCARD));
        CHECK(builder.terms.data[1].second_var == food);
        CHECK(builder.terms.data[1].is_optional());
    }

    SUBCASE("raw relation, variable target") {
        builder.term(likes, food).with(eats, food).without(likes, who);
        REQUIRE(builder.terms.count == 3);
        CHECK(builder.terms.data[0].id == ECS::PAIR(likes, ECS::WILDCARD));
        CHECK(builder.terms.data[0].second_var == food);
        CHECK(builder.terms.data[2].id == ECS::PAIR(likes, ECS::WILDCARD));
        CHECK(builder.terms.data[2].second_var == who);
        CHECK(builder.terms.data[2].is_excluded());
    }

    SUBCASE("variable relation, concrete target") {
        builder.term(who, apple).with(who, apple).without(who, apple);
        REQUIRE(builder.terms.count == 3);
        for (const QueryTerm& term : builder.terms) {
            CHECK(term.id == ECS::PAIR(ECS::WILDCARD, apple));
            CHECK(term.first_var == who);
            CHECK_FALSE(term.second_var.is_set());
        }
        CHECK(builder.terms.data[0].is_output());
        CHECK(builder.terms.data[1].flags == 0);
        CHECK(builder.terms.data[2].is_excluded());
    }

    SUBCASE("a 0 side keeps the variable but records a 0 id") {
        builder.with(Id { 0 }, food).with(who, Id { 0 });
        CHECK(builder.terms.data[0].id == 0);
        CHECK(builder.terms.data[0].second_var == food);
        CHECK(builder.terms.data[1].id == 0);
        CHECK(builder.terms.data[1].first_var == who);
    }

    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query_builder: free empties the builder and it can be reused") {
    World world;
    world.init();

    QueryBuilder builder = world.query_build();
    builder.term<Position>().var("x");
    REQUIRE(builder.terms.count == 1);
    REQUIRE(builder.var_count() == 2);

    builder.free();
    CHECK(builder.terms.count == 0);
    CHECK(builder.var_count() == 1);

    builder.with<TagA>();
    CHECK(builder.terms.count == 1);
    CHECK(builder.var("y").index == 1);

    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}
