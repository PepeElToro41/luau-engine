#include "support/test_support.hpp"

#include "engine/memory/arena_allocator.hpp"

#include <cstdint>

namespace {

bool is_aligned(const void* pointer, const usz alignment) {
    return (reinterpret_cast<uintptr_t>(pointer) & (alignment - 1)) == 0;
}

bool inside(const void* pointer, const char* buffer, const usz size) {
    const char* p = static_cast<const char*>(pointer);
    return p >= buffer && p < buffer + size;
}

} // namespace

TEST_CASE("memory/arena_allocator: an owning arena allocates its buffer lazily") {
    ArenaAllocator arena(MEMORY::KB);
    CHECK(arena.data == nullptr);
    CHECK(arena.arena_size == MEMORY::KB);
    CHECK(arena.offset == 0);
    CHECK(arena.owns_data);

    // Releasing an arena that never allocated is a no-op.
    arena.release();
    CHECK(arena.data == nullptr);

    void* block = arena.allocate(16, 8);
    REQUIRE(block != nullptr);
    CHECK(arena.data != nullptr);
    CHECK(arena.owns_data);
    CHECK(block == arena.data);
    CHECK(arena.offset == 16);
    CHECK(is_aligned(arena.data, alignof(std::max_align_t)));

    SUBCASE("release frees the buffer and the arena can be used again") {
        arena.release();
        CHECK(arena.data == nullptr);
        CHECK(arena.offset == 0);
        CHECK(arena.arena_size == MEMORY::KB);

        void* again = arena.allocate(32, 8);
        REQUIRE(again != nullptr);
        CHECK(arena.data != nullptr);
        CHECK(again == arena.data);
        CHECK(arena.offset == 32);
    }

    arena.release();
}

TEST_CASE("memory/arena_allocator: a non-owning arena never frees the caller's buffer") {
    alignas(64) char buffer[256];
    ArenaAllocator arena(buffer, sizeof(buffer));
    CHECK(arena.data == buffer);
    CHECK(arena.arena_size == sizeof(buffer));
    CHECK(arena.offset == 0);
    CHECK_FALSE(arena.owns_data);

    void* block = arena.allocate(10, 1);
    CHECK(block == buffer);
    CHECK(arena.offset == 10);

    // release() only rewinds: the buffer is kept and still usable.
    arena.release();
    CHECK(arena.data == buffer);
    CHECK(arena.offset == 0);
    CHECK_FALSE(arena.owns_data);
    CHECK(arena.allocate(10, 1) == buffer);
}

TEST_CASE("memory/arena_allocator: allocations bump the offset and are distinct, writable blocks") {
    alignas(64) char buffer[1024];
    ArenaAllocator arena(buffer, sizeof(buffer));

    char* a = static_cast<char*>(arena.allocate(100, 1));
    char* b = static_cast<char*>(arena.allocate(100, 1));
    char* c = static_cast<char*>(arena.allocate(100, 1));
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    CHECK(b == a + 100);
    CHECK(c == b + 100);
    CHECK(arena.offset == 300);

    memset(a, 1, 100);
    memset(b, 2, 100);
    memset(c, 3, 100);
    CHECK(a[99] == 1);
    CHECK(b[0] == 2);
    CHECK(b[99] == 2);
    CHECK(c[0] == 3);

    SUBCASE("a zero-sized request returns nullptr without consuming space") {
        CHECK(arena.allocate(0, 8) == nullptr);
        CHECK(arena.offset == 300);
    }

    SUBCASE("free is a no-op") {
        arena.free(b);
        arena.free(nullptr);
        CHECK(arena.offset == 300);
        CHECK(b[0] == 2);
        // The next allocation comes after c, not in b's place.
        CHECK(arena.allocate(1, 1) == c + 100);
    }

    SUBCASE("typed allocate_array sizes and aligns for the element type") {
        arena.allocate(1, 1); // misalign the offset
        u64* words = arena.allocate_array<u64>(4);
        REQUIRE(words != nullptr);
        CHECK(is_aligned(words, alignof(u64)));
        CHECK(arena.offset == 304 + 4 * sizeof(u64));
        CHECK(arena.allocate_array<u64>(0) == nullptr);
    }
}

