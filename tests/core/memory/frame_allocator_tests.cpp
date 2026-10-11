#include "support/test_support.hpp"

#include "engine/memory/frame_allocator.hpp"

#include <cstdint>

TEST_CASE("memory/frame_allocator: allocations live until the next reset") {
    FrameAllocator frame(MEMORY::KB);
    CHECK(frame.used() == 0);
    CHECK(frame.capacity() == MEMORY::KB);

    int* a = frame.allocate_array<int>(4);
    REQUIRE(a != nullptr);
    a[0] = 7;
    char* b = static_cast<char*>(frame.allocate(100, 1));
    REQUIRE(b != nullptr);
    CHECK(frame.used() >= 4 * sizeof(int) + 100);
    frame.free(a); // no-op
    CHECK(a[0] == 7);

    frame.reset();
    CHECK(frame.used() == 0);
    CHECK(frame.high_water >= 4 * sizeof(int) + 100);
    // The next frame hands out the same memory again.
    CHECK(frame.allocate(4, 4) == static_cast<void*>(a));

    frame.release();
    CHECK(frame.arena.data == nullptr);
    CHECK(frame.high_water == 0);
}

TEST_CASE("memory/frame_allocator: a full frame returns nullptr and reports once") {
    FrameAllocator frame(64);
    CHECK(frame.allocate(48, 8) != nullptr);
    CHECK(frame.allocate(48, 8) == nullptr);
    CHECK(frame.exhausted);
    CHECK(frame.allocate(48, 8) == nullptr);
    frame.reset();
    CHECK_FALSE(frame.exhausted);
    CHECK(frame.allocate(48, 8) != nullptr);
    frame.release();
}

TEST_CASE("memory/frame_allocator: a DynamicArray grows on it across scopes") {
    FrameAllocator frame(MEMORY::KB);
    DynamicArray<u32> items(&frame);
    {
        // Growing inside a nested TemporalAllocator scope does not touch
        // MAIN_ARENA, so the array survives the scope's rewind.
        TemporalAllocator temp = TemporalAllocator::create();
        DynamicArray<u32> scratch(&temp);
        scratch.push(1);
        for (u32 i = 0; i < 20; ++i) {
            items.push(i);
        }
    }
    REQUIRE(items.count == 20);
    for (u32 i = 0; i < 20; ++i) {
        CHECK(items[i] == i);
    }
    frame.reset();
    frame.release();
    CHECK_ARENA_CLEAN();
}
