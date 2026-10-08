#include "support/test_support.hpp"

#include "engine/ecs/query_builder.hpp"
#include "engine/ecs/query_program.hpp"
#include "engine/ecs/query_term.hpp"

#include <cstring>

// The compiler behind QueryBuilder::build(): which terms end up in the
// ArchetypeMatcher prefilter, which become ops, in what order, and what is
// rejected. Running the ops is dynamic_query_tests.cpp's business.

namespace {

QueryProgram compile(QueryBuilder& builder) {
    return QUERY_PROGRAM::compile(builder.world, builder.terms.data, builder.terms.count, builder.var_count(), builder.world->allocator);
}

bool has_with(const QueryProgram& program, const Id id) {
    for (usz i = 0; i < program.with_count; i++) {
        if (program.with_ids[i] == id) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("ecs/query_program: plain terms on THIS are the matcher, the program is select + yield") {
    World world;
    world.init();

    QueryBuilder builder = world.query_build();
    builder.term<Position>().with<TagA>().without<TagB>();
    QueryProgram program = compile(builder);

    REQUIRE(program.ok);
    CHECK(program.term_count == 3);
    CHECK(program.field_count == 1);
    CHECK(program.term_fields[0] == 0);
    CHECK(program.term_fields[1] == QUERY_OP_NONE);
    CHECK(program.term_fields[2] == QUERY_OP_NONE);
    CHECK(program.var_count == 1);
    CHECK(program.binds_this);

    CHECK(program.with_count == 2);
    CHECK(has_with(program, world.id<Position>()));
    CHECK(has_with(program, world.id<TagA>()));
    CHECK(program.this_term_count == 3);

    REQUIRE(program.op_count == 2);
    CHECK(program.ops[0].kind == QUERY_OP_SELECT);
    CHECK(program.ops[1].kind == QUERY_OP_YIELD);
    CHECK(std::strcmp(QUERY_PROGRAM::op_name(program.ops[0].kind), "select") == 0);

    // The matcher is the real thing: it accepts and rejects archetypes.
    const EntityId yes = world.new_entity();
    world.set<Position>(yes, { 1, 2 });
    world.add<TagA>(yes);
    const EntityId no = world.new_entity();
    world.set<Position>(no, { 1, 2 });
    world.add<TagA>(no);
    world.add<TagB>(no);
    CHECK(program.matcher.matches(world.entity_index.get_record_alive(yes)->archetype));
    CHECK_FALSE(program.matcher.matches(world.entity_index.get_record_alive(no)->archetype));

    program.free();
    CHECK_FALSE(program.ok);
    CHECK(program.op_count == 0);
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query_program: a variable term on THIS adds its wildcard pattern to the matcher and an AND op that binds") {
    World world;
    world.init();

    QueryBuilder builder = world.query_build();
    const QueryVar food = builder.var("food");
    builder.term<Position>().with<Likes>(food);
    QueryProgram program = compile(builder);

    REQUIRE(program.ok);
    CHECK(program.var_count == 2);
    CHECK(has_with(program, world.id<Position>()));
    CHECK(has_with(program, ECS::PAIR(world.id<Likes>(), ECS::WILDCARD)));
    CHECK(program.this_term_count == 1);
    CHECK(program.this_terms[0] == 0);

    REQUIRE(program.op_count == 3);
    CHECK(program.ops[0].kind == QUERY_OP_SELECT);
    CHECK(program.ops[1].kind == QUERY_OP_AND);
    CHECK(program.ops[1].term == 1);
    CHECK(program.ops[1].bind_first == QUERY_OP_NONE);
    CHECK(program.ops[1].bind_second == food.index);
    CHECK_FALSE(program.ops[1].optional);
    CHECK(program.ops[2].kind == QUERY_OP_YIELD);

    SUBCASE("a second term on the same variable does not bind it again") {
        builder.free();
        builder = world.query_build();
        const QueryVar f = builder.var("food");
        builder.with<Likes>(f).with<Eats>(f);
        program.free();
        program = compile(builder);
        REQUIRE(program.ok);
        REQUIRE(program.op_count == 4);
        CHECK(program.ops[1].bind_second == f.index);
        CHECK(program.ops[2].kind == QUERY_OP_AND);
        CHECK(program.ops[2].bind_second == QUERY_OP_NONE);
    }

    program.free();
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query_program: up() on THIS requires the (relation, *) pair and becomes an UP op") {
    World world;
    world.init();

    QueryBuilder builder = world.query_build();
    builder.term<Position>().up();
    QueryProgram program = compile(builder);

    REQUIRE(program.ok);
    CHECK(program.with_count == 1);
    CHECK(has_with(program, ECS::PAIR(ECS::CHILD_OF, ECS::WILDCARD)));
    CHECK_FALSE(has_with(program, world.id<Position>()));
    CHECK(program.this_term_count == 0);
    REQUIRE(program.op_count == 3);
    CHECK(program.ops[0].kind == QUERY_OP_SELECT);
    CHECK(program.ops[1].kind == QUERY_OP_UP);
    CHECK(program.ops[1].term == 0);
    CHECK(program.ops[2].kind == QUERY_OP_YIELD);

    SUBCASE("an excluded up() term adds nothing to the matcher") {
        builder.free();
        builder = world.query_build();
        builder.with<TagA>().without<Position>().up();
        program.free();
        program = compile(builder);
        REQUIRE(program.ok);
        CHECK(program.with_count == 1);
        CHECK(has_with(program, world.id<TagA>()));
        CHECK(program.ops[1].kind == QUERY_OP_UP);
    }

    program.free();
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query_program: a query with no term on THIS has no SELECT") {
    World world;
    world.init();

    const EntityId bob = world.new_entity();
    QueryBuilder builder = world.query_build();
    builder.with<TagA>().src(bob);
    QueryProgram program = compile(builder);

    REQUIRE(program.ok);
    CHECK_FALSE(program.binds_this);
    CHECK(program.with_count == 0);
    REQUIRE(program.op_count == 2);
    CHECK(program.ops[0].kind == QUERY_OP_AND);
    CHECK(program.ops[0].term == 0);
    CHECK(program.ops[1].kind == QUERY_OP_YIELD);

    program.free();
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query_program: a term on an unbound variable waits for the term that binds it") {
    World world;
    world.init();

    QueryBuilder builder = world.query_build();
    const QueryVar food = builder.var("food");
    // Given out of order: the source term first, then the binding one.
    builder.term<Health>().src(food).with<Likes>(food).without<Eats>(food);
    QueryProgram program = compile(builder);

    REQUIRE(program.ok);
    REQUIRE(program.op_count == 5);
    CHECK(program.ops[0].kind == QUERY_OP_SELECT);
    CHECK(program.ops[1].kind == QUERY_OP_AND);
    CHECK(program.ops[1].term == 1);
    CHECK(program.ops[1].bind_second == food.index);
    CHECK(program.ops[2].kind == QUERY_OP_AND);
    CHECK(program.ops[2].term == 0);
    CHECK(program.ops[2].bind_second == QUERY_OP_NONE);
    CHECK(program.ops[3].kind == QUERY_OP_NOT);
    CHECK(program.ops[3].term == 2);
    CHECK(program.ops[4].kind == QUERY_OP_YIELD);

    program.free();
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query_program: optional and or-chain terms stay out of the matcher") {
    World world;
    world.init();

    QueryBuilder builder = world.query_build();

    SUBCASE("optional plain terms are resolved at yield without an op") {
        builder.term<Position>().term<Velocity>().optional();
        QueryProgram program = compile(builder);
        REQUIRE(program.ok);
        CHECK(program.with_count == 1);
        CHECK(has_with(program, world.id<Position>()));
        CHECK(program.this_term_count == 2);
        CHECK(program.op_count == 2);
        program.free();
    }

    SUBCASE("optional variable terms are optional ops") {
        const QueryVar food = builder.var("food");
        builder.term<Position>().with<Likes>(food).optional();
        QueryProgram program = compile(builder);
        REQUIRE(program.ok);
        CHECK(program.with_count == 1);
        REQUIRE(program.op_count == 3);
        CHECK(program.ops[1].kind == QUERY_OP_AND);
        CHECK(program.ops[1].optional);
        CHECK(program.ops[1].bind_second == food.index);
        program.free();
    }

    SUBCASE("an or-chain is one OR op over its terms") {
        builder.term<Position>().with<TagA>().bor().with<TagB>().bor().with<Likes>().with<Health>();
        QueryProgram program = compile(builder);
        REQUIRE(program.ok);
        CHECK(program.with_count == 2);
        CHECK(has_with(program, world.id<Position>()));
        CHECK(has_with(program, world.id<Health>()));
        CHECK(program.this_term_count == 2);
        REQUIRE(program.op_count == 3);
        CHECK(program.ops[1].kind == QUERY_OP_OR);
        CHECK(program.ops[1].term == 1);
        CHECK(program.ops[1].term_count == 3);
        program.free();
    }

    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/query_program: rejected term lists leave the program not ok") {
    World world;
    world.init();

    const EntityId bob = world.new_entity();
    QueryBuilder builder = world.query_build();
    const QueryVar food = builder.var("food");

    SUBCASE("an id of 0") {
        builder.with(Id { 0 });
    }
    SUBCASE("THIS as a pair side") {
        builder.with(world.id<Likes>(), QUERY_THIS);
    }
    SUBCASE("a variable out of range") {
        builder.with(world.id<Likes>(), QueryVar { 7 });
    }
    SUBCASE("excluded and optional") {
        builder.without<TagA>().optional();
    }
    SUBCASE("traversal through a relation that is not traversable") {
        builder.term<Position>().up(world.id<Likes>());
    }
    SUBCASE("a source variable nothing binds") {
        builder.term<Position>().with<TagA>().src(food);
    }
    SUBCASE("a without() term that would bind a variable") {
        builder.term<Position>().without<Likes>(food);
    }
    SUBCASE("an or-chain alternative that would bind a variable") {
        builder.term<Position>().with<TagA>().bor().with<Likes>(food);
    }
    SUBCASE("an optional or-chain alternative") {
        builder.with<TagA>().bor().with<TagB>().optional();
    }
    SUBCASE("bor() on the last term") {
        builder.with<TagA>().bor();
    }
    SUBCASE("the same variable on both pair sides") {
        QueryTerm term = QueryTerm::make(ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD), 0);
        term.first_var = food;
        term.second_var = food;
        builder.terms.push(term);
    }
    SUBCASE("a fixed source of 0") {
        builder.with<TagA>().src(EntityId { 0 });
    }
    (void)bob;

    QueryProgram program = compile(builder);
    CHECK_FALSE(program.ok);
    program.free();

    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}