TEST_CASE("memory/arena_allocator: allocations honour power-of-two alignments") {
    alignas(64) char buffer[4096];
    ArenaAllocator arena(buffer, sizeof(buffer));

    for (usz alignment = 1; alignment <= 64; alignment *= 2) {
        CAPTURE(alignment);
        // Push the offset to an odd value so the alignment has to do work.
        arena.allocate(1, 1);
        const usz before = arena.offset;
        void* block = arena.allocate(8, alignment);
        REQUIRE(block != nullptr);
        CHECK(is_aligned(block, alignment));
        CHECK(static_cast<char*>(block) - buffer >= static_cast<long>(before));
        CHECK(static_cast<char*>(block) - buffer < static_cast<long>(before + alignment));
        CHECK(arena.offset == static_cast<usz>(static_cast<char*>(block) - buffer) + 8);
    }

    SUBCASE("an already aligned offset is not padded") {
        arena.reset();
        void* first = arena.allocate(64, 64);
        void* second = arena.allocate(64, 64);
        CHECK(first == buffer);
        CHECK(second == buffer + 64);
        CHECK(arena.offset == 128);
    }
}

TEST_CASE("memory/arena_allocator: an owning arena honours alignment up to max_align_t") {
    ArenaAllocator arena(MEMORY::KB);
    for (usz alignment = 1; alignment <= alignof(std::max_align_t); alignment *= 2) {
        CAPTURE(alignment);
        arena.allocate(1, 1);
        void* block = arena.allocate(8, alignment);
        REQUIRE(block != nullptr);
        CHECK(is_aligned(block, alignment));
    }
    arena.release();
}

TEST_CASE("memory/arena_allocator: an owning arena honours over-aligned requests") {
    // The owned buffer only comes back max_align_t (16) aligned from the heap,
    // so allocate() must align the address, not the offset.
    ArenaAllocator arena(MEMORY::MB);
    for (usz alignment = 32; alignment <= 256; alignment *= 2) {
        CAPTURE(alignment);
        void* block = arena.allocate(8, alignment);
        REQUIRE(block != nullptr);
        CHECK(is_aligned(block, alignment));
    }
    arena.release();
}

TEST_CASE("memory/arena_allocator: a full arena returns nullptr and keeps its state") {
    alignas(64) char buffer[64];
    ArenaAllocator arena(buffer, sizeof(buffer));

    SUBCASE("a request that exactly fills the arena succeeds") {
        void* block = arena.allocate(64, 1);
        CHECK(block == buffer);
        CHECK(arena.offset == 64);
        CHECK(arena.allocate(1, 1) == nullptr);
        CHECK(arena.offset == 64);
    }

    SUBCASE("a request larger than the arena fails up front") {
        CHECK(arena.allocate(65, 1) == nullptr);
        CHECK(arena.offset == 0);
        // The arena is still usable afterwards.
        CHECK(arena.allocate(64, 1) == buffer);
    }

    SUBCASE("alignment padding counts toward exhaustion") {
        arena.allocate(1, 1);
        // Aligning to 32 lands on offset 32, so 33 bytes no longer fit.
        CHECK(arena.allocate(33, 32) == nullptr);
        CHECK(arena.offset == 1);
        CHECK(arena.allocate(32, 32) == buffer + 32);
        CHECK(arena.offset == 64);
    }

    SUBCASE("reset makes the whole buffer available again") {
        CHECK(arena.allocate(64, 1) != nullptr);
        CHECK(arena.allocate(1, 1) == nullptr);
        arena.reset();
        CHECK(arena.offset == 0);
        CHECK(arena.allocate(64, 1) == buffer);
    }
}

