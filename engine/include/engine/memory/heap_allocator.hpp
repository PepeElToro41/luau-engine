#pragma once

#include "engine/memory/base_allocator.hpp"

// General-purpose allocator backed by the C runtime heap (malloc / realloc /
// free). Every block is prefixed with a small header holding its size and the
// distance to the raw malloc pointer, so any alignment can be honored and
// free() / reallocate() need nothing from the caller beyond the pointer.
struct HeapAllocator : BaseAllocator {
    void* allocate(size_t size, size_t alignment) override;
    void free(void* ptr) override;
    void* reallocate(void* ptr, size_t new_size, size_t alignment) override;
};

namespace MEMORY {

// Process-wide HeapAllocator. Containers constructed without an explicit
// allocator use this one.
BaseAllocator* heap_allocator();

} // namespace MEMORY
