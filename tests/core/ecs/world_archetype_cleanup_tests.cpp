#include "support/test_support.hpp"

#include "engine/ecs/archetype/archetype.hpp"
#include "engine/ecs/component_record.hpp"
#include "engine/ecs/entity_index.hpp"
#include "engine/ecs/query/query.hpp"

// World::cleanup: reclaiming the archetypes that hold no entity, by age
// against World::clock, and the world staying consistent afterwards.

namespace {

Archetype* archetype_of(World& world, const EntityId entity) {
    EntityRecord* record = world.entity_index.get_record_alive(entity);
    REQUIRE(record != nullptr);
    REQUIRE(record->archetype != nullptr);
    return record->archetype;
}

// Every archetype slot in use holds rows (or is the root) and the dead count
// agrees.
bool no_dead_archetypes(const World& world) {
    for (usz i = 0; i < world.archetypes.alive_count; i++) {
        const Archetype* archetype = world.archetypes.get_element_any(world.archetypes.get_alive_id(i));
        if (!archetype->alive) {
            return false;
        }
    }
    return world.dead_archetype_count == 0;
}

} // namespace

TEST_CASE("ecs/world: cleanup destroys every empty archetype and keeps the rest") {
    World world;
    world.init();
    // init() leaves some empty intermediate tables behind; start from none.
    world.cleanup();
    REQUIRE(no_dead_archetypes(world));
    const usz baseline = world.archetypes.alive_count;

    const EntityId a = world.new_entity();
    const EntityId b = world.new_entity();
    world.set(a, Position { 1, 1 });      // {Position}
    world.set(a, Velocity { 2, 2 });      // {Position, Velocity}; {Position} now empty
    world.set(b, Health { 3 });           // {Health}
    world.add<TagA>(b);                   // {Health, TagA}; {Health} now empty
    Archetype* with_both = archetype_of(world, a);
    Archetype* with_health_tag = archetype_of(world, b);
    CHECK(world.archetypes.alive_count == baseline + 4);
    CHECK(world.dead_archetype_count == 2);
    CHECK(world.alive_archetype_count == baseline + 2);

    const usz destroyed = world.cleanup();
    CHECK(destroyed == 2);
    CHECK(world.archetypes.alive_count == baseline + 2);
    CHECK(no_dead_archetypes(world));
    CHECK(world.alive_archetype_count == baseline + 2);
    CHECK_ARENA_CLEAN();

    SUBCASE("the populated archetypes and their data are untouched") {
        CHECK(archetype_of(world, a) == with_both);
        CHECK(archetype_of(world, b) == with_health_tag);
        CHECK(world.get<Position>(a)->x == 1);
        CHECK(world.get<Velocity>(a)->dx == 2);
        CHECK(world.get<Health>(b)->value == 3);
        CHECK(world.has<TagA>(b));
    }
    SUBCASE("the root is never destroyed") {
        CHECK(world.root_archetype != nullptr);
        CHECK(world.root_archetype->alive);
        CHECK(world.cleanup() == 0);
        CHECK(world.root_archetype != nullptr);
    }
    SUBCASE("a second cleanup finds nothing") {
        CHECK(world.cleanup() == 0);
        CHECK(world.archetypes.alive_count == baseline + 2);
    }
    SUBCASE("the destroyed tables are rebuilt on demand and the edges still work") {
        // {Position} was destroyed: removing Velocity has to recreate it.
        world.remove<Velocity>(a);
        Archetype* with_position = archetype_of(world, a);
        CHECK(with_position->type.id_count == 1);
        CHECK(with_position->alive);
        CHECK(world.get<Position>(a)->x == 1);
        CHECK(world.archetypes.alive_count == baseline + 3);
        // The old edge from {Position, Velocity} went with the table; the new
        // one leads to the recreated archetype.
        Archetype** backwards = with_both->backwards_edges.find(world.id<Velocity>());
        REQUIRE(backwards != nullptr);
        CHECK(*backwards == with_position);

        world.set(a, Velocity { 5, 5 });
        CHECK(archetype_of(world, a) == with_both);
        CHECK(world.get<Velocity>(a)->dx == 5);
    }
    SUBCASE("component records forget the destroyed tables") {
        const ComponentRecord* position = ComponentRecord::component_record_find(&world, world.id<Position>());
        REQUIRE(position != nullptr);
        // Only {Position, Velocity} mentions Position now.
        CHECK(position->archetype_count() == 1);
        CHECK(position->columns_index.contains(with_both->archetype_id));
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/world: cleanup(min_age) only reclaims archetypes dead for long enough") {
    World world;
    world.init();
    world.cleanup();
    const usz baseline = world.archetypes.alive_count;

    const EntityId a = world.new_entity();
    const EntityId b = world.new_entity();
    world.set(a, Position { });
    world.set(b, Health { });
    Archetype* with_position = archetype_of(world, a);
    Archetype* with_health = archetype_of(world, b);

    // {Position} dies at 0, {Health} dies at 10.
    world.remove<Position>(a);
    world.tick(10);
    world.remove<Health>(b);
    CHECK(with_position->died_at == 0);
    CHECK(with_health->died_at == 10);
    CHECK(world.dead_archetype_count == 2);

    SUBCASE("nothing is old enough yet") {
        CHECK(world.cleanup(11) == 0);
        CHECK(world.dead_archetype_count == 2);
        CHECK(world.archetypes.alive_count == baseline + 2);
    }
    SUBCASE("only the older table goes") {
        CHECK(world.cleanup(5) == 1);
        CHECK(world.dead_archetype_count == 1);
        CHECK(world.archetypes.alive_count == baseline + 1);
        // {Health} survived; putting Health back lands in the same table.
        world.set(b, Health { 7 });
        CHECK(archetype_of(world, b) == with_health);
        CHECK(with_health->alive);
    }
    SUBCASE("time passing makes both eligible") {
        world.tick(5);
        CHECK(world.cleanup(5) == 2);
        CHECK(world.dead_archetype_count == 0);
        CHECK(world.archetypes.alive_count == baseline);
    }
    SUBCASE("an archetype that was refilled is not reclaimed") {
        world.set(a, Position { });
        REQUIRE(with_position->alive);
        CHECK(world.cleanup(0) == 1);
        CHECK(archetype_of(world, a) == with_position);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/world: cleanup keeps cached queries consistent") {
    World world;
    world.init();

    const EntityId a = world.new_entity();
    const EntityId b = world.new_entity();
    world.set(a, Position { 1, 0 });
    world.set(b, Position { 2, 0 });
    world.add<TagA>(b);

    Query<Position> positions = world.query<Position>(QUERY_CACHED);
    CHECK(positions.count() == 2);

    // Empty {Position, TagA} by moving b back to {Position}, then reclaim it.
    world.remove<TagA>(b);
    CHECK(world.cleanup() >= 1);

    SUBCASE("the cache no longer visits the destroyed table") {
        usz visited = 0;
        f32 sum = 0;
        positions.each([&](const EntityId, Position& position) {
            visited++;
            sum += position.x;
        });
        CHECK(visited == 2);
        CHECK(sum == 3);
        CHECK(positions.count() == 2);
    }
    SUBCASE("a rebuilt table is picked up again") {
        world.add<TagA>(b);
        world.add<TagB>(a);
        CHECK(positions.count() == 2);
    }

    positions.cleanup();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/world: deletion cascades leave the archetype counts consistent") {
    World world;
    world.init();

    const EntityId parent = world.new_entity();
    for (usz i = 0; i < 4; i++) {
        const EntityId child = world.new_entity();
        world.add(child, ECS::PAIR(ECS::CHILD_OF, parent));
        if (i % 2 == 0) {
            world.set(child, Position { });
        }
    }
    CHECK(world.alive_archetype_count + world.dead_archetype_count == world.archetypes.alive_count);

    // Deleting the parent deletes the children and destroys the tables that
    // held (CHILD_OF, parent); the counts have to follow.
    CHECK(world.delete_entity(parent));
    CHECK(world.alive_archetype_count + world.dead_archetype_count == world.archetypes.alive_count);

    const usz dead = world.dead_archetype_count;
    CHECK(world.cleanup() == dead);
    CHECK(world.dead_archetype_count == 0);
    CHECK(world.alive_archetype_count == world.archetypes.alive_count);

    world.free();
    CHECK_ARENA_CLEAN();
}
