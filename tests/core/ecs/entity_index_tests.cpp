#include "support/test_support.hpp"

#include "engine/ecs/ecs.hpp"
#include "engine/ecs/entity_index.hpp"
#include "engine/memory/heap_allocator.hpp"

// The EntityIndex is standalone: no World is involved, so every test builds
// one on the heap allocator and calls free() at the end.

namespace {

constexpr EntityId with_generation(const EntityIdLow id, const EntityGeneration generation) {
    return ENTITY_INDEX::append_generation(id, generation);
}

// --- Constexpr helpers -------------------------------------------------------

static_assert(ENTITY_INDEX::PAGE_SIZE == 1024);
static_assert(ENTITY_INDEX::PAGE_MASK == 1023);
static_assert(ENTITY_INDEX::entity_generation(with_generation(5, 3)) == 3);
static_assert(ENTITY_INDEX::entity_generation(5) == 0);
static_assert(ECS::ENTITY_LOW(with_generation(5, 3)) == 5);
static_assert(ENTITY_INDEX::get_page_index(0) == 0);
static_assert(ENTITY_INDEX::get_page_index(1023) == 0);
static_assert(ENTITY_INDEX::get_page_index(1024) == 1);
static_assert(ENTITY_INDEX::get_page_index(with_generation(1024, 9)) == 1, "the generation does not pick the page");
static_assert(ENTITY_INDEX::get_page_offset(1023) == 1023);
static_assert(ENTITY_INDEX::get_page_offset(1024) == 0);
static_assert(ENTITY_INDEX::get_page_offset(1025) == 1);
static_assert(ENTITY_INDEX::increment_generation(5) == with_generation(5, 1));
static_assert(ENTITY_INDEX::increment_generation(with_generation(5, 1)) == with_generation(5, 2));
static_assert(ENTITY_INDEX::increment_generation(with_generation(5, ECS::GENERATION_SIZE - 1)) == with_generation(5, 1),
    "wrap-around skips generation 0");

} // namespace

TEST_CASE("ecs/entity_index: constexpr helpers split and rebuild ids") {
    const EntityId entity = with_generation(77, 4);
    CHECK(ENTITY_INDEX::entity_generation(entity) == 4);
    CHECK(ECS::ENTITY_LOW(entity) == 77);
    CHECK(ENTITY_INDEX::append_generation(ECS::ENTITY_LOW(entity), ENTITY_INDEX::entity_generation(entity)) == entity);
    CHECK(ENTITY_INDEX::get_page_index(entity) == 0);
    CHECK(ENTITY_INDEX::get_page_offset(entity) == 77);

    SUBCASE("increment_generation keeps the low id") {
        const EntityId next = ENTITY_INDEX::increment_generation(entity);
        CHECK(ECS::ENTITY_LOW(next) == 77);
        CHECK(ENTITY_INDEX::entity_generation(next) == 5);
    }
    SUBCASE("increment_generation wraps to 1, never 0") {
        const EntityId last = with_generation(77, ECS::GENERATION_SIZE - 1);
        const EntityId wrapped = ENTITY_INDEX::increment_generation(last);
        CHECK(ENTITY_INDEX::entity_generation(wrapped) == 1);
        CHECK(ECS::ENTITY_LOW(wrapped) == 77);
    }
}

TEST_CASE("ecs/entity_index: a fresh index is empty") {
    EntityIndex index(MEMORY::heap_allocator());
    CHECK(index.is_empty());
    CHECK(index.count() == 0);
    CHECK_FALSE(index.is_alive(0));
    CHECK_FALSE(index.is_alive(1));
    CHECK(index.get_record_alive(1) == nullptr);
    CHECK(index.get_record_any(1) == nullptr);
    CHECK(index.get_current(1) == 0);
    index.free();
}

