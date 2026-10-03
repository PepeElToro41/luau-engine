#include "support/test_support.hpp"

#include "engine/memory/arena_allocator.hpp"
#include "engine/templates/sparse_list.hpp"

namespace {

struct Payload {
    i32 a = 0;
    i32 b = 0;
};

} // namespace

TEST_CASE("templates/sparse_list: id helpers pack a slot index and a generation") {
    using namespace SPARSE_LIST;

    const SparseId id = append_generation(5, 3);
    CHECK(element_low(id) == 5);
    CHECK(element_generation(id) == 3);
    CHECK(id != 5);

    CHECK(get_page_index(0) == 0);
    CHECK(get_page_index(63) == 0);
    CHECK(get_page_index(64) == 1);
    CHECK(get_page_offset(64) == 0);
    CHECK(get_page_offset(65) == 1);
    // The generation never leaks into the page lookup.
    CHECK(get_page_index(append_generation(70, 9)) == 1);
    CHECK(get_page_offset(append_generation(70, 9)) == 6);

    SUBCASE("increment_generation bumps the high bits and skips 0 on wrap-around") {
        CHECK(increment_generation(5) == append_generation(5, 1));
        CHECK(increment_generation(append_generation(5, 1)) == append_generation(5, 2));
        const SparseId last = append_generation(5, GENERATION_SIZE - 1);
        CHECK(increment_generation(last) == append_generation(5, 1));
        CHECK(element_low(increment_generation(last)) == 5);
    }
}

TEST_CASE("templates/sparse_list: a fresh list has no pages and reports nothing alive") {
    SparseList<Payload> list;
    CHECK(list.alive_count == 0);
    CHECK(list.next_element == 0);
    CHECK(list.is_empty());
    CHECK(list.sparse_pages.count == 0);
    CHECK(list.dense_list.count == 0);
    CHECK(list.allocator == MEMORY::heap_allocator());
    CHECK(list.sparse_pages.allocator == MEMORY::heap_allocator());
    CHECK(list.dense_list.allocator == MEMORY::heap_allocator());

    CHECK_FALSE(list.is_alive(0));
    CHECK(list.get_element_alive(0) == nullptr);
    CHECK(list.get_element_any(0) == nullptr);
    CHECK_FALSE(list.delete_element(0));

    // clear() and free() on a never-used list are valid no-ops.
    list.clear();
    list.free();
    CHECK(list.sparse_pages.count == 0);
}

TEST_CASE("templates/sparse_list: new_element hands out sequential zeroed slots") {
    SparseList<Payload> list;

    const SparseId first = list.new_element();
    const SparseId second = list.new_element();
    const SparseId third = list.new_element();
    CHECK(first == 0);
    CHECK(second == 1);
    CHECK(third == 2);
    CHECK(list.alive_count == 3);
    CHECK(list.next_element == 3);
    CHECK(list.sparse_pages.count == 1);
    CHECK_FALSE(list.is_empty());

    for (SparseId id : {first, second, third}) {
        CHECK(list.is_alive(id));
        Payload* element = list.get_element_alive(id);
        REQUIRE(element != nullptr);
        CHECK(element->a == 0);
        CHECK(element->b == 0);
        CHECK(element == list.get_element_any(id));
    }

    // Elements are distinct storage.
    list.get_element_alive(first)->a = 1;
    list.get_element_alive(second)->a = 2;
    CHECK(list.get_element_alive(first)->a == 1);
    CHECK(list.get_element_alive(second)->a == 2);
    CHECK(list.get_element_alive(third)->a == 0);

    // Never-issued ids on the allocated page are not alive.
    CHECK_FALSE(list.is_alive(3));
    CHECK(list.get_element_alive(3) == nullptr);
    CHECK(list.get_element_any(3) != nullptr);

    list.free();
}

