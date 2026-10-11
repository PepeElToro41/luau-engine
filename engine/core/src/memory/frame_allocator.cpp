#include "engine/memory/frame_allocator.hpp"

#include <cstdio>

FrameAllocator::FrameAllocator(const usz bytes)
    : arena(bytes) {}

void FrameAllocator::reset() {
    if (this->arena.offset > this->high_water) {
        this->high_water = this->arena.offset;
    }
    this->arena.reset();
    this->exhausted = false;
}

void FrameAllocator::release() {
    this->arena.release();
    this->arena.offset = 0;
    this->high_water = 0;
    this->exhausted = false;
}

void* FrameAllocator::allocate(const size_t size, const size_t alignment) {
    if (size == 0) {
        return nullptr;
    }
    const usz before = this->arena.offset;
    void* block = this->arena.allocate(size, alignment);
    if (block == nullptr && !this->exhausted) {
        // The arena already printed the sizes; say whose arena it was, once.
        fprintf(stderr, "[memory] frame allocator exhausted at %llu of %llu bytes; the rest of this frame's scratch allocations fail\n",
            static_cast<unsigned long long>(before), static_cast<unsigned long long>(this->arena.arena_size));
        this->exhausted = true;
    }
    return block;
}

void FrameAllocator::free(void* ptr) {
    (void)ptr;
}

void* FrameAllocator::reallocate(void* ptr, const size_t new_size, const size_t alignment) {
    return this->arena.reallocate(ptr, new_size, alignment);
}