TEST_CASE("ecs/entity_index: new_entity issues sequential ids with generation 0") {
    EntityIndex index(MEMORY::heap_allocator());

    const EntityId a = index.new_entity();
    const EntityId b = index.new_entity();
    const EntityId c = index.new_entity();
    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(c == 3);
    CHECK(ENTITY_INDEX::entity_generation(c) == 0);
    CHECK(index.count() == 3);
    CHECK_FALSE(index.is_empty());
    CHECK(index.last_id == 3);

    SUBCASE("every issued id is alive") {
        CHECK(index.is_alive(a));
        CHECK(index.is_alive(b));
        CHECK(index.is_alive(c));
        CHECK_FALSE(index.is_alive(4));
        CHECK_FALSE(index.is_alive(0));
    }
    SUBCASE("get_alive_id walks the alive ids in order") {
        CHECK(index.get_alive_id(0) == a);
        CHECK(index.get_alive_id(1) == b);
        CHECK(index.get_alive_id(2) == c);
    }
    SUBCASE("out_record receives a cleared record") {
        EntityRecord* record = nullptr;
        const EntityId d = index.new_entity(&record);
        REQUIRE(record != nullptr);
        CHECK(record == index.get_record_alive(d));
        CHECK(record->dense != 0);
        CHECK(record->archetype == nullptr);
        CHECK(record->archetype_row == 0);
        CHECK(record->flags == 0);
    }

    index.free();
}

TEST_CASE("ecs/entity_index: get_record_alive rejects stale generations, get_record_any does not") {
    EntityIndex index(MEMORY::heap_allocator());
    const EntityId entity = index.new_entity();
    const EntityId stale = with_generation(entity, 5);

    CHECK(index.get_record_alive(entity) != nullptr);
    CHECK(index.get_record_alive(stale) == nullptr);
    CHECK_FALSE(index.is_alive(stale));
    CHECK(index.get_record_any(stale) == index.get_record_any(entity));
    CHECK(index.get_record_any(stale) == index.get_record_alive(entity));

    SUBCASE("get_current resolves any generation to the alive id") {
        CHECK(index.get_current(stale) == entity);
        CHECK(index.get_current(entity) == entity);
    }
    SUBCASE("a slot on an allocated page that was never issued is not alive") {
        CHECK(index.get_record_any(500) != nullptr);
        CHECK(index.get_record_any(500)->dense == 0);
        CHECK(index.get_record_alive(500) == nullptr);
        CHECK(index.get_current(500) == 0);
    }
    SUBCASE("a page nothing touched is not allocated") {
        CHECK(index.get_record_any(ENTITY_INDEX::PAGE_SIZE * 3) == nullptr);
        CHECK(index.get_record_alive(ENTITY_INDEX::PAGE_SIZE * 3) == nullptr);
    }

    index.free();
}

TEST_CASE("ecs/entity_index: delete_entity bumps the generation") {
    EntityIndex index(MEMORY::heap_allocator());
    const EntityId entity = index.new_entity();
    EntityRecord* record = index.get_record_alive(entity);
    record->archetype_row = 9;
    record->flags = ENTITY_RECORD_DELETING;

    CHECK(index.delete_entity(entity));
    CHECK_FALSE(index.is_alive(entity));
    CHECK(index.count() == 0);
    CHECK(index.is_empty());
    CHECK(index.get_record_alive(entity) == nullptr);
    CHECK(index.get_current(entity) == 0);

    SUBCASE("deleting again fails") {
        CHECK_FALSE(index.delete_entity(entity));
        CHECK_FALSE(index.delete_entity(with_generation(entity, 1)));
    }
    SUBCASE("the record is cleared but still reachable") {
        EntityRecord* any = index.get_record_any(entity);
        REQUIRE(any == record);
        CHECK(any->archetype == nullptr);
        CHECK(any->archetype_row == 0);
        CHECK(any->flags == 0);
    }
    SUBCASE("the recycled id carries generation 1") {
        const EntityId reused = index.new_entity();
        CHECK(ECS::ENTITY_LOW(reused) == ECS::ENTITY_LOW(entity));
        CHECK(ENTITY_INDEX::entity_generation(reused) == 1);
        CHECK(index.is_alive(reused));
        CHECK_FALSE(index.is_alive(entity));
        CHECK(index.get_current(entity) == reused);
        CHECK(index.last_id == 1);
    }
    SUBCASE("each delete bumps once more") {
        const EntityId second = index.new_entity();
        CHECK(index.delete_entity(second));
        const EntityId third = index.new_entity();
        CHECK(ENTITY_INDEX::entity_generation(third) == 2);
        CHECK(ECS::ENTITY_LOW(third) == ECS::ENTITY_LOW(entity));
    }

    index.free();
}

TEST_CASE("ecs/entity_index: dead ids are recycled before fresh ones are issued") {
    EntityIndex index(MEMORY::heap_allocator());
    const EntityId a = index.new_entity();
    const EntityId b = index.new_entity();
    const EntityId c = index.new_entity();
    (void)a;
    (void)c;

    CHECK(index.delete_entity(b));
    const EntityId reused = index.new_entity();
    CHECK(ECS::ENTITY_LOW(reused) == 2);
    CHECK(ENTITY_INDEX::entity_generation(reused) == 1);

    const EntityId fresh = index.new_entity();
    CHECK(fresh == 4);
    CHECK(index.count() == 4);
    CHECK(index.last_id == 4);

    index.free();
}