TEST_CASE("templates/sparse_list: get_alive_id enumerates the dense range") {
    SparseList<Payload> list;
    SparseId ids[5];
    for (SparseId& id : ids) {
        id = list.new_element();
    }

    for (usz i = 0; i < list.alive_count; ++i) {
        CHECK(list.get_alive_id(i) == ids[i]);
    }

    // Deleting from the middle moves the last alive id into the hole so the
    // range [0, alive_count) stays dense.
    CHECK(list.delete_element(ids[1]));
    CHECK(list.alive_count == 4);
    CHECK(list.get_alive_id(0) == ids[0]);
    CHECK(list.get_alive_id(1) == ids[4]);
    CHECK(list.get_alive_id(2) == ids[2]);
    CHECK(list.get_alive_id(3) == ids[3]);
    for (usz i = 0; i < list.alive_count; ++i) {
        CHECK(list.is_alive(list.get_alive_id(i)));
    }

    list.free();
}

TEST_CASE("templates/sparse_list: delete_element kills the id and reports stale lookups") {
    SparseList<Payload> list;
    const SparseId a = list.new_element();
    const SparseId b = list.new_element();
    const SparseId c = list.new_element();
    list.get_element_alive(b)->a = 42;

    SUBCASE("deleting the middle element keeps the others alive") {
        CHECK(list.delete_element(b));
        CHECK(list.alive_count == 2);
        CHECK_FALSE(list.is_alive(b));
        CHECK(list.get_element_alive(b) == nullptr);
        CHECK(list.is_alive(a));
        CHECK(list.is_alive(c));
        CHECK(list.get_element_alive(a) != nullptr);
        CHECK(list.get_element_alive(c) != nullptr);

        // The slot's memory is still reachable through the "any" lookup.
        Payload* dead = list.get_element_any(b);
        REQUIRE(dead != nullptr);
        CHECK(dead->a == 42);
    }

    SUBCASE("deleting the last element") {
        CHECK(list.delete_element(c));
        CHECK(list.alive_count == 2);
        CHECK_FALSE(list.is_alive(c));
        CHECK(list.is_alive(a));
        CHECK(list.is_alive(b));
    }

    SUBCASE("deleting twice fails the second time") {
        CHECK(list.delete_element(a));
        CHECK_FALSE(list.delete_element(a));
        CHECK(list.alive_count == 2);
    }

    SUBCASE("ids that were never issued cannot be deleted") {
        CHECK_FALSE(list.delete_element(3));
        CHECK_FALSE(list.delete_element(1000));
        CHECK_FALSE(list.delete_element(SPARSE_LIST::append_generation(a, 1)));
        CHECK(list.alive_count == 3);
    }

    list.free();
}

TEST_CASE("templates/sparse_list: deleted slots are revived with a bumped generation") {
    SparseList<Payload> list;
    const SparseId original = list.new_element();
    list.get_element_alive(original)->a = 7;
    list.get_element_alive(original)->b = 8;
    CHECK(list.delete_element(original));

    const SparseId revived = list.new_element();
    CHECK(SPARSE_LIST::element_low(revived) == SPARSE_LIST::element_low(original));
    CHECK(SPARSE_LIST::element_generation(revived) == 1);
    CHECK(revived != original);
    // No new slot was handed out.
    CHECK(list.next_element == 1);
    CHECK(list.dense_list.count == 1);
    CHECK(list.alive_count == 1);

    // The stale id no longer matches, the fresh one does, and the slot was
    // zeroed again.
    CHECK_FALSE(list.is_alive(original));
    CHECK(list.get_element_alive(original) == nullptr);
    CHECK(list.is_alive(revived));
    Payload* element = list.get_element_alive(revived);
    REQUIRE(element != nullptr);
    CHECK(element->a == 0);
    CHECK(element->b == 0);
    // The stale id and the live id address the same storage.
    CHECK(list.get_element_any(original) == element);

    SUBCASE("each delete bumps the generation again") {
        CHECK(list.delete_element(revived));
        const SparseId again = list.new_element();
        CHECK(SPARSE_LIST::element_generation(again) == 2);
        CHECK_FALSE(list.is_alive(revived));
        CHECK(list.is_alive(again));
    }

    SUBCASE("new slots are only issued once every dead one is reused") {
        const SparseId fresh = list.new_element();
        CHECK(fresh == 1);
        CHECK(list.next_element == 2);
        CHECK(list.alive_count == 2);

        list.delete_element(revived);
        list.delete_element(fresh);
        CHECK(list.alive_count == 0);
        CHECK(list.is_empty());

        const SparseId first = list.new_element();
        const SparseId second = list.new_element();
        const SparseId third = list.new_element();
        CHECK(list.next_element == 3);
        CHECK(SPARSE_LIST::element_low(third) == 2);
        CHECK(SPARSE_LIST::element_generation(third) == 0);
        // The two revived slots are the recycled ones, each bumped once more.
        CHECK(SPARSE_LIST::element_low(first) != SPARSE_LIST::element_low(second));
        CHECK(SPARSE_LIST::element_low(first) < 2);
        CHECK(SPARSE_LIST::element_low(second) < 2);
        CHECK(SPARSE_LIST::element_generation(first) >= 1);
        CHECK(SPARSE_LIST::element_generation(second) >= 1);
    }

    list.free();
}

