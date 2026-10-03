#include "support/test_support.hpp"

#include "engine/memory/temporal_allocator.hpp"

#include <cstdint>

namespace {

usz offset_of(const void* pointer) {
    return static_cast<usz>(static_cast<const char*>(pointer) - MAIN_ARENA.data);
}

} // namespace

TEST_CASE("memory/temporal_allocator: MAIN_ARENA is a non-owning arena over the thread buffer") {
    CHECK(MAIN_ARENA.data == MAIN_ARENA_BUFFER);
    CHECK(MAIN_ARENA.arena_size == MAIN_ARENA_BYTES);
    CHECK(MAIN_ARENA_BYTES == 4 * MEMORY::MB);
    CHECK_FALSE(MAIN_ARENA.owns_data);
    CHECK_ARENA_CLEAN();
}

TEST_CASE("memory/temporal_allocator: create records the current arena mark") {
    {
        TemporalAllocator temp = TemporalAllocator::create();
        CHECK(temp.arena_mark == 0);
        CHECK(temp.arena_mark != TemporalAllocator::moved_from);
        CHECK_ARENA_CLEAN();
    }
    CHECK_ARENA_CLEAN();

    SUBCASE("a mark taken later records the bumped offset") {
        TemporalAllocator outer = TemporalAllocator::create();
        outer.allocate(100, 1);
        CHECK(MAIN_ARENA.offset == 100);
        {
            TemporalAllocator inner = TemporalAllocator::create();
            CHECK(inner.arena_mark == 100);
        }
        CHECK(MAIN_ARENA.offset == 100);
    }
    CHECK_ARENA_CLEAN();
}

TEST_CASE("memory/temporal_allocator: allocations bump MAIN_ARENA and are rewound at scope exit") {
    {
        TemporalAllocator temp = TemporalAllocator::create();

        char* a = static_cast<char*>(temp.allocate(100, 1));
        char* b = static_cast<char*>(temp.allocate(100, 1));
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        CHECK(a == MAIN_ARENA.data);
        CHECK(b == a + 100);
        CHECK(MAIN_ARENA.offset == 200);

        memset(a, 1, 100);
        memset(b, 2, 100);
        CHECK(a[99] == 1);
        CHECK(b[0] == 2);

        SUBCASE("a zero-sized request returns nullptr") {
            CHECK(temp.allocate(0, 8) == nullptr);
            CHECK(MAIN_ARENA.offset == 200);
        }

        SUBCASE("free is a no-op") {
            temp.free(a);
            temp.free(nullptr);
            CHECK(MAIN_ARENA.offset == 200);
            CHECK(temp.allocate(1, 1) == b + 100);
        }

        SUBCASE("reallocate is unsupported and leaves the arena untouched") {
            CHECK(temp.reallocate(a, 200, 1) == nullptr);
            CHECK(temp.reallocate(nullptr, 10, 1) == nullptr);
            CHECK(temp.reallocate(a, 0, 1) == nullptr);
            CHECK(MAIN_ARENA.offset == 200);
            CHECK(a[0] == 1);
        }
    }
    CHECK_ARENA_CLEAN();
}

TEST_CASE("memory/temporal_allocator: allocations honour alignment") {
    {
        TemporalAllocator temp = TemporalAllocator::create();
        for (usz alignment = 1; alignment <= 64; alignment *= 2) {
            CAPTURE(alignment);
            temp.allocate(1, 1); // knock the offset off alignment
            void* block = temp.allocate(8, alignment);
            REQUIRE(block != nullptr);
            CHECK(reinterpret_cast<uintptr_t>(block) % alignment == 0);
            CHECK(MAIN_ARENA.offset == offset_of(block) + 8);
        }

        u64* words = temp.allocate_array<u64>(3);
        REQUIRE(words != nullptr);
        CHECK(reinterpret_cast<uintptr_t>(words) % alignof(u64) == 0);
    }
    CHECK_ARENA_CLEAN();
}

TEST_CASE("memory/temporal_allocator: nested scopes rewind to their own marks") {
    {
        TemporalAllocator outer = TemporalAllocator::create();
        char* kept = static_cast<char*>(outer.allocate(64, 1));
        memset(kept, 7, 64);
        CHECK(MAIN_ARENA.offset == 64);

        {
            TemporalAllocator inner = TemporalAllocator::create();
            CHECK(inner.arena_mark == 64);
            inner.allocate(1000, 1);
            inner.allocate(1000, 1);
            CHECK(MAIN_ARENA.offset == 2064);

            {
                TemporalAllocator innermost = TemporalAllocator::create();
                innermost.allocate(10, 1);
                CHECK(MAIN_ARENA.offset == 2074);
            }
            CHECK(MAIN_ARENA.offset == 2064);
        }
        // Only the inner allocations are gone; the outer block survives.
        CHECK(MAIN_ARENA.offset == 64);
        CHECK(kept[0] == 7);
        CHECK(kept[63] == 7);

        // The space the inner scope used is handed out again.
        CHECK(outer.allocate(1, 1) == kept + 64);
    }
    CHECK_ARENA_CLEAN();
}

TEST_CASE("memory/temporal_allocator: sequential scopes reuse the same memory") {
    void* first = nullptr;
    {
        TemporalAllocator temp = TemporalAllocator::create();
        first = temp.allocate(256, 16);
        CHECK(first != nullptr);
    }
    CHECK_ARENA_CLEAN();
    {
        TemporalAllocator temp = TemporalAllocator::create();
        void* second = temp.allocate(256, 16);
        CHECK(second == first);
    }
    CHECK_ARENA_CLEAN();
}

