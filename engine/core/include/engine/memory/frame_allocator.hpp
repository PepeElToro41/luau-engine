#pragma once

#include "engine/defines.hpp"
#include "engine/memory/arena_allocator.hpp"
#include "engine/memory/base_allocator.hpp"

// Scratch memory that lives for one frame. Everything a frame builds and
// throws away (draw lists, sort keys, culling results, ...) is bumped out of
// one arena and forgotten all at once by reset() at the start of the next
// frame; free() is a no-op and nothing is released individually.
//
// It differs from TemporalAllocator in not being scoped: a block allocated
// here stays valid until the next reset() no matter which scope allocated
// it, so a container on the frame allocator may be filled from inside a
// Query::each callback (which holds its own TemporalAllocator on MAIN_ARENA)
// without being rewound away. Prefer it for anything that outlives the scope
// that produced it within the frame.
//
// The Engine owns one as a singleton (get_singleton<FrameAllocator>()),
// resets it in update() and hands it to the Renderer as
// Renderer::frame_allocator.
//
// TODO: the arena is one fixed block. A frame that draws more than it holds
// gets nullptr from allocate() (reported once per frame, see `exhausted`)
// and the caller drops that work. Chain a second block when the first fills,
// or grow the arena for the next frame from `high_water`.
struct FrameAllocator : BaseAllocator {
    // Allocates its buffer of `bytes` lazily, on the first allocate().
    explicit FrameAllocator(usz bytes);

    FrameAllocator(const FrameAllocator&) = delete;
    FrameAllocator& operator=(const FrameAllocator&) = delete;

    // Starts a frame: forgets every allocation of the previous one.
    void reset();
    // Gives the buffer back. reset() / allocate() may be used again after.
    void release();

    // nullptr when the frame's arena is full (an error is logged once per
    // frame).
    void* allocate(size_t size, size_t alignment) override;
    // No-op: memory comes back with reset().
    void free(void* ptr) override;
    // Not supported, same as ArenaAllocator: logs an error and returns
    // nullptr. Containers grow by allocate + copy instead.
    void* reallocate(void* ptr, size_t new_size, size_t alignment) override;

    // Bytes in use by the current frame.
    usz used() const { return this->arena.offset; }
    // The arena's size.
    usz capacity() const { return this->arena.arena_size; }

    ArenaAllocator arena;
    // The most any frame has used since init or release.
    usz high_water = 0;
    // Set when an allocation failed this frame; cleared by reset().
    bool exhausted = false;
};
