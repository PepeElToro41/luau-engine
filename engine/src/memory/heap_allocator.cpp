#include "engine/memory/heap_allocator.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

// Sits right before the pointer handed to the caller. Layout of a block:
//
//     raw ──┐  (slack for alignment)   ┌── user pointer (aligned)
//           v                          v
//           [ ..... ][ BlockHeader ][ size bytes ....... ]
//
// `offset` is user - raw, so free() can get back to the malloc block, and
// `size` / `alignment` let reallocate() copy and re-align without the caller
// passing them back.
struct BlockHeader {
    size_t offset;
    size_t size;
    size_t alignment;
};

size_t clamp_alignment(const size_t alignment) {
    // The header itself has to be aligned, and alignment 0 means "don't care".
    return alignment < alignof(BlockHeader) ? alignof(BlockHeader) : alignment;
}

// Bytes malloc has to give us so the header and `size` bytes fit at any offset.
size_t raw_size(const size_t size, const size_t alignment) {
    return sizeof(BlockHeader) + (alignment - 1) + size;
}

// Where the user pointer lands inside `raw` for `alignment`.
char* aligned_user_pointer(void* raw, const size_t alignment) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(raw) + sizeof(BlockHeader);
    const uintptr_t aligned = (base + alignment - 1) & ~static_cast<uintptr_t>(alignment - 1);
    return reinterpret_cast<char*>(aligned);
}

BlockHeader* header_of(void* user) {
    return static_cast<BlockHeader*>(user) - 1;
}

void write_header(void* raw, char* user, const size_t size, const size_t alignment) {
    BlockHeader* header = header_of(user);
    header->offset = static_cast<size_t>(user - static_cast<char*>(raw));
    header->size = size;
    header->alignment = alignment;
}

} // namespace

void* HeapAllocator::allocate(const size_t size, size_t alignment) {
    if (size == 0) {
        return nullptr;
    }
    alignment = clamp_alignment(alignment);

    void* raw = std::malloc(raw_size(size, alignment));
    if (raw == nullptr) {
        return nullptr;
    }
    char* user = aligned_user_pointer(raw, alignment);
    write_header(raw, user, size, alignment);
    return user;
}

void HeapAllocator::free(void* ptr) {
    if (ptr == nullptr) {
        return;
    }
    const BlockHeader* header = header_of(ptr);
    std::free(static_cast<char*>(ptr) - header->offset);
}

void* HeapAllocator::reallocate(void* ptr, const size_t new_size, size_t alignment) {
    if (ptr == nullptr) {
        return this->allocate(new_size, alignment);
    }
    if (new_size == 0) {
        this->free(ptr);
        return nullptr;
    }

    const BlockHeader old_header = *header_of(ptr);
    // Growing to the larger of the two alignments keeps the old contents in
    // bounds even if a caller changes alignment between calls.
    alignment = clamp_alignment(alignment);
    if (alignment < old_header.alignment) {
        alignment = old_header.alignment;
    }

    void* raw = static_cast<char*>(ptr) - old_header.offset;
    char* new_raw = static_cast<char*>(std::realloc(raw, raw_size(new_size, alignment)));
    if (new_raw == nullptr) {
        return nullptr;
    }

    // realloc may have moved the block, so the aligned slot can sit at a
    // different offset than before. Slide the contents over before the new
    // header is written on top of wherever the old bytes were.
    char* user = aligned_user_pointer(new_raw, alignment);
    const size_t new_offset = static_cast<size_t>(user - new_raw);
    if (new_offset != old_header.offset) {
        const size_t preserved = old_header.size < new_size ? old_header.size : new_size;
        std::memmove(user, new_raw + old_header.offset, preserved);
    }
    write_header(new_raw, user, new_size, alignment);
    return user;
}

namespace MEMORY {

BaseAllocator* heap_allocator() {
    static HeapAllocator instance;
    return &instance;
}

} // namespace MEMORY