TEST_CASE("templates/sparse_list: element pointers stay valid while new pages are added") {
    SparseList<Payload> list;
    const SparseId early = list.new_element();
    Payload* pointer = list.get_element_alive(early);
    REQUIRE(pointer != nullptr);
    pointer->a = 123;
    pointer->b = 456;

    // Spill into several pages: the page table array grows and relocates its
    // SparsePage records, but the element storage itself never moves.
    const usz total = SPARSE_LIST::PAGE_SIZE * 20;
    for (usz i = 1; i < total; ++i) {
        list.new_element();
    }
    CHECK(list.alive_count == total);
    CHECK(list.sparse_pages.count == 20);
    CHECK(list.get_element_alive(early) == pointer);
    CHECK(pointer->a == 123);
    CHECK(pointer->b == 456);

    // Ids on later pages map to their own page.
    const SparseId last = list.get_alive_id(total - 1);
    CHECK(SPARSE_LIST::get_page_index(last) == 19);
    CHECK(list.is_alive(last));
    CHECK(list.get_element_alive(last) != nullptr);
    CHECK(list.get_element_alive(last) != pointer);

    // Deleting an element elsewhere does not move survivors either.
    CHECK(list.delete_element(list.get_alive_id(5)));
    CHECK(list.get_element_alive(early) == pointer);
    CHECK(pointer->a == 123);

    list.free();
}

TEST_CASE("templates/sparse_list: clear kills every element but keeps slots and pages") {
    SparseList<Payload> list;
    SparseId ids[100];
    for (SparseId& id : ids) {
        id = list.new_element();
    }
    const usz pages = list.sparse_pages.count;
    CHECK(pages == 2);

    list.clear();
    CHECK(list.alive_count == 0);
    CHECK(list.is_empty());
    CHECK(list.sparse_pages.count == pages);
    CHECK(list.dense_list.count == 100);
    CHECK(list.next_element == 100);
    for (SparseId id : ids) {
        CHECK_FALSE(list.is_alive(id));
        CHECK(list.get_element_alive(id) == nullptr);
        CHECK_FALSE(list.delete_element(id));
    }

    // Every slot is reused before a new one is issued, all with a bumped
    // generation.
    for (usz i = 0; i < 100; ++i) {
        const SparseId revived = list.new_element();
        CHECK(SPARSE_LIST::element_generation(revived) == 1);
        CHECK(SPARSE_LIST::element_low(revived) < 100);
        CHECK(list.is_alive(revived));
    }
    CHECK(list.next_element == 100);
    CHECK(list.sparse_pages.count == pages);

    const SparseId fresh = list.new_element();
    CHECK(fresh == 100);
    CHECK(list.next_element == 101);

    list.free();
}

TEST_CASE("templates/sparse_list: free releases pages and restarts ids from zero") {
    SparseList<Payload> list;
    for (int i = 0; i < 70; ++i) {
        list.new_element();
    }
    list.delete_element(3);
    CHECK(list.sparse_pages.count == 2);

    list.free();
    CHECK(list.alive_count == 0);
    CHECK(list.next_element == 0);
    CHECK(list.sparse_pages.count == 0);
    CHECK(list.sparse_pages.data == nullptr);
    CHECK(list.dense_list.count == 0);
    CHECK(list.dense_list.data == nullptr);
    CHECK_FALSE(list.is_alive(0));
    CHECK(list.get_element_any(0) == nullptr);

    // A freed list starts over: ids are fresh, not recycled.
    const SparseId id = list.new_element();
    CHECK(id == 0);
    CHECK(list.is_alive(id));
    CHECK(list.sparse_pages.count == 1);

    list.free();
}