TEST_CASE("ecs/entity_index: deleting moves the last alive id into the hole") {
    EntityIndex index(MEMORY::heap_allocator());
    const EntityId a = index.new_entity();
    const EntityId b = index.new_entity();
    const EntityId c = index.new_entity();

    CHECK(index.delete_entity(a));
    CHECK(index.count() == 2);
    CHECK(index.get_alive_id(0) == c);
    CHECK(index.get_alive_id(1) == b);
    CHECK(index.get_record_alive(c)->dense == 1);
    CHECK(index.get_record_alive(b)->dense == 2);

    SUBCASE("deleting the last alive id leaves the others in place") {
        CHECK(index.delete_entity(b));
        CHECK(index.count() == 1);
        CHECK(index.get_alive_id(0) == c);
    }

    index.free();
}

TEST_CASE("ecs/entity_index: make_alive registers an exact id") {
    EntityIndex index(MEMORY::heap_allocator());

    SUBCASE("a fresh id with a generation, on a page far ahead") {
        const EntityId entity = with_generation(3000, 7);
        EntityRecord* record = index.make_alive(entity);
        REQUIRE(record != nullptr);
        CHECK(record == index.get_record_alive(entity));
        CHECK(record->archetype == nullptr);
        CHECK(index.is_alive(entity));
        CHECK_FALSE(index.is_alive(3000));
        CHECK(index.get_current(3000) == entity);
        CHECK(index.count() == 1);
        CHECK(index.last_id == 3000);
        // Pages 0 and 1 were skipped and stay unallocated.
        CHECK(index.pages.count == 3);
        CHECK(index.pages[0] == nullptr);
        CHECK(index.pages[1] == nullptr);
        CHECK(index.pages[2] != nullptr);
        // The next fresh id follows the highest registered one.
        CHECK(index.new_entity() == 3001);
    }
    SUBCASE("an already alive id adopts the generation given") {
        const EntityId entity = index.new_entity();
        const EntityId regenerated = with_generation(entity, 5);
        EntityRecord* record = index.make_alive(regenerated);
        CHECK(record == index.get_record_any(entity));
        CHECK(index.is_alive(regenerated));
        CHECK_FALSE(index.is_alive(entity));
        CHECK(index.count() == 1);
    }
    SUBCASE("a dead id is revived under the generation given") {
        const EntityId entity = index.new_entity();
        CHECK(index.delete_entity(entity));
        const EntityId revived = with_generation(entity, 9);
        EntityRecord* record = index.make_alive(revived);
        CHECK(record != nullptr);
        CHECK(index.is_alive(revived));
        CHECK(index.count() == 1);
        CHECK(index.get_alive_id(0) == revived);
        // Nothing is left in the dead pool to recycle.
        CHECK(index.new_entity() == 2);
    }

    index.free();
}

TEST_CASE("ecs/entity_index: set_range bounds the ids new_entity issues") {
    EntityIndex index(MEMORY::heap_allocator());

    SUBCASE("ids start at min") {
        index.set_range(100, 0);
        CHECK(index.new_entity() == 100);
        CHECK(index.new_entity() == 101);
    }
    SUBCASE("a min of 0 starts after the highest id issued") {
        index.make_alive(50);
        index.set_range(0, 0);
        CHECK(index.range_min == 51);
        CHECK(index.new_entity() == 51);
    }
    SUBCASE("an exhausted range yields 0 and a null record") {
        index.set_range(1, 2);
        CHECK(index.new_entity() == 1);
        CHECK(index.new_entity() == 2);
        EntityRecord placeholder;
        EntityRecord* record = &placeholder;
        CHECK(index.new_entity(&record) == 0);
        CHECK(record == nullptr);
        CHECK(index.count() == 2);

        // A recycled in-range id is still handed out.
        CHECK(index.delete_entity(1));
        CHECK(index.new_entity() == with_generation(1, 1));
    }
    SUBCASE("deleting an out-of-range id drops it instead of recycling it") {
        index.set_range(100, 0);
        const EntityId in_range = index.new_entity();
        CHECK(in_range == 100);
        index.make_alive(5);
        CHECK(index.count() == 2);
        const usz dense_count = index.dense_list.count;

        CHECK(index.delete_entity(5));
        CHECK(index.count() == 1);
        CHECK(index.dense_list.count == dense_count - 1);
        CHECK_FALSE(index.is_alive(5));
        CHECK(index.get_record_any(5)->dense == 0);
        // The next id is fresh, not the dropped 5.
        CHECK(index.new_entity() == 101);
    }
    SUBCASE("an in-range delete still recycles") {
        index.set_range(10, 20);
        const EntityId entity = index.new_entity();
        CHECK(index.delete_entity(entity));
        CHECK(index.new_entity() == with_generation(entity, 1));
    }

    index.free();
}

