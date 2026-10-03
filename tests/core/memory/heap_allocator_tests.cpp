#include "support/test_support.hpp"

#include "engine/memory/heap_allocator.hpp"

#include <cstdint>

namespace {

bool is_aligned(const void* pointer, const usz alignment) {
    return (reinterpret_cast<uintptr_t>(pointer) & (alignment - 1)) == 0;
}

void fill_pattern(char* block, const usz size, const u8 seed) {
    for (usz i = 0; i < size; ++i) {
        block[i] = static_cast<char>(static_cast<u8>(i * 31 + seed));
    }
}

bool check_pattern(const char* block, const usz size, const u8 seed) {
    for (usz i = 0; i < size; ++i) {
        if (static_cast<u8>(block[i]) != static_cast<u8>(i * 31 + seed)) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("memory/heap_allocator: the process-wide allocator is a single instance") {
    BaseAllocator* first = MEMORY::heap_allocator();
    BaseAllocator* second = MEMORY::heap_allocator();
    REQUIRE(first != nullptr);
    CHECK(first == second);

    void* block = first->allocate(32, 8);
    REQUIRE(block != nullptr);
    second->free(block);
}

TEST_CASE("memory/heap_allocator: allocate returns distinct writable blocks") {
    HeapAllocator heap;

    char* a = static_cast<char*>(heap.allocate(64, 8));
    char* b = static_cast<char*>(heap.allocate(64, 8));
    char* c = static_cast<char*>(heap.allocate(1, 1));
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    CHECK(a != b);
    CHECK(b != c);
    // Blocks never overlap.
    CHECK((a + 64 <= b || b + 64 <= a));

    fill_pattern(a, 64, 1);
    fill_pattern(b, 64, 2);
    c[0] = 'x';
    CHECK(check_pattern(a, 64, 1));
    CHECK(check_pattern(b, 64, 2));
    CHECK(c[0] == 'x');

    SUBCASE("freeing one block leaves the others intact") {
        heap.free(b);
        CHECK(check_pattern(a, 64, 1));
        CHECK(c[0] == 'x');
        b = nullptr;
    }

    heap.free(a);
    heap.free(b);
    heap.free(c);
}

TEST_CASE("memory/heap_allocator: zero-sized requests and null frees follow the malloc contract") {
    HeapAllocator heap;
    CHECK(heap.allocate(0, 8) == nullptr);
    CHECK(heap.allocate(0, 1) == nullptr);
    CHECK(heap.allocate_array<int>(0) == nullptr);
    heap.free(nullptr); // must not crash
    CHECK(heap.reallocate(nullptr, 0, 8) == nullptr);
    CHECK(heap.reallocate_array<int>(nullptr, 0) == nullptr);
}

TEST_CASE("memory/heap_allocator: allocations honour alignments up to 256") {
    HeapAllocator heap;
    void* blocks[9];
    usz index = 0;

    for (usz alignment = 1; alignment <= 256; alignment *= 2) {
        CAPTURE(alignment);
        void* block = heap.allocate(100, alignment);
        REQUIRE(block != nullptr);
        CHECK(is_aligned(block, alignment));
        // The whole block is usable.
        memset(block, 0xCD, 100);
        blocks[index] = block;
        index += 1;
    }

    for (usz i = 0; i < index; ++i) {
        CHECK(static_cast<u8*>(blocks[i])[99] == 0xCD);
        heap.free(blocks[i]);
    }

    SUBCASE("alignment 0 and small alignments still give a usable, header-aligned block") {
        void* loose = heap.allocate(16, 0);
        REQUIRE(loose != nullptr);
        CHECK(is_aligned(loose, alignof(size_t)));
        memset(loose, 0, 16);
        heap.free(loose);
    }

    SUBCASE("many over-aligned blocks at once all stay aligned") {
        void* many[64];
        for (void*& block : many) {
            block = heap.allocate(7, 64);
            REQUIRE(block != nullptr);
            CHECK(is_aligned(block, 64));
        }
        for (void* block : many) {
            heap.free(block);
        }
    }
}

TEST_CASE("memory/heap_allocator: typed array helpers size and align for the element type") {
    HeapAllocator heap;

    u64* words = heap.allocate_array<u64>(10);
    REQUIRE(words != nullptr);
    CHECK(is_aligned(words, alignof(u64)));
    for (u64 i = 0; i < 10; ++i) {
        words[i] = i * i;
    }

    words = heap.reallocate_array<u64>(words, 1000);
    REQUIRE(words != nullptr);
    CHECK(is_aligned(words, alignof(u64)));
    for (u64 i = 0; i < 10; ++i) {
        CHECK(words[i] == i * i);
    }
    words[999] = 42;

    CHECK(heap.reallocate_array<u64>(words, 0) == nullptr);
}

TEST_CASE("memory/heap_allocator: reallocate from nullptr allocates and to zero frees") {
    HeapAllocator heap;

    void* block = heap.reallocate(nullptr, 48, 16);
    REQUIRE(block != nullptr);
    CHECK(is_aligned(block, 16));
    memset(block, 0x11, 48);

    CHECK(heap.reallocate(block, 0, 16) == nullptr);
    // The block was freed by the call above: nothing left to free.
}

TEST_CASE("memory/heap_allocator: reallocate preserves contents when growing and shrinking") {
    HeapAllocator heap;
    char* block = static_cast<char*>(heap.allocate(100, 8));
    REQUIRE(block != nullptr);
    fill_pattern(block, 100, 5);

    SUBCASE("growing keeps every old byte") {
        block = static_cast<char*>(heap.reallocate(block, 10000, 8));
        REQUIRE(block != nullptr);
        CHECK(is_aligned(block, 8));
        CHECK(check_pattern(block, 100, 5));
        // The new tail is writable.
        memset(block + 100, 0, 9900);
        CHECK(check_pattern(block, 100, 5));
    }

    SUBCASE("shrinking keeps the prefix") {
        block = static_cast<char*>(heap.reallocate(block, 20, 8));
        REQUIRE(block != nullptr);
        CHECK(check_pattern(block, 20, 5));
    }

    SUBCASE("repeated growth across many sizes never loses data") {
        usz size = 100;
        for (int step = 0; step < 12; ++step) {
            const usz new_size = size * 2 + 13;
            block = static_cast<char*>(heap.reallocate(block, new_size, 8));
            REQUIRE(block != nullptr);
            CHECK(check_pattern(block, size, 5));
            fill_pattern(block, new_size, 5);
            size = new_size;
        }
        CHECK(check_pattern(block, size, 5));
    }

    heap.free(block);
}

TEST_CASE("memory/heap_allocator: reallocate keeps over-aligned blocks aligned") {
    HeapAllocator heap;

    for (usz alignment = 16; alignment <= 256; alignment *= 2) {
        CAPTURE(alignment);
        char* block = static_cast<char*>(heap.allocate(200, alignment));
        REQUIRE(block != nullptr);
        CHECK(is_aligned(block, alignment));
        fill_pattern(block, 200, static_cast<u8>(alignment));

        // Grow several times through sizes that force the block to move.
        usz size = 200;
        for (int step = 0; step < 8; ++step) {
            const usz new_size = size * 3;
            block = static_cast<char*>(heap.reallocate(block, new_size, alignment));
            REQUIRE(block != nullptr);
            CHECK(is_aligned(block, alignment));
            CHECK(check_pattern(block, size, static_cast<u8>(alignment)));
            fill_pattern(block, new_size, static_cast<u8>(alignment));
            size = new_size;
        }

        // Shrinking keeps the alignment too.
        block = static_cast<char*>(heap.reallocate(block, 50, alignment));
        REQUIRE(block != nullptr);
        CHECK(is_aligned(block, alignment));
        CHECK(check_pattern(block, 50, static_cast<u8>(alignment)));

        heap.free(block);
    }
}

TEST_CASE("memory/heap_allocator: reallocate with a larger alignment re-aligns and keeps contents") {
    HeapAllocator heap;
    char* block = static_cast<char*>(heap.allocate(64, 8));
    REQUIRE(block != nullptr);
    fill_pattern(block, 64, 9);

    block = static_cast<char*>(heap.reallocate(block, 128, 128));
    REQUIRE(block != nullptr);
    CHECK(is_aligned(block, 128));
    CHECK(check_pattern(block, 64, 9));

    // Asking for a smaller alignment afterwards never drops below the
    // alignment the block already has.
    block = static_cast<char*>(heap.reallocate(block, 256, 8));
    REQUIRE(block != nullptr);
    CHECK(is_aligned(block, 128));
    CHECK(check_pattern(block, 64, 9));

    heap.free(block);
}

TEST_CASE("memory/heap_allocator: many interleaved allocations and frees do not corrupt each other") {
    HeapAllocator heap;
    constexpr usz block_count = 200;
    char* blocks[block_count] = {};
    usz sizes[block_count] = {};

    for (usz i = 0; i < block_count; ++i) {
        sizes[i] = (i * 37) % 500 + 1;
        const usz alignment = 1ull << (i % 7); // 1 .. 64
        blocks[i] = static_cast<char*>(heap.allocate(sizes[i], alignment));
        REQUIRE(blocks[i] != nullptr);
        CHECK(is_aligned(blocks[i], alignment));
        fill_pattern(blocks[i], sizes[i], static_cast<u8>(i));
    }

    // Free every other block, then check and reallocate the survivors.
    for (usz i = 0; i < block_count; i += 2) {
        heap.free(blocks[i]);
        blocks[i] = nullptr;
    }
    for (usz i = 1; i < block_count; i += 2) {
        CHECK(check_pattern(blocks[i], sizes[i], static_cast<u8>(i)));
        const usz alignment = 1ull << (i % 7);
        blocks[i] = static_cast<char*>(heap.reallocate(blocks[i], sizes[i] * 2, alignment));
        REQUIRE(blocks[i] != nullptr);
        CHECK(is_aligned(blocks[i], alignment));
        CHECK(check_pattern(blocks[i], sizes[i], static_cast<u8>(i)));
    }
    for (usz i = 1; i < block_count; i += 2) {
        heap.free(blocks[i]);
    }
}