TEST_CASE("templates/sparse_list: move constructor and assignment leave the source empty") {
    SparseList<Payload> source;
    SparseId ids[3];
    for (SparseId& id : ids) {
        id = source.new_element();
    }
    source.get_element_alive(ids[2])->a = 9;
    source.delete_element(ids[0]);
    Payload* stable = source.get_element_alive(ids[2]);

    SUBCASE("move construction transfers the pages and the id list") {
        SparseList<Payload> target(std::move(source));
        CHECK(target.alive_count == 2);
        CHECK(target.next_element == 3);
        CHECK(target.sparse_pages.count == 1);
        CHECK(target.dense_list.count == 3);
        CHECK(target.is_alive(ids[1]));
        CHECK(target.is_alive(ids[2]));
        CHECK_FALSE(target.is_alive(ids[0]));
        CHECK(target.get_element_alive(ids[2]) == stable);
        CHECK(stable->a == 9);

        CHECK(source.alive_count == 0);
        CHECK(source.next_element == 0);
        CHECK(source.sparse_pages.count == 0);
        CHECK(source.sparse_pages.data == nullptr);
        CHECK(source.dense_list.count == 0);
        CHECK(source.dense_list.data == nullptr);
        CHECK_FALSE(source.is_alive(ids[1]));

        target.free();
    }

    SUBCASE("move assignment frees the old contents and takes the new ones") {
        SparseList<Payload> target;
        for (int i = 0; i < 200; ++i) {
            target.new_element();
        }

        target = std::move(source);
        CHECK(target.alive_count == 2);
        CHECK(target.next_element == 3);
        CHECK(target.sparse_pages.count == 1);
        CHECK(target.get_element_alive(ids[2]) == stable);
        CHECK_FALSE(target.is_alive(150));

        CHECK(source.alive_count == 0);
        CHECK(source.sparse_pages.count == 0);
        CHECK(source.dense_list.count == 0);

        target.free();
    }

    source.free();
}

TEST_CASE("templates/sparse_list: an explicit allocator backs pages and the id list") {
    alignas(64) static char buffer[256 * 1024];
    ArenaAllocator arena(buffer, sizeof(buffer));

    SparseList<Payload> list(&arena);
    CHECK(list.allocator == &arena);
    CHECK(list.sparse_pages.allocator == &arena);
    CHECK(list.dense_list.allocator == &arena);
    CHECK(arena.offset == 0);

    const SparseId id = list.new_element();
    CHECK(arena.offset > 0);
    Payload* element = list.get_element_alive(id);
    REQUIRE(element != nullptr);
    CHECK(reinterpret_cast<char*>(element) >= buffer);
    CHECK(reinterpret_cast<char*>(element) < buffer + sizeof(buffer));
    CHECK(reinterpret_cast<char*>(list.dense_list.data) >= buffer);
    CHECK(reinterpret_cast<char*>(list.dense_list.data) < buffer + sizeof(buffer));

    for (int i = 0; i < 300; ++i) {
        list.new_element();
    }
    CHECK(list.sparse_pages.count == 5);
    CHECK(list.get_element_alive(id) == element);
    for (const SparsePage<Payload>& page : list.sparse_pages) {
        CHECK(reinterpret_cast<char*>(page.data) >= buffer);
        CHECK(reinterpret_cast<char*>(page.data) < buffer + sizeof(buffer));
        CHECK(reinterpret_cast<char*>(page.dense) >= buffer);
        CHECK(reinterpret_cast<char*>(page.dense) < buffer + sizeof(buffer));
    }

    list.free();
    arena.reset();

    SUBCASE("a temporal allocator reclaims everything when its scope ends") {
        {
            TemporalAllocator temp = TemporalAllocator::create();
            SparseList<Payload> scratch(&temp);
            for (int i = 0; i < 100; ++i) {
                scratch.new_element();
            }
            CHECK(scratch.alive_count == 100);
            CHECK(MAIN_ARENA.offset > 0);
        }
        CHECK_ARENA_CLEAN();
    }
}
