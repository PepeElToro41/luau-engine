#pragma once

#include "engine/defines.hpp"
#include "engine/memory/arena_allocator.hpp"

// Per-thread scratch arena backing every TemporalAllocator on that thread.
inline constexpr usz MAIN_ARENA_BYTES = MEMORY::MB * 4;
inline thread_local char MAIN_ARENA_BUFFER[MAIN_ARENA_BYTES] = {};
inline thread_local ArenaAllocator MAIN_ARENA(MAIN_ARENA_BUFFER, MAIN_ARENA_BYTES);

// Scoped scratch allocator. create() records where MAIN_ARENA currently is;
// every allocation made through the allocator bumps the arena, and when the
// allocator goes out of scope the arena is rewound to that mark, releasing all
// of them at once. Nest freely, as long as inner allocators die before outer
// ones (which scoping guarantees).
//
//     TemporalAllocator temp = TemporalAllocator::create();
//     DynamicArray<int> scratch(&temp);
//     ...
//     // scope ends: scratch's memory is reclaimed, no free() needed
//
// This is the one type in the engine with a destructor: rewinding the arena
// on scope exit is the whole point of it.
struct TemporalAllocator : BaseAllocator {
    static constexpr usz moved_from = static_cast<usz>(-1);

    usz arena_mark = moved_from;

    [[nodiscard]] static TemporalAllocator create() {
        return TemporalAllocator(MAIN_ARENA.offset);
    }

    explicit TemporalAllocator(const usz arena_mark) : arena_mark(arena_mark) {}

    TemporalAllocator(const TemporalAllocator&) = delete;
    TemporalAllocator& operator=(const TemporalAllocator&) = delete;

    TemporalAllocator(TemporalAllocator&& other) noexcept : arena_mark(other.arena_mark) {
        other.arena_mark = moved_from;
    }

    TemporalAllocator& operator=(TemporalAllocator&& other) noexcept {
        if (this != &other) {
            this->arena_mark = other.arena_mark;
            other.arena_mark = moved_from;
        }
        return *this;
    }

    ~TemporalAllocator() {
        if (this->arena_mark != moved_from) {
            MAIN_ARENA.reset_to(this->arena_mark);
        }
    }

    void* allocate(const size_t size, const size_t alignment) override {
        if (this->arena_mark == moved_from) {
            // Reused after a move: start a new scope from here.
            this->arena_mark = MAIN_ARENA.offset;
        }
        return MAIN_ARENA.allocate(size, alignment);
    }

    // No-op: everything is released when the allocator goes out of scope.
    void free(void* ptr) override {
        (void)ptr;
    }

    // Not supported, same as ArenaAllocator: logs an error and returns nullptr.
    void* reallocate(void* ptr, const size_t new_size, const size_t alignment) override {
        return MAIN_ARENA.reallocate(ptr, new_size, alignment);
    }
};
