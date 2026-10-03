#pragma once

#include "engine/defines.hpp"
#include "engine/memory/base_allocator.hpp"

namespace MEMORY {

constexpr usz KB = 1024;
constexpr usz MB = KB * 1024;
constexpr usz GB = MB * 1024;

} // namespace MEMORY

// Linear (bump) allocator over one contiguous buffer. Allocation is a pointer
// bump; individual frees are no-ops and memory is reclaimed all at once with
// reset() / reset_to(). Arenas do not support reallocate(): callers that grow
// buffers must allocate a new block and copy themselves.
struct ArenaAllocator : BaseAllocator {
    // Arena that allocates its own buffer of `arena_size` bytes from the heap
    // on first use. Call release() to give the buffer back.
    explicit ArenaAllocator(usz arena_size);

    // Arena over a caller-provided buffer. The arena never frees it.
    ArenaAllocator(void* buffer, usz buffer_size);

    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;
    ArenaAllocator(ArenaAllocator&& other) noexcept;
    ArenaAllocator& operator=(ArenaAllocator&& other) noexcept;

    // Returns nullptr when the arena is full.
    void* allocate(size_t size, size_t alignment) override;
    // No-op: arena memory is only reclaimed by reset() / reset_to().
    void free(void* ptr) override;
    // Not supported: logs an error and returns nullptr. The old block is
    // left untouched.
    void* reallocate(void* ptr, size_t new_size, size_t alignment) override;

    // Forgets every allocation. The buffer is kept.
    void reset();
    // Rewinds to an offset previously read from `offset`, forgetting every
    // allocation made after it.
    void reset_to(usz new_offset);

    // Frees the buffer if this arena owns it. The arena can be used again and
    // will allocate a fresh buffer on the next allocate().
    void release();

    char* data = nullptr;
    usz arena_size = 0;
    usz offset = 0;
    bool owns_data = false;
};
