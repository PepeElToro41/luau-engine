#include "support/bench.hpp"

#include "engine/memory/arena_allocator.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/memory/temporal_allocator.hpp"

BENCH_CASE("memory/allocators: allocate 64 bytes") {
    BaseAllocator* heap = MEMORY::heap_allocator();
    bench.run("heap allocate + free", [&] {
        void* p = heap->allocate(64, 16);
        ankerl::nanobench::doNotOptimizeAway(p);
        heap->free(p);
    });

    ArenaAllocator arena(MEMORY::MB);
    bench.run("arena allocate + reset", [&] {
        void* p = arena.allocate(64, 16);
        ankerl::nanobench::doNotOptimizeAway(p);
        arena.reset();
    });
    arena.release();

    bench.run("temporal scope: create + allocate + rewind", [&] {
        TemporalAllocator temp = TemporalAllocator::create();
        void* p = temp.allocate(64, 16);
        ankerl::nanobench::doNotOptimizeAway(p);
    });
}

BENCH_CASE("memory/allocators: 256 allocations of 64 bytes") {
    constexpr usz count = 256;
    BaseAllocator* heap = MEMORY::heap_allocator();
    void* blocks[count];

    bench.batch(count).run("heap", [&] {
        for (usz i = 0; i < count; ++i) {
            blocks[i] = heap->allocate(64, 16);
        }
        for (usz i = 0; i < count; ++i) {
            heap->free(blocks[i]);
        }
    });

    ArenaAllocator arena(MEMORY::MB);
    bench.batch(count).run("arena", [&] {
        for (usz i = 0; i < count; ++i) {
            blocks[i] = arena.allocate(64, 16);
        }
        ankerl::nanobench::doNotOptimizeAway(blocks[count - 1]);
        arena.reset();
    });
    arena.release();
}
