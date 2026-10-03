#pragma once

#include <cstddef>

// Interface every allocator implements. Alignment is always a power of two.
//
// Contract (mirrors libc malloc / realloc / free, with alignment added):
//   - allocate(0, ...) may return nullptr.
//   - free(nullptr) is a no-op.
//   - reallocate(nullptr, n, a) behaves like allocate(n, a).
//   - reallocate(p, 0, a) behaves like free(p) and returns nullptr.
//   - reallocate() keeps the block aligned to `a`, which should be the
//     alignment the block was allocated with; the contents up to the smaller
//     of the old and new size are preserved. On failure it returns nullptr
//     and leaves the old block untouched.
//   - Callers never pass the size or alignment back to free(): an allocator
//     that needs them records them itself.
struct BaseAllocator {
    virtual void* allocate(size_t size, size_t alignment) = 0;
    virtual void free(void* ptr) = 0;
    virtual void* reallocate(void* ptr, size_t new_size, size_t alignment) = 0;

    // Typed wrappers over the virtual calls: `count` elements of T, sized and
    // aligned for T. The memory is raw, no constructors run. Same contract as
    // the untyped versions (count 0 may yield nullptr, reallocate from nullptr
    // allocates, to 0 frees).
    template <typename T>
    T* allocate_array(const size_t count) {
        return static_cast<T*>(this->allocate(count * sizeof(T), alignof(T)));
    }

    template <typename T>
    T* reallocate_array(T* ptr, const size_t count) {
        return static_cast<T*>(this->reallocate(ptr, count * sizeof(T), alignof(T)));
    }
};
