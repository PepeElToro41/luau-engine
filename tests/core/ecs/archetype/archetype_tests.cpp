#include "support/test_support.hpp"

#include "engine/ecs/archetype/archetype.hpp"
#include "engine/ecs/archetype/archetype_signature.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/entity_index.hpp"
#include "engine/memory/heap_allocator.hpp"

namespace {

Archetype* archetype_of(World& world, const EntityId entity) {
    EntityRecord* record = world.entity_index.get_record_alive(entity);
    REQUIRE(record != nullptr);
    REQUIRE(record->archetype != nullptr);
    return record->archetype;
}

bool is_sorted(const ArchetypeType& type) {
    for (usz i = 1; i < type.id_count; i++) {
        if (type.ids[i - 1] >= type.ids[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

// --- ArchetypeType -----------------------------------------------------------

TEST_CASE("ecs/archetype_type: clone copies the ids into independent storage") {
    BaseAllocator* allocator = MEMORY::heap_allocator();
    u64 ids[] = { 1, 5, 9 };
    const ArchetypeType type(ids, 3);

    ArchetypeType copy = type.clone(allocator);
    REQUIRE(copy.id_count == 3);
    CHECK(copy.ids != type.ids);
    CHECK(copy == type);

    ids[1] = 6;
    CHECK(copy.ids[1] == 5);
    CHECK(copy != type);

    SUBCASE("reserve leaves room without changing the count") {
        ArchetypeType reserved = type.clone(allocator, 4);
        CHECK(reserved.id_count == 3);
        CHECK(reserved.equals(type));
        CHECK(reserved.ids[1] == 6);
        reserved.free(allocator);
    }
    SUBCASE("an empty type clones to an empty type") {
        const ArchetypeType empty;
        ArchetypeType empty_copy = empty.clone(allocator);
        CHECK(empty_copy.id_count == 0);
        CHECK(empty_copy == empty);
        empty_copy.free(allocator);
    }

    copy.free(allocator);
    CHECK(copy.ids == nullptr);
    CHECK(copy.id_count == 0);
}

TEST_CASE("ecs/archetype_type: insert keeps the ids sorted") {
    BaseAllocator* allocator = MEMORY::heap_allocator();
    u64 ids[] = { 10, 20, 30 };
    const ArchetypeType type(ids, 3);

    SUBCASE("in front") {
        ArchetypeType inserted = type.insert(allocator, 5);
        REQUIRE(inserted.id_count == 4);
        CHECK(inserted.ids[0] == 5);
        CHECK(inserted.ids[3] == 30);
        CHECK(is_sorted(inserted));
        inserted.free(allocator);
    }
    SUBCASE("in the middle") {
        ArchetypeType inserted = type.insert(allocator, 25);
        REQUIRE(inserted.id_count == 4);
        CHECK(inserted.ids[2] == 25);
        CHECK(inserted.ids[3] == 30);
        CHECK(is_sorted(inserted));
        inserted.free(allocator);
    }
    SUBCASE("at the back, pairs sort after plain ids") {
        const Id pair = ECS::PAIR(1, 1);
        ArchetypeType inserted = type.insert(allocator, pair);
        REQUIRE(inserted.id_count == 4);
        CHECK(inserted.ids[3] == pair);
        CHECK(is_sorted(inserted));
        inserted.free(allocator);
    }
    SUBCASE("an id already present leaves the set unchanged") {
        ArchetypeType inserted = type.insert(allocator, 20);
        CHECK(inserted.id_count == 3);
        CHECK(inserted == type);
        inserted.free(allocator);
    }
    SUBCASE("into an empty type") {
        const ArchetypeType empty;
        ArchetypeType inserted = empty.insert(allocator, 7);
        REQUIRE(inserted.id_count == 1);
        CHECK(inserted.ids[0] == 7);
        inserted.free(allocator);
    }
    SUBCASE("the source is untouched") {
        ArchetypeType inserted = type.insert(allocator, 25);
        CHECK(type.id_count == 3);
        CHECK(ids[2] == 30);
        inserted.free(allocator);
    }
}

TEST_CASE("ecs/archetype_type: remove drops exactly one id") {
    BaseAllocator* allocator = MEMORY::heap_allocator();
    u64 ids[] = { 10, 20, 30 };
    const ArchetypeType type(ids, 3);

    SUBCASE("a present id") {
        ArchetypeType removed = type.remove(allocator, 20);
        REQUIRE(removed.id_count == 2);
        CHECK(removed.ids[0] == 10);
        CHECK(removed.ids[1] == 30);
        CHECK(is_sorted(removed));
        removed.free(allocator);
    }
    SUBCASE("the first and last id") {
        ArchetypeType without_first = type.remove(allocator, 10);
        ArchetypeType without_last = type.remove(allocator, 30);
        REQUIRE(without_first.id_count == 2);
        REQUIRE(without_last.id_count == 2);
        CHECK(without_first.ids[0] == 20);
        CHECK(without_last.ids[1] == 20);
        without_first.free(allocator);
        without_last.free(allocator);
    }
    SUBCASE("a missing id leaves the set unchanged") {
        ArchetypeType removed = type.remove(allocator, 99);
        CHECK(removed.id_count == 3);
        CHECK(removed == type);
        removed.free(allocator);
    }
    SUBCASE("removing the only id gives an empty type") {
        u64 single[] = { 4 };
        const ArchetypeType one(single, 1);
        ArchetypeType removed = one.remove(allocator, 4);
        CHECK(removed.id_count == 0);
        CHECK(removed == ArchetypeType());
        removed.free(allocator);
    }
    SUBCASE("insert then remove round-trips") {
        ArchetypeType inserted = type.insert(allocator, 25);
        ArchetypeType restored = inserted.remove(allocator, 25);
        CHECK(restored == type);
        inserted.free(allocator);
        restored.free(allocator);
    }
}

TEST_CASE("ecs/archetype_type: equality and hashing follow the id set") {
    u64 a_ids[] = { 1, 2, 3 };
    u64 b_ids[] = { 1, 2, 3 };
    u64 c_ids[] = { 1, 2, 4 };
    u64 d_ids[] = { 1, 2 };
    const ArchetypeType a(a_ids, 3);
    const ArchetypeType b(b_ids, 3);
    const ArchetypeType c(c_ids, 3);
    const ArchetypeType d(d_ids, 2);
    const ArchetypeType empty;

    CHECK(a == b);
    CHECK(a.equals(b));
    CHECK_FALSE(a != b);
    CHECK(a != c);
    CHECK(a != d);
    CHECK_FALSE(d.equals(a));
    CHECK(empty == ArchetypeType());
    CHECK(empty != d);

    CHECK(a.hash() == b.hash());
    CHECK(a.hash() != c.hash());
    CHECK(a.hash() != d.hash());
    CHECK(std::hash<ArchetypeType>{}(a) == a.hash());

    SUBCASE("a prefix-equal shorter type is not equal") {
        CHECK_FALSE(a.equals(d));
        CHECK_FALSE(d.equals(a));
    }
}

// --- ArchetypeSignature ------------------------------------------------------

TEST_CASE("ecs/archetype_signature: component ids go in the mask, everything else in the bloom") {
    const Id tag_entity = ECS::REST + 1;
    const Id pair = ECS::PAIR(ECS::REST + 2, ECS::REST + 3);
    Id ids[] = { 3, 70, tag_entity, pair };
    const ArchetypeSignature signature = ArchetypeSignature::build(ids, 4);

    CHECK(signature.mask.has(3));
    CHECK(signature.mask.has(70));
    CHECK_FALSE(signature.mask.has(4));
    CHECK_FALSE(signature.bloom.is_empty());

    SUBCASE("every non-component id passes the bloom") {
        BloomFilter query;
        query.add(tag_entity);
        CHECK(signature.bloom.test(query));
        BloomFilter pair_query;
        pair_query.add(pair);
        CHECK(signature.bloom.test(pair_query));
    }
    SUBCASE("a pair also registers its two wildcard spellings") {
        BloomFilter relation_wildcard;
        relation_wildcard.add(ECS::PAIR(ECS::REST + 2, ECS::WILDCARD));
        CHECK(signature.bloom.test(relation_wildcard));
        BloomFilter target_wildcard;
        target_wildcard.add(ECS::PAIR(ECS::WILDCARD, ECS::REST + 3));
        CHECK(signature.bloom.test(target_wildcard));
    }
    SUBCASE("build equals adding one by one") {
        ArchetypeSignature manual;
        for (usz i = 0; i < 4; i++) {
            manual.add(ids[i]);
        }
        CHECK(manual.bloom.bitset == signature.bloom.bitset);
        CHECK(manual.mask.outer == signature.mask.outer);
        for (usz i = 0; i < COMPONENT_MASK_WORDS; i++) {
            CHECK(manual.mask.words[i] == signature.mask.words[i]);
        }
    }
    SUBCASE("components alone leave the bloom empty") {
        Id components[] = { 1, 2, ECS::MAX_COMPONENT_ID };
        const ArchetypeSignature only_components = ArchetypeSignature::build(components, 3);
        CHECK(only_components.bloom.is_empty());
        CHECK(only_components.mask.has(ECS::MAX_COMPONENT_ID));
    }
    SUBCASE("an empty type has an empty signature") {
        const ArchetypeSignature empty = ArchetypeSignature::build(nullptr, 0);
        CHECK(empty.mask.is_empty());
        CHECK(empty.bloom.is_empty());
    }
}

// --- Archetype (world-backed) ------------------------------------------------

TEST_CASE("ecs/archetype: the root archetype is the empty type and stores nothing") {
    World world;
    world.init();

    Archetype* root = world.root_archetype;
    REQUIRE(root != nullptr);
    CHECK(root->type.id_count == 0);
    CHECK(root->data.column_count == 0);
    CHECK(root->data.entity_capacity == 0);
    CHECK(root->data.entity_count == 0);
    CHECK(root->world == &world);
    CHECK_FALSE(root->contains(1));
    CHECK(root->get_column(1) == nullptr);

    SUBCASE("a new entity is parked in the root") {
        const EntityId entity = world.new_entity();
        CHECK(archetype_of(world, entity) == root);
        CHECK(root->data.entity_count == 0);
    }
    SUBCASE("ensure_archetype with an empty type returns the root") {
        CHECK(Archetype::ensure_archetype(&world, ArchetypeType()) == root);
        Archetype** found = world.archetype_index.find(ArchetypeType());
        REQUIRE(found != nullptr);
        CHECK(*found == root);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/archetype: ensure_archetype dedupes by type") {
    World world;
    world.init();
    const Id position = world.component<Position>();
    const Id velocity = world.component<Velocity>();
    const usz before = world.archetypes.alive_count;

    u64 ids[] = { position < velocity ? position : velocity, position < velocity ? velocity : position };
    const ArchetypeType type(ids, 2);

    Archetype* first = Archetype::ensure_archetype(&world, type);
    REQUIRE(first != nullptr);
    CHECK(world.archetypes.alive_count == before + 1);
    CHECK(first->type == type);
    CHECK(first->type.ids != ids);

    SUBCASE("the same type again is the same archetype") {
        Archetype* again = Archetype::ensure_archetype(&world, type);
        CHECK(again == first);
        CHECK(world.archetypes.alive_count == before + 1);
    }
    SUBCASE("an entity with both components lands in it") {
        const EntityId entity = world.new_entity();
        world.set(entity, Position { 1, 2 });
        world.set(entity, Velocity { 3, 4 });
        CHECK(archetype_of(world, entity) == first);
        CHECK(world.archetypes.alive_count == before + 2);
    }
    SUBCASE("the archetype id resolves through the world's list") {
        CHECK(world.archetypes.get_element_alive(first->archetype_id) == first);
    }
    SUBCASE("the signature summarises the type") {
        CHECK(first->signature.mask.has(position));
        CHECK(first->signature.mask.has(velocity));
        CHECK(first->signature.bloom.is_empty());
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/archetype: columns are set up per id") {
    World world;
    world.init();
    const Id position = world.component<Position>();
    const Id tag = world.tag<TagA>();

    const EntityId entity = world.new_entity();
    world.set(entity, Position { 1.5f, 2.5f });
    world.add(entity, tag);
    Archetype* archetype = archetype_of(world, entity);

    CHECK(archetype->type.id_count == 2);
    CHECK(archetype->data.column_count == 2);
    CHECK(is_sorted(archetype->type));
    CHECK(archetype->contains(position));
    CHECK(archetype->contains(tag));
    CHECK_FALSE(archetype->contains(world.component<Velocity>()));

    SUBCASE("a component column carries its TypeInfo and data") {
        ArchetypeColumn* column = archetype->get_column(position);
        REQUIRE(column != nullptr);
        CHECK(column->type_info.length == sizeof(Position));
        CHECK(column->type_info.alignment == alignof(Position));
        CHECK(column->data != nullptr);
        const Position* stored = static_cast<const Position*>(column->read(0));
        CHECK(stored->x == 1.5f);
        CHECK(stored->y == 2.5f);
        CHECK(stored == world.get<Position>(entity));
    }
    SUBCASE("a tag column has zero length") {
        ArchetypeColumn* column = archetype->get_column(tag);
        REQUIRE(column != nullptr);
        CHECK(column->type_info.length == 0);
        CHECK(column->data == nullptr);
    }
    SUBCASE("column i stores type.ids[i]") {
        for (usz i = 0; i < archetype->type.id_count; i++) {
            const usz* index = archetype->columns_index.find(archetype->type.ids[i]);
            REQUIRE(index != nullptr);
            CHECK(*index == i);
            CHECK(archetype->get_column(archetype->type.ids[i]) == &archetype->data.columns[i]);
        }
    }
    SUBCASE("get_column for an id not in the type is null") {
        CHECK(archetype->get_column(world.component<Health>()) == nullptr);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/archetype: add and remove populate the edge pair") {
    World world;
    world.init();
    const Id position = world.component<Position>();
    const Id velocity = world.component<Velocity>();
    Archetype* root = world.root_archetype;

    const EntityId entity = world.new_entity();
    world.set(entity, Position { });
    Archetype* with_position = archetype_of(world, entity);
    REQUIRE(with_position != root);

    SUBCASE("root -> {Position} has a forward edge and a backwards edge") {
        Archetype** forward = root->forward_edges.find(position);
        REQUIRE(forward != nullptr);
        CHECK(*forward == with_position);
        Archetype** backwards = with_position->backwards_edges.find(position);
        REQUIRE(backwards != nullptr);
        CHECK(*backwards == root);
        CHECK(with_position->swapped_edges.is_empty());
        CHECK(with_position->swapped_backwards_edges.is_empty());
    }
    SUBCASE("adding a second id links the two archetypes both ways") {
        world.set(entity, Velocity { });
        Archetype* with_both = archetype_of(world, entity);
        REQUIRE(with_both != with_position);
        CHECK(with_both->type.id_count == 2);

        Archetype** forward = with_position->forward_edges.find(velocity);
        REQUIRE(forward != nullptr);
        CHECK(*forward == with_both);
        Archetype** backwards = with_both->backwards_edges.find(velocity);
        REQUIRE(backwards != nullptr);
        CHECK(*backwards == with_position);

        SUBCASE("removing it reuses the backwards edge") {
            const usz archetype_count = world.archetypes.alive_count;
            world.remove(entity, velocity);
            CHECK(archetype_of(world, entity) == with_position);
            CHECK(world.archetypes.alive_count == archetype_count);
            CHECK(with_both->data.entity_count == 0);
            CHECK_FALSE(with_both->alive);
            CHECK(with_position->data.entity_count == 1);
        }
    }
    SUBCASE("removing an id the archetype came from creates the edge pair the other way") {
        // Build {Position, Velocity} directly so no edge exists yet, then remove.
        const EntityId other = world.new_entity();
        world.set(other, Velocity { });
        world.set(other, Position { });
        Archetype* with_both = archetype_of(world, other);
        REQUIRE(with_both->backwards_edges.find(velocity) == nullptr);

        world.remove(other, velocity);
        CHECK(archetype_of(world, other) == with_position);
        Archetype** backwards = with_both->backwards_edges.find(velocity);
        REQUIRE(backwards != nullptr);
        CHECK(*backwards == with_position);
        Archetype** forward = with_position->forward_edges.find(velocity);
        REQUIRE(forward != nullptr);
        CHECK(*forward == with_both);
    }
    SUBCASE("traverse_add / traverse_remove answer without an entity") {
        Archetype* via_add = root->traverse_add(&world, position);
        CHECK(via_add == with_position);
        Archetype* via_remove = with_position->traverse_remove(&world, position);
        CHECK(via_remove == root);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/archetype: an exclusive relation swaps through a forward-only edge") {
    World world;
    world.init();
    const EntityId parent_a = world.new_entity();
    const EntityId parent_b = world.new_entity();
    const Id child_of_a = ECS::PAIR(ECS::CHILD_OF, parent_a);
    const Id child_of_b = ECS::PAIR(ECS::CHILD_OF, parent_b);

    const EntityId child = world.new_entity();
    world.add(child, child_of_a);
    Archetype* under_a = archetype_of(world, child);
    REQUIRE(under_a->contains(child_of_a));
    CHECK(under_a->contains(ECS::PAIR(ECS::CHILD_OF, ECS::WILDCARD)));

    world.add(child, child_of_b);
    Archetype* under_b = archetype_of(world, child);
    REQUIRE(under_b != under_a);
    CHECK(world.has(child, child_of_b));
    CHECK_FALSE(world.has(child, child_of_a));
    CHECK(under_b->type.id_count == 1);
    CHECK(under_b->type.ids[0] == child_of_b);

    SUBCASE("the source records the swap") {
        Archetype** forward = under_a->forward_edges.find(child_of_b);
        REQUIRE(forward != nullptr);
        CHECK(*forward == under_b);
        const Id* old_id = under_a->swapped_edges.find(child_of_b);
        REQUIRE(old_id != nullptr);
        CHECK(*old_id == child_of_a);
    }
    SUBCASE("the destination records the reverse link, not a backwards edge") {
        Archetype** source = under_b->swapped_backwards_edges.find(child_of_a);
        REQUIRE(source != nullptr);
        CHECK(*source == under_a);
        Archetype** backwards = under_b->backwards_edges.find(child_of_b);
        CHECK((backwards == nullptr || *backwards != under_a));
    }
    SUBCASE("the wildcard alias now points at the new pair's column") {
        CHECK(under_b->contains(ECS::PAIR(ECS::CHILD_OF, ECS::WILDCARD)));
        CHECK(under_b->get_column(ECS::PAIR(ECS::CHILD_OF, ECS::WILDCARD)) == under_b->get_column(child_of_b));
        CHECK(under_b->contains(ECS::PAIR(ECS::WILDCARD, parent_b)));
        CHECK_FALSE(under_b->contains(ECS::PAIR(ECS::WILDCARD, parent_a)));
    }
    SUBCASE("rows moved with the swap") {
        CHECK(under_a->data.entity_count == 0);
        CHECK(under_b->data.entity_count == 1);
        CHECK(under_b->data.entities[0] == child);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/archetype: entity_count follows inserts and deletes") {
    World world;
    world.init();

    EntityId entities[3];
    for (usz i = 0; i < 3; i++) {
        entities[i] = world.new_entity();
        world.set(entities[i], Health { static_cast<i32>(i + 1) });
    }
    Archetype* archetype = archetype_of(world, entities[0]);
    CHECK(archetype->data.entity_count == 3);
    CHECK(archetype->alive);
    for (usz i = 0; i < 3; i++) {
        CHECK(archetype->data.entities[i] == entities[i]);
        CHECK(world.entity_index.get_record_alive(entities[i])->archetype_row == i);
    }

    SUBCASE("deleting the first row swaps the last one in") {
        CHECK(world.delete_entity(entities[0]));
        CHECK(archetype->data.entity_count == 2);
        CHECK(archetype->data.entities[0] == entities[2]);
        CHECK(world.entity_index.get_record_alive(entities[2])->archetype_row == 0);
        CHECK(world.get<Health>(entities[2])->value == 3);
        CHECK(world.get<Health>(entities[1])->value == 2);
        CHECK(archetype->alive);
    }
    SUBCASE("deleting the last row touches nothing else") {
        CHECK(world.delete_entity(entities[2]));
        CHECK(archetype->data.entity_count == 2);
        CHECK(archetype->data.entities[0] == entities[0]);
        CHECK(archetype->data.entities[1] == entities[1]);
    }
    SUBCASE("deleting every entity marks the archetype dead") {
        for (usz i = 0; i < 3; i++) {
            CHECK(world.delete_entity(entities[i]));
        }
        CHECK(archetype->data.entity_count == 0);
        CHECK_FALSE(archetype->alive);
        CHECK(archetype->data.entity_capacity == ARCHETYPE_INITIAL_CAPACITY);

        SUBCASE("and a new row revives it") {
            const EntityId fresh = world.new_entity();
            world.set(fresh, Health { 9 });
            CHECK(archetype_of(world, fresh) == archetype);
            CHECK(archetype->alive);
            CHECK(archetype->data.entity_count == 1);
        }
    }
    SUBCASE("clearing an entity drops its row and parks it in the root") {
        world.clear(entities[1]);
        CHECK(archetype->data.entity_count == 2);
        CHECK(archetype_of(world, entities[1]) == world.root_archetype);
        CHECK(archetype->data.entities[1] == entities[2]);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/archetype: capacity starts at ARCHETYPE_INITIAL_CAPACITY and doubles") {
    World world;
    world.init();

    const EntityId first = world.new_entity();
    world.set(first, Position { 0, 0 });
    Archetype* archetype = archetype_of(world, first);
    CHECK(archetype->data.entity_capacity == ARCHETYPE_INITIAL_CAPACITY);

    SUBCASE("filling the initial capacity does not grow") {
        for (usz i = 1; i < ARCHETYPE_INITIAL_CAPACITY; i++) {
            world.set(world.new_entity(), Position { static_cast<f32>(i), 0 });
        }
        CHECK(archetype->data.entity_count == ARCHETYPE_INITIAL_CAPACITY);
        CHECK(archetype->data.entity_capacity == ARCHETYPE_INITIAL_CAPACITY);

        SUBCASE("one more row doubles it and keeps the data") {
            const EntityId extra = world.new_entity();
            world.set(extra, Position { 100, 0 });
            CHECK(archetype->data.entity_count == ARCHETYPE_INITIAL_CAPACITY + 1);
            CHECK(archetype->data.entity_capacity == ARCHETYPE_INITIAL_CAPACITY * 2);
            CHECK(world.get<Position>(first)->x == 0);
            CHECK(world.get<Position>(extra)->x == 100);
            for (usz i = 0; i < ARCHETYPE_INITIAL_CAPACITY + 1; i++) {
                const EntityId row_entity = archetype->data.entities[i];
                CHECK(world.entity_index.get_record_alive(row_entity)->archetype_row == i);
            }
        }
    }
    SUBCASE("ensure_capacity rounds up to a power of two and never shrinks") {
        archetype->ensure_capacity(100);
        CHECK(archetype->data.entity_capacity == 128);
        archetype->ensure_capacity(4);
        CHECK(archetype->data.entity_capacity == 128);
        archetype->ensure_capacity(129);
        CHECK(archetype->data.entity_capacity == 256);
        CHECK(world.get<Position>(first) != nullptr);
    }
    SUBCASE("a tag-only archetype also tracks capacity") {
        const EntityId tagged = world.new_entity();
        world.add<TagA>(tagged);
        Archetype* tag_archetype = archetype_of(world, tagged);
        CHECK(tag_archetype->data.entity_capacity == ARCHETYPE_INITIAL_CAPACITY);
        CHECK(tag_archetype->data.column_count == 1);
        CHECK(tag_archetype->data.columns[0].data == nullptr);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/archetype: move_entity copies shared columns") {
    World world;
    world.init();

    const EntityId entity = world.new_entity();
    world.set(entity, Position { 7, 8 });
    world.set(entity, Velocity { 1, 2 });
    Archetype* source = archetype_of(world, entity);

    world.add<TagA>(entity);
    Archetype* destination = archetype_of(world, entity);
    REQUIRE(destination != source);
    CHECK(destination->type.id_count == 3);
    CHECK(source->data.entity_count == 0);
    CHECK(destination->data.entity_count == 1);
    CHECK(world.get<Position>(entity)->x == 7);
    CHECK(world.get<Position>(entity)->y == 8);
    CHECK(world.get<Velocity>(entity)->dx == 1);
    CHECK(world.get<Velocity>(entity)->dy == 2);

    SUBCASE("moving back drops the tag column only") {
        world.remove<TagA>(entity);
        CHECK(archetype_of(world, entity) == source);
        CHECK(world.get<Position>(entity)->y == 8);
        CHECK(world.get<Velocity>(entity)->dx == 1);
    }
    SUBCASE("removing a component loses only that data") {
        world.remove<Velocity>(entity);
        CHECK(world.get<Velocity>(entity) == nullptr);
        CHECK(world.get<Position>(entity)->x == 7);
        CHECK(world.has<TagA>(entity));
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

// --- Liveness bookkeeping ----------------------------------------------------

TEST_CASE("ecs/archetype: the world counts alive and dead archetypes") {
    World world;
    world.init();

    // init() builds a few archetypes for the built-in ids; only the root and
    // the tables those ids ended up in hold rows.
    CHECK(world.alive_archetype_count + world.dead_archetype_count == world.archetypes.alive_count);
    CHECK(world.root_archetype->alive);
    const usz alive_baseline = world.alive_archetype_count;
    const usz dead_baseline = world.dead_archetype_count;

    const EntityId entity = world.new_entity();
    world.set(entity, Position { 1, 2 });
    Archetype* with_position = archetype_of(world, entity);

    SUBCASE("a new archetype is alive as soon as its first row lands") {
        CHECK(with_position->alive);
        CHECK(world.alive_archetype_count == alive_baseline + 1);
        CHECK(world.dead_archetype_count == dead_baseline);
    }
    SUBCASE("emptying it moves it to the dead count") {
        world.remove<Position>(entity);
        CHECK_FALSE(with_position->alive);
        CHECK(world.alive_archetype_count == alive_baseline);
        CHECK(world.dead_archetype_count == dead_baseline + 1);

        SUBCASE("and a new row moves it back") {
            world.set(entity, Position { 3, 4 });
            CHECK(archetype_of(world, entity) == with_position);
            CHECK(with_position->alive);
            CHECK(world.alive_archetype_count == alive_baseline + 1);
            CHECK(world.dead_archetype_count == dead_baseline);
        }
    }
    SUBCASE("moving between two archetypes touches the counts only on transitions") {
        world.set(entity, Velocity { });
        Archetype* with_both = archetype_of(world, entity);
        // {Position} emptied, {Position, Velocity} filled.
        CHECK_FALSE(with_position->alive);
        CHECK(with_both->alive);
        CHECK(world.alive_archetype_count == alive_baseline + 1);
        CHECK(world.dead_archetype_count == dead_baseline + 1);

        // A second entity passing through populated tables changes nothing.
        const EntityId other = world.new_entity();
        world.set(other, Position { });
        world.set(other, Velocity { });
        CHECK(world.alive_archetype_count == alive_baseline + 1);
        CHECK(world.dead_archetype_count == dead_baseline + 1);
        world.delete_entity(other);
        CHECK(world.alive_archetype_count == alive_baseline + 1);
        CHECK(world.dead_archetype_count == dead_baseline + 1);
    }
    SUBCASE("the counts always add up to the archetype slots in use") {
        for (usz i = 0; i < 8; i++) {
            const EntityId e = world.new_entity();
            world.set(e, Health { static_cast<i32>(i) });
            if (i % 2 == 0) {
                world.add<TagA>(e);
            }
            if (i % 3 == 0) {
                world.delete_entity(e);
            }
        }
        CHECK(world.alive_archetype_count + world.dead_archetype_count == world.archetypes.alive_count);
    }

    world.free();
    CHECK(world.alive_archetype_count == 0);
    CHECK(world.dead_archetype_count == 0);
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/archetype: mark_dead stamps died_at with the world clock") {
    World world;
    world.init();

    const EntityId entity = world.new_entity();
    world.set(entity, Position { });
    Archetype* archetype = archetype_of(world, entity);
    REQUIRE(archetype->alive);

    world.tick(5);
    world.tick(2);
    CHECK(world.clock == 7);

    SUBCASE("emptying the archetype records the time of death") {
        world.remove<Position>(entity);
        CHECK_FALSE(archetype->alive);
        CHECK(archetype->died_at == 7);

        world.tick();
        world.set(entity, Position { });
        world.remove<Position>(entity);
        CHECK(archetype->died_at == 8);
    }
    SUBCASE("a freshly created archetype is stamped dead at creation") {
        // Build the table without an entity so it stays empty.
        Archetype* fresh = archetype->traverse_add(&world, world.id<Velocity>());
        REQUIRE(fresh != archetype);
        CHECK_FALSE(fresh->alive);
        CHECK(fresh->died_at == 7);
        CHECK(fresh->data.entity_count == 0);
    }
    SUBCASE("mark_alive / mark_dead are idempotent") {
        const usz alive_count = world.alive_archetype_count;
        const usz dead_count = world.dead_archetype_count;
        archetype->mark_alive();
        archetype->mark_alive();
        CHECK(world.alive_archetype_count == alive_count);
        CHECK(world.dead_archetype_count == dead_count);

        world.remove<Position>(entity);
        REQUIRE_FALSE(archetype->alive);
        CHECK(world.alive_archetype_count == alive_count - 1);
        CHECK(world.dead_archetype_count == dead_count + 1);
        world.tick();
        archetype->mark_dead();
        // Still stamped by the real transition, not the redundant call.
        CHECK(archetype->died_at == 7);
        CHECK(world.alive_archetype_count == alive_count - 1);
        CHECK(world.dead_archetype_count == dead_count + 1);
    }
    SUBCASE("free() resets the clock") {
        world.free();
        CHECK(world.clock == 0);
        world.init();
    }

    world.free();
    CHECK_ARENA_CLEAN();
}
