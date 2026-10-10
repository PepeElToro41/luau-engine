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

// Addable: addable<T>() / addable_of() / can_add() / add().

namespace {

void init_position(World&, EntityId, void* data) {
    *static_cast<Position*>(data) = Position { 7, 9 };
}

// A default that looks something up on the world: the entity's own Health.
void init_velocity_from_health(World& world, const EntityId entity, void* data) {
    const Health* health = world.get<Health>(entity);
    *static_cast<Velocity*>(data) = Velocity { health != nullptr ? static_cast<f32>(health->value) : -1.0f, 0 };
}

} // namespace

TEST_CASE("scene/inspector: addable stores an Addable on the component entity") {
    World world;
    world.init();

    INSPECTOR::addable<Position>(world, &init_position);
    const Id position = world.id<Position>();
    const Addable* addable = INSPECTOR::addable_of(world, position);
    REQUIRE(addable != nullptr);
    CHECK(addable->init == &init_position);
    CHECK(world.has<Addable>(position));
    CHECK(INSPECTOR::addable_of(world, world.id<Velocity>()) == nullptr);

    SUBCASE("the typed form defaults to default_init for data and none for tags") {
        INSPECTOR::addable<Velocity>(world);
        INSPECTOR::addable<TagA>(world);
        REQUIRE(INSPECTOR::addable_of(world, world.id<Velocity>()) != nullptr);
        CHECK(INSPECTOR::addable_of(world, world.id<Velocity>())->init == &INSPECTOR::default_init<Velocity>);
        REQUIRE(INSPECTOR::addable_of(world, world.id<TagA>()) != nullptr);
        CHECK(INSPECTOR::addable_of(world, world.id<TagA>())->init == nullptr);
    }

    SUBCASE("calling it again replaces the init") {
        INSPECTOR::addable<Position>(world, nullptr);
        REQUIRE(INSPECTOR::addable_of(world, position) != nullptr);
        CHECK(INSPECTOR::addable_of(world, position)->init == nullptr);
        CHECK(world.query<Addable>().count() == 1);
    }

    SUBCASE("ignores pairs, 0 and dead ids") {
        const EntityId parent = world.new_entity();
        INSPECTOR::addable(world, ECS::PAIR(ECS::CHILD_OF, parent), nullptr);
        INSPECTOR::addable(world, 0, nullptr);
        const EntityId dead = world.new_entity();
        world.delete_entity(dead);
        INSPECTOR::addable(world, dead, nullptr);
        CHECK(INSPECTOR::addable_of(world, dead) == nullptr);
        CHECK(world.query<Addable>().count() == 1);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("scene/inspector: add sets the default value from init and fires the added hook with it") {
    World world;
    world.init();
    INSPECTOR::addable<Position>(world, &init_position);
    HookLog log;
    world.hook_added<Position>(&HookLog::record_added, &log);

    const EntityId entity = world.new_entity();
    CHECK(INSPECTOR::can_add(world, entity, world.id<Position>()));
    CHECK(INSPECTOR::add(world, entity, world.id<Position>()));
    REQUIRE(world.has<Position>(entity));
    CHECK(world.get<Position>(entity)->x == 7);
    CHECK(world.get<Position>(entity)->y == 9);
    REQUIRE(log.events.count == 1);
    CHECK(log.events[0].entity == entity);
    CHECK(log.events[0].id == world.id<Position>());

    SUBCASE("default_init copies a value-initialized T") {
        INSPECTOR::addable<Health>(world);
        CHECK(INSPECTOR::add(world, entity, world.id<Health>()));
        REQUIRE(world.has<Health>(entity));
        CHECK(world.get<Health>(entity)->value == Health {}.value);
    }

    SUBCASE("init may read the world and the entity") {
        world.set(entity, Health { 42 });
        INSPECTOR::addable<Velocity>(world, &init_velocity_from_health);
        CHECK(INSPECTOR::add(world, entity, world.id<Velocity>()));
        REQUIRE(world.has<Velocity>(entity));
        CHECK(world.get<Velocity>(entity)->dx == 42.0f);
    }

    SUBCASE("a nullptr init leaves zeroed bytes") {
        INSPECTOR::addable<Velocity>(world, nullptr);
        CHECK(INSPECTOR::add(world, entity, world.id<Velocity>()));
        REQUIRE(world.has<Velocity>(entity));
        CHECK(world.get<Velocity>(entity)->dx == 0.0f);
        CHECK(world.get<Velocity>(entity)->dy == 0.0f);
    }

    SUBCASE("tags are added without data") {
        INSPECTOR::addable<TagA>(world);
        CHECK(INSPECTOR::add(world, entity, world.id<TagA>()));
        CHECK(world.has<TagA>(entity));
    }

    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("scene/inspector: add refuses what can_add refuses") {
    World world;
    world.init();
    INSPECTOR::addable<Position>(world);
    const EntityId entity = world.new_entity();

    SUBCASE("a component that is not addable") {
        CHECK_FALSE(INSPECTOR::can_add(world, entity, world.id<Velocity>()));
        CHECK_FALSE(INSPECTOR::add(world, entity, world.id<Velocity>()));
        CHECK_FALSE(world.has<Velocity>(entity));
    }

    SUBCASE("a component the entity already has keeps its value") {
        world.set(entity, Position { 1, 2 });
        CHECK_FALSE(INSPECTOR::can_add(world, entity, world.id<Position>()));
        CHECK_FALSE(INSPECTOR::add(world, entity, world.id<Position>()));
        CHECK(world.get<Position>(entity)->x == 1);
    }

    SUBCASE("a dead entity") {
        world.delete_entity(entity);
        CHECK_FALSE(INSPECTOR::can_add(world, entity, world.id<Position>()));
        CHECK_FALSE(INSPECTOR::add(world, entity, world.id<Position>()));
    }

    SUBCASE("pairs and 0") {
        CHECK_FALSE(INSPECTOR::add(world, entity, ECS::PAIR(ECS::CHILD_OF, world.new_entity())));
        CHECK_FALSE(INSPECTOR::add(world, entity, 0));
    }

    world.free();
    CHECK_ARENA_CLEAN();
}
