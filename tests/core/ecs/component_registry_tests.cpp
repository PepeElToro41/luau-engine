#include "support/test_support.hpp"

#include "engine/ecs/component_registry.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/utils/log.hpp"

#include <cstring>
#include <utility>

// The two component id tiers: preregistered ids the process agrees on and
// runtime ids each world hands out on its own (component_registry.hpp,
// World::component / tag / register_component).
//
// Every test here touches the process-wide registry, so each resets it on
// the way in and out and uses types of its own: the sample types of
// test_support.hpp stay runtime components for every other test.

namespace {

struct RegPosition {
    f32 x = 0;
    f32 y = 0;
};

struct RegVelocity {
    f32 dx = 0;
    f32 dy = 0;
};

struct RegCounter {
    i32 hits = 0;
};

struct RegMarker {};
struct RegLater {
    u64 value = 0;
};

// Distinct types by the dozen, to fill a tier.
template <usz N>
struct Filler {
    u8 bytes[8] = {};
};

template <usz N>
struct PreFiller {
    u8 bytes[4] = {};
};

template <std::size_t... I>
usz claim_fillers(World& world, std::index_sequence<I...>) {
    usz claimed = 0;
    ((world.component<Filler<I>>() != 0 ? claimed++ : claimed), ...);
    return claimed;
}

template <std::size_t... I>
usz preregister_fillers(std::index_sequence<I...>) {
    usz claimed = 0;
    ((COMPONENT_REGISTRY::preregister<PreFiller<I>>("PreFiller") != 0 ? claimed++ : claimed), ...);
    return claimed;
}

// Silences the errors the exhaustion and collision cases provoke on purpose.
struct Quiet {
    LogLevel level;
    Quiet() : level(LOG::stderr_level()) { LOG::set_stderr_level(LOG_LEVEL_NONE); }
    void restore() const { LOG::set_stderr_level(this->level); }
};

} // namespace

