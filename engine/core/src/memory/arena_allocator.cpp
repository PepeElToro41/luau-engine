#include "engine/memory/arena_allocator.hpp"

#include "engine/memory/heap_allocator.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>

ArenaAllocator::ArenaAllocator(const usz arena_size)
    : arena_size(arena_size), owns_data(true) {}

ArenaAllocator::ArenaAllocator(void* buffer, const usz buffer_size)
    : data(static_cast<char*>(buffer)), arena_size(buffer_size), owns_data(false) {}

ArenaAllocator::ArenaAllocator(ArenaAllocator&& other) noexcept
    : data(other.data), arena_size(other.arena_size), offset(other.offset), owns_data(other.owns_data) {
    other.data = nullptr;
    other.offset = 0;
    other.owns_data = false;
}

ArenaAllocator& ArenaAllocator::operator=(ArenaAllocator&& other) noexcept {
    if (this != &other) {
        this->release();
        this->data = other.data;
        this->arena_size = other.arena_size;
        this->offset = other.offset;
        this->owns_data = other.owns_data;
        other.data = nullptr;
        other.offset = 0;
        other.owns_data = false;
    }
    return *this;
}

void* ArenaAllocator::allocate(const size_t size, const size_t alignment) {
    if (size == 0) {
        return nullptr;
    }
    if (this->data == nullptr) {
        // Owned arenas allocate lazily so an unused arena costs nothing.
        this->data = static_cast<char*>(
            MEMORY::heap_allocator()->allocate(this->arena_size, alignof(std::max_align_t)));
        this->owns_data = true;
    }

    // Align the absolute address, not the offset: the buffer itself is only
    // guaranteed max_align_t alignment when owned, and nothing at all when
    // borrowed (MAIN_ARENA_BUFFER is a plain char array).
    const uintptr_t base = reinterpret_cast<uintptr_t>(this->data);
    const uintptr_t aligned = (base + this->offset + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
    const usz aligned_offset = aligned - base;
    if (aligned_offset + size > this->arena_size) {
        fprintf(stderr, "[memory] arena out of memory: %llu bytes requested, %llu of %llu used\n",
                static_cast<unsigned long long>(size), static_cast<unsigned long long>(this->offset),
                static_cast<unsigned long long>(this->arena_size));
        return nullptr;
    }

    this->offset = aligned_offset + size;
    return this->data + aligned_offset;
}

void ArenaAllocator::free(void* ptr) {
    (void)ptr;
}

void* ArenaAllocator::reallocate(void* ptr, const size_t new_size, const size_t alignment) {
    (void)ptr;
    (void)alignment;
    // An arena cannot resize a block and does not know how large it was, so
    // there is nothing sensible to copy. Callers must allocate + copy themselves.
    fprintf(stderr, "[memory] error: reallocate is not supported on an arena (%llu bytes requested)\n",
            static_cast<unsigned long long>(new_size));
    return nullptr;
}

void ArenaAllocator::reset() {
    this->offset = 0;
}

void ArenaAllocator::reset_to(const usz new_offset) {
    this->offset = new_offset;
}

void ArenaAllocator::release() {
    if (this->owns_data && this->data != nullptr) {
        MEMORY::heap_allocator()->free(this->data);
        this->data = nullptr;
    }
    this->offset = 0;
}
