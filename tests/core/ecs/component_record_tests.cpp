#include "support/test_support.hpp"

#include "engine/ecs/archetype/archetype.hpp"
#include "engine/ecs/component_record.hpp"

// The three structures a ComponentRecord keeps over the archetypes holding
// its id: columns_index (archetype id -> column), archetype_list (dense
// archetype + column) and archetype_index (archetype id -> position in the
// list). link_archetype and unlink_archetype keep them in agreement.

namespace {

// Every entry of the list is found by both maps at the right place, and the
// maps hold nothing else.
void check_consistent(const ComponentRecord* record) {
    CHECK(record->columns_index.count == record->archetype_list.count);
    CHECK(record->archetype_index.count == record->archetype_list.count);
    for (usz i = 0; i < record->archetype_list.count; i++) {
        const RecordColumn& entry = record->archetype_list[i];
        REQUIRE(entry.archetype != nullptr);
        const ArchetypeId id = entry.archetype->archetype_id;
        const usz* position = record->archetype_index.find(id);
        REQUIRE(position != nullptr);
        CHECK(*position == i);
        const usz* column = record->columns_index.find(id);
        REQUIRE(column != nullptr);
        CHECK(*column == entry.column);
        CHECK(entry.archetype->type.ids[entry.column] == record->id);
    }
}

} // namespace

TEST_CASE("ecs/component_record: archetypes are linked in creation order with their column") {
    World world;
    world.init();

    // Health first, so its component id sorts before Position's in a type.
    const Id health = world.id<Health>();
    const Id position = world.id<Position>();
    REQUIRE(health < position);
    const EntityId a = world.new_entity();
    world.set<Position>(a, { 1, 1 });
    const EntityId b = world.new_entity();
    world.set<Position>(b, { 2, 2 });
    world.add<TagA>(b);
    const EntityId c = world.new_entity();
    world.set<Health>(c, { 3 });
    world.set<Position>(c, { 3, 3 });

    const ComponentRecord* record = ComponentRecord::component_record_find(&world, position);
    REQUIRE(record != nullptr);
    // [Position], [Position, TagA], [Health, Position]: three archetypes,
    // in the order they were created, and the column differs in the last.
    REQUIRE(record->archetype_count() == 3);
    check_consistent(record);
    CHECK(record->archetype_list[0].archetype == world.entity_index.get_record_alive(a)->archetype);
    CHECK(record->archetype_list[1].archetype == world.entity_index.get_record_alive(b)->archetype);
    CHECK(record->archetype_list[2].archetype == world.entity_index.get_record_alive(c)->archetype);
    CHECK(record->archetype_list[0].column == 0);
    CHECK(record->archetype_list[2].column == 1);

    SUBCASE("linking an archetype twice changes nothing") {
        Archetype* first = record->archetype_list[0].archetype;
        ComponentRecord* mutable_record = ComponentRecord::component_record_find(&world, position);
        CHECK_FALSE(mutable_record->link_archetype(first, 0));
        CHECK(record->archetype_count() == 3);
        check_consistent(record);
    }

    SUBCASE("a wildcard record points at the first matching pair column of each archetype") {
        const EntityId apple = world.new_entity();
        const EntityId pear = world.new_entity();
        world.add<Likes>(a, pear);
        world.add<Likes>(a, apple);
        const ComponentRecord* likes_any = ComponentRecord::component_record_find(&world, ECS::PAIR(world.id<Likes>(), ECS::WILDCARD));
        REQUIRE(likes_any != nullptr);
        // a passed through [Position, (Likes, pear)] into
        // [Position, (Likes, apple), (Likes, pear)]; both are linked once.
        CHECK(likes_any->archetype_count() == 2);
        CHECK(likes_any->columns_index.count == 2);
        CHECK(likes_any->archetype_index.count == 2);
        const Archetype* current = world.entity_index.get_record_alive(a)->archetype;
        const usz* column = likes_any->columns_index.find(current->archetype_id);
        REQUIRE(column != nullptr);
        CHECK(ECS::PAIR_SECOND(current->type.ids[*column]) == ECS::ENTITY_LOW(apple));
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/component_record: unlinking an archetype in the middle swaps the last one into its place") {
    World world;
    world.init();

    const Id position = world.id<Position>();
    // Three archetypes holding Position; the middle one is also the only
    // holder of the tag entity `mark`, so deleting `mark` destroys it.
    const EntityId mark = world.new_entity();
    const EntityId a = world.new_entity();
    world.set<Position>(a, { 1, 1 });
    const EntityId b = world.new_entity();
    world.set<Position>(b, { 2, 2 });
    world.add(b, mark);
    const EntityId c = world.new_entity();
    world.set<Position>(c, { 3, 3 });
    world.add<TagA>(c);

    const ComponentRecord* record = ComponentRecord::component_record_find(&world, position);
    REQUIRE(record != nullptr);
    REQUIRE(record->archetype_count() == 3);
    Archetype* first = record->archetype_list[0].archetype;
    Archetype* last = record->archetype_list[2].archetype;
    const ArchetypeId middle_id = record->archetype_list[1].archetype->archetype_id;

    REQUIRE(world.delete_entity(mark));
    // b fell back to [Position]; [Position, mark] is gone.
    CHECK(world.entity_index.get_record_alive(b)->archetype == first);
    CHECK_FALSE(world.archetypes.is_alive(middle_id));

    REQUIRE(record->archetype_count() == 2);
    check_consistent(record);
    CHECK(record->archetype_list[0].archetype == first);
    CHECK(record->archetype_list[1].archetype == last);
    CHECK(*record->archetype_index.find(last->archetype_id) == 1);
    CHECK(record->columns_index.find(middle_id) == nullptr);
    CHECK(record->archetype_index.find(middle_id) == nullptr);

    SUBCASE("unlinking the last entry needs no swap, and an unknown id is refused") {
        ComponentRecord* mutable_record = ComponentRecord::component_record_find(&world, position);
        CHECK_FALSE(mutable_record->unlink_archetype(middle_id));
        CHECK(mutable_record->unlink_archetype(last->archetype_id));
        CHECK(record->archetype_count() == 1);
        check_consistent(record);
        CHECK(record->archetype_list[0].archetype == first);
        // Put it back so the world tears down with the record complete.
        CHECK(mutable_record->link_archetype(last, *last->columns_index.find(position)));
        check_consistent(record);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}