TEST_CASE("ecs/entity_index: clear kills every entity but keeps the pages") {
    EntityIndex index(MEMORY::heap_allocator());
    const EntityId a = index.new_entity();
    const EntityId b = index.new_entity();
    EntityRecord* record_a = index.get_record_alive(a);
    record_a->archetype_row = 3;
    const usz page_count = index.pages.count;

    index.clear();
    CHECK(index.is_empty());
    CHECK(index.count() == 0);
    CHECK_FALSE(index.is_alive(a));
    CHECK_FALSE(index.is_alive(b));
    CHECK(index.pages.count == page_count);
    CHECK(index.get_record_any(a) == record_a);
    CHECK(record_a->archetype_row == 0);

    SUBCASE("the killed ids come back with a bumped generation") {
        const EntityId first = index.new_entity();
        const EntityId second = index.new_entity();
        CHECK(ENTITY_INDEX::entity_generation(first) == 1);
        CHECK(ENTITY_INDEX::entity_generation(second) == 1);
        CHECK(ECS::ENTITY_LOW(first) + ECS::ENTITY_LOW(second) == 3);
        CHECK(index.count() == 2);
        CHECK(index.new_entity() == 3);
    }

    index.free();
}

TEST_CASE("ecs/entity_index: free returns to the fresh state") {
    EntityIndex index(MEMORY::heap_allocator());
    index.set_range(10, 0);
    const EntityId entity = index.new_entity();
    CHECK(entity == 10);

    index.free();
    CHECK(index.is_empty());
    CHECK(index.count() == 0);
    CHECK(index.pages.count == 0);
    CHECK(index.dense_list.count == 0);
    CHECK(index.last_id == 0);
    CHECK(index.range_min == 0);
    CHECK(index.range_max == 0);
    CHECK_FALSE(index.is_alive(entity));

    SUBCASE("the index is usable again") {
        CHECK(index.new_entity() == 1);
        CHECK(index.count() == 1);
        index.free();
    }
    SUBCASE("freeing twice is harmless") {
        index.free();
        CHECK(index.is_empty());
    }
}

TEST_CASE("ecs/entity_index: pages fill at PAGE_SIZE and records stay put") {
    EntityIndex index(MEMORY::heap_allocator());

    EntityRecord* first_record = nullptr;
    const EntityId first = index.new_entity(&first_record);
    REQUIRE(first == 1);

    EntityId last = first;
    for (usz i = 1; i < ENTITY_INDEX::PAGE_SIZE - 1; i++) {
        last = index.new_entity();
    }
    CHECK(last == ENTITY_INDEX::PAGE_SIZE - 1);
    CHECK(index.pages.count == 1);
    CHECK(ENTITY_INDEX::get_page_index(last) == 0);

    const EntityId boundary = index.new_entity();
    CHECK(boundary == ENTITY_INDEX::PAGE_SIZE);
    CHECK(ENTITY_INDEX::get_page_index(boundary) == 1);
    CHECK(ENTITY_INDEX::get_page_offset(boundary) == 0);
    CHECK(index.pages.count == 2);
    CHECK(index.pages[1] != nullptr);
    CHECK(index.get_record_alive(boundary) == index.pages[1]);
    CHECK(index.get_record_alive(last) == index.pages[0] + (ENTITY_INDEX::PAGE_SIZE - 1));

    SUBCASE("records never move when more pages are added") {
        for (usz i = 0; i < ENTITY_INDEX::PAGE_SIZE * 2; i++) {
            index.new_entity();
        }
        CHECK(index.pages.count == 4);
        CHECK(index.get_record_alive(first) == first_record);
        CHECK(index.get_record_alive(boundary) == index.pages[1]);
        CHECK(index.count() == ENTITY_INDEX::PAGE_SIZE * 3);
    }

    index.free();
}