TEST_CASE("memory/temporal_allocator: a moved-from allocator no longer rewinds the arena") {
    {
        TemporalAllocator guard = TemporalAllocator::create(); // rewinds to 0 at the end

        SUBCASE("move construction hands the mark to the new allocator") {
            TemporalAllocator* survivor = nullptr;
            {
                TemporalAllocator source = TemporalAllocator::create();
                source.allocate(100, 1);
                CHECK(source.arena_mark == 0);

                TemporalAllocator target(std::move(source));
                CHECK(target.arena_mark == 0);
                CHECK(source.arena_mark == TemporalAllocator::moved_from);
                survivor = &target;

                // Allocating through the target continues the same scope.
                target.allocate(50, 1);
                CHECK(MAIN_ARENA.offset == 150);
            }
            // Both died: the target rewound to the shared mark exactly once.
            (void)survivor;
            CHECK(MAIN_ARENA.offset == 0);
        }

        SUBCASE("the moved-from source dying does not rewind") {
            TemporalAllocator target = TemporalAllocator::create();
            target.allocate(10, 1);
            {
                TemporalAllocator source = TemporalAllocator::create();
                CHECK(source.arena_mark == 10);
                source.allocate(20, 1);
                CHECK(MAIN_ARENA.offset == 30);

                target = std::move(source);
                CHECK(target.arena_mark == 10);
                CHECK(source.arena_mark == TemporalAllocator::moved_from);
            }
            // source went out of scope but was moved-from: nothing rewound.
            CHECK(MAIN_ARENA.offset == 30);
        }

        SUBCASE("self move-assignment is a no-op") {
            TemporalAllocator temp = TemporalAllocator::create();
            temp.allocate(10, 1);
            TemporalAllocator& alias = temp;
            temp = std::move(alias);
            CHECK(temp.arena_mark == 0);
            CHECK(MAIN_ARENA.offset == 10);
        }
    }
    CHECK_ARENA_CLEAN();
}

TEST_CASE("memory/temporal_allocator: a moved-from allocator reused starts a new scope") {
    {
        TemporalAllocator outer = TemporalAllocator::create();
        outer.allocate(40, 1);
        CHECK(MAIN_ARENA.offset == 40);

        TemporalAllocator source = TemporalAllocator::create();
        {
            TemporalAllocator sink(std::move(source));
            CHECK(source.arena_mark == TemporalAllocator::moved_from);
        }
        CHECK(MAIN_ARENA.offset == 40);

        {
            // The first allocation after a move takes a fresh mark at the
            // current offset, so this scope only owns what it allocates now.
            outer.allocate(10, 1);
            CHECK(MAIN_ARENA.offset == 50);
            TemporalAllocator reused(std::move(source));
            CHECK(reused.arena_mark == TemporalAllocator::moved_from);
            reused.allocate(100, 1);
            CHECK(reused.arena_mark == 50);
            CHECK(MAIN_ARENA.offset == 150);
        }
        CHECK(MAIN_ARENA.offset == 50);
    }
    CHECK_ARENA_CLEAN();
}

TEST_CASE("memory/temporal_allocator: an explicit mark rewinds to that offset") {
    {
        TemporalAllocator outer = TemporalAllocator::create();
        outer.allocate(100, 1);
        outer.allocate(100, 1);
        CHECK(MAIN_ARENA.offset == 200);
        {
            TemporalAllocator partial(100);
            CHECK(partial.arena_mark == 100);
            partial.allocate(300, 1);
            CHECK(MAIN_ARENA.offset == 500);
        }
        // Rewound to the explicit mark, dropping the second outer block too.
        CHECK(MAIN_ARENA.offset == 100);
    }
    CHECK_ARENA_CLEAN();
}

TEST_CASE("memory/temporal_allocator: containers backed by a temporal allocator need no free") {
    {
        TemporalAllocator temp = TemporalAllocator::create();
        DynamicArray<int> scratch(&temp);
        for (int i = 0; i < 1000; ++i) {
            scratch.push(i);
        }
        CHECK(scratch.count == 1000);
        CHECK(scratch[999] == 999);
        // The array's buffer lives inside MAIN_ARENA.
        CHECK(reinterpret_cast<char*>(scratch.data) >= MAIN_ARENA.data);
        CHECK(reinterpret_cast<char*>(scratch.data) < MAIN_ARENA.data + MAIN_ARENA_BYTES);
        CHECK(MAIN_ARENA.offset >= 1000 * sizeof(int));
    }
    CHECK_ARENA_CLEAN();
}

TEST_CASE("memory/temporal_allocator: exhausting MAIN_ARENA returns nullptr and rewinds cleanly") {
    {
        TemporalAllocator temp = TemporalAllocator::create();
        void* big = temp.allocate(MAIN_ARENA_BYTES, 1);
        CHECK(big != nullptr);
        CHECK(MAIN_ARENA.offset == MAIN_ARENA_BYTES);
        CHECK(temp.allocate(1, 1) == nullptr);
        CHECK(MAIN_ARENA.offset == MAIN_ARENA_BYTES);
    }
    CHECK_ARENA_CLEAN();
    {
        TemporalAllocator temp = TemporalAllocator::create();
        CHECK(temp.allocate(MAIN_ARENA_BYTES + 1, 1) == nullptr);
        CHECK_ARENA_CLEAN();
    }
    CHECK_ARENA_CLEAN();
}