TEST_CASE("memory/arena_allocator: reallocate is unsupported and leaves the old block alone") {
    alignas(64) char buffer[256];
    ArenaAllocator arena(buffer, sizeof(buffer));

    char* block = static_cast<char*>(arena.allocate(16, 8));
    REQUIRE(block != nullptr);
    memset(block, 0xAB, 16);
    const usz offset = arena.offset;

    CHECK(arena.reallocate(block, 32, 8) == nullptr);
    CHECK(arena.reallocate(nullptr, 32, 8) == nullptr);
    CHECK(arena.reallocate(block, 0, 8) == nullptr);
    CHECK(arena.offset == offset);
    for (int i = 0; i < 16; ++i) {
        CHECK(static_cast<u8>(block[i]) == 0xAB);
    }
    CHECK(arena.reallocate_array<int>(nullptr, 4) == nullptr);
}

TEST_CASE("memory/arena_allocator: reset_to rewinds the offset to an earlier mark") {
    alignas(64) char buffer[512];
    ArenaAllocator arena(buffer, sizeof(buffer));

    void* kept = arena.allocate(100, 1);
    const usz mark = arena.offset;
    CHECK(mark == 100);

    void* scratch_a = arena.allocate(50, 1);
    void* scratch_b = arena.allocate(50, 1);
    CHECK(arena.offset == 200);
    CHECK(scratch_a == buffer + 100);
    CHECK(scratch_b == buffer + 150);

    arena.reset_to(mark);
    CHECK(arena.offset == mark);

    // Memory after the mark is handed out again; memory before it is kept.
    void* reused = arena.allocate(50, 1);
    CHECK(reused == scratch_a);
    CHECK(kept == buffer);
    CHECK(arena.offset == 150);

    SUBCASE("reset_to the current offset changes nothing") {
        arena.reset_to(arena.offset);
        CHECK(arena.offset == 150);
    }

    SUBCASE("reset is reset_to(0)") {
        arena.reset();
        CHECK(arena.offset == 0);
        CHECK(arena.data == buffer);
        CHECK(arena.allocate(1, 1) == buffer);
    }
}

TEST_CASE("memory/arena_allocator: move constructor and assignment transfer the buffer") {
    SUBCASE("moving a non-owning arena") {
        alignas(64) char buffer[128];
        ArenaAllocator source(buffer, sizeof(buffer));
        source.allocate(40, 1);

        ArenaAllocator target(std::move(source));
        CHECK(target.data == buffer);
        CHECK(target.arena_size == sizeof(buffer));
        CHECK(target.offset == 40);
        CHECK_FALSE(target.owns_data);
        CHECK(target.allocate(8, 8) == buffer + 40);

        CHECK(source.data == nullptr);
        CHECK(source.offset == 0);
        CHECK_FALSE(source.owns_data);
    }

    SUBCASE("moving an owning arena transfers ownership") {
        ArenaAllocator source(MEMORY::KB);
        void* block = source.allocate(24, 8);
        char* data = source.data;
        REQUIRE(data != nullptr);

        ArenaAllocator target(std::move(source));
        CHECK(target.data == data);
        CHECK(target.owns_data);
        CHECK(target.offset == 24);
        CHECK(target.arena_size == MEMORY::KB);
        CHECK(inside(block, target.data, target.arena_size));

        CHECK(source.data == nullptr);
        CHECK(source.offset == 0);
        CHECK_FALSE(source.owns_data);

        target.release();
        CHECK(target.data == nullptr);
    }

    SUBCASE("move assignment releases the target's previous buffer") {
        alignas(64) char buffer[128];
        ArenaAllocator source(buffer, sizeof(buffer));
        source.allocate(16, 1);

        ArenaAllocator target(MEMORY::KB);
        target.allocate(8, 8);
        REQUIRE(target.data != nullptr);
        CHECK(target.owns_data);

        target = std::move(source);
        CHECK(target.data == buffer);
        CHECK(target.arena_size == sizeof(buffer));
        CHECK(target.offset == 16);
        CHECK_FALSE(target.owns_data);
        CHECK(target.allocate(1, 1) == buffer + 16);

        CHECK(source.data == nullptr);
        CHECK(source.offset == 0);
        CHECK_FALSE(source.owns_data);

        target.release();
    }
}

TEST_CASE("memory/arena_allocator: memory size constants are powers of 1024") {
    CHECK(MEMORY::KB == 1024);
    CHECK(MEMORY::MB == 1024 * MEMORY::KB);
    CHECK(MEMORY::GB == 1024 * MEMORY::MB);
}
