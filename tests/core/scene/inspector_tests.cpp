#include "support/test_support.hpp"

#include "engine/scene/inspector.hpp"

#include <cstring>

// The Inspector component on component entities: expose<T>() and of().

namespace {

bool draw_a(InspectorContext&, void*) { return true; }
bool draw_b(InspectorContext&, void*) { return false; }

} // namespace

TEST_CASE("scene/inspector: expose stores an Inspector on the component entity") {
    World world;
    world.init();

    INSPECTOR::expose<Position>(world, "Position", &draw_a, 5);
    const Id position = world.id<Position>();
    const Inspector* inspector = INSPECTOR::of(world, position);
    REQUIRE(inspector != nullptr);
    CHECK(strcmp(inspector->name, "Position") == 0);
    CHECK(inspector->draw == &draw_a);
    CHECK(inspector->order == 5);
    CHECK(world.has<Inspector>(position));

    SUBCASE("exposing again replaces it") {
        INSPECTOR::expose<Position>(world, "Pos", &draw_b);
        const Inspector* again = INSPECTOR::of(world, position);
        REQUIRE(again != nullptr);
        CHECK(strcmp(again->name, "Pos") == 0);
        CHECK(again->draw == &draw_b);
        CHECK(again->order == 100);
        CHECK(world.query<Inspector>().count() == 1);
    }

    SUBCASE("entities holding the component are unaffected") {
        const EntityId entity = world.new_entity();
        world.set(entity, Position { 1, 2 });
        CHECK_FALSE(world.has<Inspector>(entity));
        CHECK(world.get<Position>(entity)->x == 1);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("scene/inspector: of is nullptr for unexposed ids, pairs, tags and dead ids") {
    World world;
    world.init();

    INSPECTOR::expose<Position>(world, "Position", &draw_a);
    CHECK(INSPECTOR::of(world, world.id<Velocity>()) == nullptr);
    CHECK(INSPECTOR::of(world, world.id<TagA>()) == nullptr);
    CHECK(INSPECTOR::of(world, 0) == nullptr);

    const EntityId parent = world.new_entity();
    CHECK(INSPECTOR::of(world, ECS::PAIR(ECS::CHILD_OF, parent)) == nullptr);

    const EntityId tag = world.new_entity();
    INSPECTOR::expose(world, tag, "tag", &draw_a);
    CHECK(INSPECTOR::of(world, tag) != nullptr);
    world.delete_entity(tag);
    CHECK(INSPECTOR::of(world, tag) == nullptr);

    SUBCASE("expose ignores pairs, 0, dead ids and null draw functions") {
        INSPECTOR::expose(world, ECS::PAIR(ECS::CHILD_OF, parent), "pair", &draw_a);
        INSPECTOR::expose(world, 0, "zero", &draw_a);
        INSPECTOR::expose(world, tag, "dead", &draw_a);
        INSPECTOR::expose<Velocity>(world, "Velocity", nullptr);
        CHECK(INSPECTOR::of(world, world.id<Velocity>()) == nullptr);
        CHECK(world.query<Inspector>().count() == 1);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}