TEST_CASE("ecs/component_registry: two worlds agree on a preregistered id whatever order they touch it in") {
    COMPONENT_REGISTRY::reset();
    const Id position = COMPONENT_REGISTRY::preregister<RegPosition>("RegPosition");
    const Id velocity = COMPONENT_REGISTRY::preregister<RegVelocity>("RegVelocity");
    const Id marker = COMPONENT_REGISTRY::preregister<RegMarker>("RegMarker");
    REQUIRE(position != 0);
    REQUIRE(velocity != 0);
    REQUIRE(marker != 0);
    CHECK(position != velocity);
    CHECK(velocity != marker);
    CHECK(COMPONENT_REGISTRY::count() == 3);
    // Claiming again is the same id, and the entries read back.
    CHECK(COMPONENT_REGISTRY::preregister<RegPosition>("again") == position);
    CHECK(COMPONENT_REGISTRY::count() == 3);
    const ComponentRegistryEntry* entry = COMPONENT_REGISTRY::find<RegPosition>();
    REQUIRE(entry != nullptr);
    CHECK(entry->id == position);
    CHECK(entry->info.length == sizeof(RegPosition));
    CHECK(entry->info.alignment == alignof(RegPosition));
    CHECK_FALSE(entry->is_tag);
    CHECK(strcmp(entry->name, "RegPosition") == 0);
    CHECK(COMPONENT_REGISTRY::find_id(position) == entry);
    CHECK(COMPONENT_REGISTRY::find_name("RegPosition") == entry);
    CHECK(COMPONENT_REGISTRY::find_name("Nothing") == nullptr);
    CHECK(COMPONENT_REGISTRY::find<RegMarker>()->is_tag);
    CHECK(COMPONENT_REGISTRY::find<RegMarker>()->info.length == 0);

    World a;
    a.init();
    World b;
    b.init();
    // Opposite orders, with a runtime type thrown in between.
    CHECK(a.component<RegPosition>() == position);
    CHECK(a.component<Position>() != 0);
    CHECK(a.component<RegVelocity>() == velocity);
    CHECK(a.tag<RegMarker>() == marker);
    CHECK(b.tag<RegMarker>() == marker);
    CHECK(b.component<RegVelocity>() == velocity);
    CHECK(b.component<RegPosition>() == position);

    // The data is stored the usual way.
    const TypeInfo* info = static_cast<const TypeInfo*>(b.get(position, ECS::COMPONENT));
    REQUIRE(info != nullptr);
    CHECK(info->length == sizeof(RegPosition));
    CHECK(b.get(marker, ECS::COMPONENT) == nullptr);
    CHECK(a.component_type_of(position) == TYPE_ID::get<RegPosition>());
    CHECK(b.component_type_of(position) == TYPE_ID::get<RegPosition>());

    // Entities use them like any other component.
    const EntityId e = b.new_entity();
    b.set(e, RegPosition { 1, 2 });
    b.add<RegMarker>(e);
    REQUIRE(b.get<RegPosition>(e) != nullptr);
    CHECK(b.get<RegPosition>(e)->y == 2);
    CHECK(b.has<RegMarker>(e));

    a.free();
    b.free();
    COMPONENT_REGISTRY::reset();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/component_registry: two worlds may disagree on a runtime id and each stays consistent") {
    COMPONENT_REGISTRY::reset();
    World a;
    a.init();
    World b;
    b.init();

    const EntityId a_position = a.component<RegPosition>();
    const EntityId a_velocity = a.component<RegVelocity>();
    const EntityId b_velocity = b.component<RegVelocity>();
    const EntityId b_position = b.component<RegPosition>();
    CHECK(a_position == ECS::MAX_COMPONENT_ID);
    CHECK(a_velocity == ECS::MAX_COMPONENT_ID - 1);
    CHECK(b_velocity == ECS::MAX_COMPONENT_ID);
    CHECK(b_position == ECS::MAX_COMPONENT_ID - 1);
    CHECK(a_position != b_position);

    // Self-consistent: every later lookup in a world returns its own id.
    CHECK(a.component<RegPosition>() == a_position);
    CHECK(a.id<RegPosition>() == a_position);
    CHECK(b.component<RegPosition>() == b_position);
    CHECK(a.component_type_of(a_position) == TYPE_ID::get<RegPosition>());
    CHECK(b.component_type_of(a_position) == TYPE_ID::get<RegVelocity>());
    CHECK(a.component_type_of(ECS::MAX_COMPONENT_ID - 2) == 0);
    CHECK(a.component_type_of(ECS::COMPONENT) == 0);
    CHECK(a.component_type_of(0) == 0);
    CHECK(COMPONENT_REGISTRY::count() == 0);

    a.free();
    b.free();
    COMPONENT_REGISTRY::reset();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/component_registry: preregistering after a world claimed runtime ids collides with nothing") {
    COMPONENT_REGISTRY::reset();
    World world;
    world.init();
    const EntityId runtime = world.component<RegPosition>();
    REQUIRE(runtime == ECS::MAX_COMPONENT_ID);

    // Late, but from the other end of the band.
    const Id later = COMPONENT_REGISTRY::preregister<RegLater>("RegLater");
    REQUIRE(later == ECS::FIRST_COMPONENT_ID);
    CHECK(world.component<RegLater>() == later);
    CHECK(world.component<RegPosition>() == runtime);
    // The runtime tier keeps counting down from where it was.
    CHECK(world.component<RegVelocity>() == ECS::MAX_COMPONENT_ID - 1);

    SUBCASE("a type claimed as runtime here keeps its id when it is preregistered afterwards") {
        // The world bound RegPosition before the registry knew it; the
        // registry's id is for worlds that come later.
        const Id registered = COMPONENT_REGISTRY::preregister<RegPosition>("RegPosition");
        CHECK(registered == ECS::FIRST_COMPONENT_ID + 1);
        CHECK(world.component<RegPosition>() == runtime);
        World fresh;
        fresh.init();
        CHECK(fresh.component<RegPosition>() == registered);
        fresh.free();
    }

    world.free();
    COMPONENT_REGISTRY::reset();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/component_registry: a runtime id and a preregistered id never alias") {
    COMPONENT_REGISTRY::reset();
    World world;
    world.init();
    // Runtime ids 256 down to 7.
    REQUIRE(claim_fillers(world, std::make_index_sequence<250>()) == 250);
    CHECK(world.runtime_next_id == 6);
    // Preregistered ids 1 to 7: the last one is the id Filler<249> holds.
    REQUIRE(preregister_fillers(std::make_index_sequence<7>()) == 7);
    CHECK(COMPONENT_REGISTRY::find<PreFiller<6>>()->id == 7);
    CHECK(world.component<Filler<249>>() == 7);

    CHECK(world.component<PreFiller<5>>() == 6);
    const Quiet quiet;
    // Refused rather than silently rebinding the slot.
    CHECK(world.component<PreFiller<6>>() == 0);
    quiet.restore();
    CHECK(world.component<Filler<249>>() == 7);
    CHECK(world.component_type_of(7) == TYPE_ID::get<Filler<249>>());
    // A fresh world has no such conflict.
    World fresh;
    fresh.init();
    CHECK(fresh.component<PreFiller<6>>() == 7);
    fresh.free();

    world.free();
    COMPONENT_REGISTRY::reset();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/component_registry: register_component by name reuses the id of a name seen before") {
    COMPONENT_REGISTRY::reset();
    World world;
    world.init();

    const TypeInfo info { 12, 4 };
    const EntityId health = world.register_component("Health", info);
    const EntityId mana = world.register_component("Mana", info);
    REQUIRE(health != 0);
    REQUIRE(mana != 0);
    CHECK(health != mana);
    CHECK(health == ECS::MAX_COMPONENT_ID);
    CHECK(mana == ECS::MAX_COMPONENT_ID - 1);
    CHECK(world.find_component("Health") == health);
    CHECK(world.find_component("Mana") == mana);
    CHECK(world.find_component("Stamina") == 0);
    CHECK(world.find_component(nullptr) == 0);
    CHECK(world.component_type_of(health) == 0);

    const TypeInfo* stored = static_cast<const TypeInfo*>(world.get(health, ECS::COMPONENT));
    REQUIRE(stored != nullptr);
    CHECK(stored->length == 12);
    CHECK(stored->alignment == 4);

    SUBCASE("the same name is the same id, even with another size") {
        CHECK(world.register_component("Health", info) == health);
        const Quiet quiet;
        CHECK(world.register_component("Health", TypeInfo { 16, 8 }) == health);
        quiet.restore();
        CHECK(static_cast<const TypeInfo*>(world.get(health, ECS::COMPONENT))->length == 12);
        CHECK(world.runtime_next_id == ECS::MAX_COMPONENT_ID - 2);
    }
    SUBCASE("a zero-length info makes a tag") {
        const EntityId tag = world.register_component("Dead", TypeInfo {});
        REQUIRE(tag != 0);
        CHECK(world.get(tag, ECS::COMPONENT) == nullptr);
        const EntityId e = world.new_entity();
        world.add(e, tag);
        CHECK(world.has(e, tag));
        CHECK(world.get(e, tag) == nullptr);
    }
    SUBCASE("the id is a component like any other") {
        const EntityId e = world.new_entity();
        const u8 bytes[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
        world.set(e, health, bytes);
        const u8* read = static_cast<const u8*>(world.get(e, health));
        REQUIRE(read != nullptr);
        CHECK(memcmp(read, bytes, sizeof(bytes)) == 0);
    }
    SUBCASE("an empty name is refused") {
        CHECK(world.register_component("", info) == 0);
        CHECK(world.register_component(nullptr, info) == 0);
    }
    SUBCASE("C++ types and names share the runtime tier") {
        CHECK(world.component<RegPosition>() == ECS::MAX_COMPONENT_ID - 2);
        CHECK(world.register_component("Stamina", info) == ECS::MAX_COMPONENT_ID - 3);
    }

    world.free();
    COMPONENT_REGISTRY::reset();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/component_registry: a full band reports an error and returns 0") {
    COMPONENT_REGISTRY::reset();

    SUBCASE("the runtime tier of one world") {
        World world;
        world.init();
        CHECK(claim_fillers(world, std::make_index_sequence<256>()) == 256);
        CHECK(world.runtime_next_id == 0);
        const Quiet quiet;
        CHECK(world.component<RegPosition>() == 0);
        CHECK(world.register_component("Health", TypeInfo { 4, 4 }) == 0);
        quiet.restore();
        // Nothing was bound for the refused type.
        CHECK(world.type_index.find(TYPE_ID::get<RegPosition>()) == nullptr);
        world.free();
    }
    SUBCASE("the registry") {
        CHECK(preregister_fillers(std::make_index_sequence<256>()) == 256);
        CHECK(COMPONENT_REGISTRY::count() == 256);
        const Quiet quiet;
        CHECK(COMPONENT_REGISTRY::preregister<RegPosition>("RegPosition") == 0);
        quiet.restore();
        CHECK(COMPONENT_REGISTRY::count() == 256);
        // A world whose whole band is preregistered has no runtime tier left.
        World world;
        world.init();
        CHECK(world.component<PreFiller<0>>() == 1);
        CHECK(world.component<PreFiller<255>>() == 256);
        const Quiet quiet_again;
        CHECK(world.component<RegPosition>() == 0);
        quiet_again.restore();
        world.free();
    }

    COMPONENT_REGISTRY::reset();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/component_registry: a strict world refuses a type nobody preregistered") {
    COMPONENT_REGISTRY::reset();
    const Id position = COMPONENT_REGISTRY::preregister<RegPosition>("RegPosition");
    REQUIRE(position != 0);

    World world;
    world.init();
    // A runtime type bound before the world turned strict keeps working.
    const EntityId early = world.component<RegVelocity>();
    REQUIRE(early != 0);
    world.strict_components = true;

    CHECK(world.component<RegPosition>() == position);
    CHECK(world.component<RegVelocity>() == early);
    const Quiet quiet;
    CHECK(world.component<RegCounter>() == 0);
    CHECK(world.tag<RegMarker>() == 0);
    CHECK(world.id<RegMarker>() == 0);
    quiet.restore();
    CHECK(world.type_index.find(TYPE_ID::get<RegCounter>()) == nullptr);
    CHECK(world.runtime_next_id == ECS::MAX_COMPONENT_ID - 1);

    // The typed operations on a refused type are no-ops.
    const EntityId e = world.new_entity();
    const Quiet quiet_again;
    world.set(e, RegCounter { 3 });
    world.add<RegMarker>(e);
    quiet_again.restore();
    CHECK(world.get<RegCounter>(e) == nullptr);
    CHECK_FALSE(world.has<RegMarker>(e));

    // Names are a world-local tier too, so they are refused as well? No:
    // register_component is the runtime tier by design, and a strict world
    // only refuses C++ types it cannot have been told about in advance.
    CHECK(world.register_component("Scripted", TypeInfo { 8, 8 }) != 0);

    world.free();
    COMPONENT_REGISTRY::reset();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/component_registry: the registry decides whether a type carries data, process-wide") {
    COMPONENT_REGISTRY::reset();
    const Id counter = COMPONENT_REGISTRY::preregister_tag<RegCounter>("RegCounter");
    REQUIRE(counter != 0);
    CHECK(COMPONENT_REGISTRY::find<RegCounter>()->is_tag);

    World world;
    world.init();
    // component<T> on a type the registry made a tag stores nothing.
    CHECK(world.component<RegCounter>() == counter);
    CHECK(world.get(counter, ECS::COMPONENT) == nullptr);
    CHECK(world.tag<RegCounter>() == counter);
    const EntityId e = world.new_entity();
    world.add(e, counter);
    CHECK(world.has(e, counter));
    CHECK(world.get(e, counter) == nullptr);

    // And the other way round: a tag<T> call on a preregistered component
    // still gets the component, data included.
    const Id position = COMPONENT_REGISTRY::preregister<RegPosition>("RegPosition");
    World other;
    other.init();
    CHECK(other.tag<RegPosition>() == position);
    const TypeInfo* info = static_cast<const TypeInfo*>(other.get(position, ECS::COMPONENT));
    REQUIRE(info != nullptr);
    CHECK(info->length == sizeof(RegPosition));

    world.free();
    other.free();
    COMPONENT_REGISTRY::reset();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/component_registry: reset forgets every registration") {
    COMPONENT_REGISTRY::reset();
    COMPONENT_REGISTRY::preregister<RegPosition>("RegPosition");
    COMPONENT_REGISTRY::preregister<RegVelocity>("RegVelocity");
    REQUIRE(COMPONENT_REGISTRY::count() == 2);
    COMPONENT_REGISTRY::reset();
    CHECK(COMPONENT_REGISTRY::count() == 0);
    CHECK(COMPONENT_REGISTRY::find<RegPosition>() == nullptr);
    CHECK(COMPONENT_REGISTRY::find_id(1) == nullptr);
    // Ids are handed out from the bottom again.
    CHECK(COMPONENT_REGISTRY::preregister<RegVelocity>("RegVelocity") == ECS::FIRST_COMPONENT_ID);
    COMPONENT_REGISTRY::reset();
}
