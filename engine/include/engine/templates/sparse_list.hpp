#pragma once

#include "engine/defines.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"

#include <cstring>
#include <utility>

// A SparseId packs a 32-bit slot index (low) and a 32-bit generation (high).
// The generation is bumped every time a slot is deleted, so an id held by a
// caller stops matching once its slot has been recycled.
using SparseId = u64;
using SparseIdLow = u64;
using SparseGeneration = u64;

namespace SPARSE_LIST {

constexpr u64 ELEMENT_BITS = 32;
constexpr u64 ELEMENT_SIZE = 1ull << ELEMENT_BITS;
constexpr u64 ELEMENT_MASK = ELEMENT_SIZE - 1;

constexpr u64 GENERATION_BITS = 32;
constexpr u64 GENERATION_SIZE = 1ull << GENERATION_BITS;

constexpr usz PAGE_SIZE_BITS = 6;
constexpr usz PAGE_SIZE = 1ull << PAGE_SIZE_BITS; // 64 elements per page
constexpr usz PAGE_MASK = PAGE_SIZE - 1;

inline SparseIdLow element_low(const SparseId id) {
    return id & ELEMENT_MASK;
}
inline SparseGeneration element_generation(const SparseId id) {
    return id >> ELEMENT_BITS;
}
inline SparseId append_generation(const SparseIdLow id, const SparseGeneration generation) {
    return id | (generation << ELEMENT_BITS);
}
// Page lookups work on the low bits, so a full id (with generation) is fine.
inline usz get_page_index(const SparseId id) {
    return element_low(id) >> PAGE_SIZE_BITS;
}
inline usz get_page_offset(const SparseId id) {
    return id & PAGE_MASK;
}

// Next generation for a slot. Generation 0 is what a freshly created slot
// carries, so on wrap-around we skip to 1: a recycled id never equals a fresh one.
inline SparseId increment_generation(const SparseId element) {
    const SparseIdLow id = element_low(element);
    const SparseGeneration new_generation = element_generation(element) + 1;

    if (new_generation < GENERATION_SIZE) {
        return append_generation(id, new_generation);
    }
    return append_generation(id, 1);
}

} // namespace SPARSE_LIST

// One fixed-size page of element storage plus, per slot, the index of that
// slot's id inside SparseList::dense_list.
template <typename T>
struct SparsePage {
    T* data = nullptr;
    usz* dense = nullptr;

    void allocate(BaseAllocator* allocator) {
        this->data = allocator->allocate_array<T>(SPARSE_LIST::PAGE_SIZE);
        this->dense = allocator->allocate_array<usz>(SPARSE_LIST::PAGE_SIZE);
        // Unused slots must not point at a random dense index, or is_alive()
        // could read garbage.
        std::memset(this->dense, 0, SPARSE_LIST::PAGE_SIZE * sizeof(usz));
    }

    void free(BaseAllocator* allocator) {
        allocator->free(this->data);
        allocator->free(this->dense);
        this->data = nullptr;
        this->dense = nullptr;
    }
};

// Paged sparse set with slot recycling, in the style of the flecs sparse
// allocator / entity index (minus id ranges).
//
// Elements live in fixed pages that are never moved, so a T* stays valid for
// as long as its slot is alive. `dense_list` holds every id ever handed out:
// indices [0, alive_count) are alive, the rest are dead slots waiting to be
// revived, already carrying their next generation.
//
// T is treated as plain data: new slots are zeroed, and no constructors or
// destructors are run. Storage is lazy and there is no destructor; call free().
template <typename T>
struct SparseList {
    SparseList()
        : allocator(MEMORY::heap_allocator()), sparse_pages(this->allocator), dense_list(this->allocator) {}
    explicit SparseList(BaseAllocator* allocator)
        : allocator(allocator), sparse_pages(allocator), dense_list(allocator) {}

    SparseList(const SparseList&) = delete;
    SparseList& operator=(const SparseList&) = delete;

    SparseList(SparseList&& other) noexcept
        : allocator(other.allocator), sparse_pages(std::move(other.sparse_pages)),
          dense_list(std::move(other.dense_list)), alive_count(other.alive_count),
          next_element(other.next_element) {
        other.alive_count = 0;
        other.next_element = 0;
    }

    SparseList& operator=(SparseList&& other) noexcept {
        if (this != &other) {
            this->free();
            this->allocator = other.allocator;
            this->sparse_pages = std::move(other.sparse_pages);
            this->dense_list = std::move(other.dense_list);
            this->alive_count = other.alive_count;
            this->next_element = other.next_element;
            other.alive_count = 0;
            other.next_element = 0;
        }
        return *this;
    }

    // --- Elements -----------------------------------------------------------

    // Hands out a slot, reviving a deleted one when available. The element's
    // memory is zeroed either way.
    SparseId new_element() {
        const usz dense = this->alive_count;
        SparseId id;

        if (dense < this->dense_list.count) {
            // Reviving: the dead entry already carries its bumped generation.
            id = this->dense_list[dense];
        } else {
            id = this->next_element;
            this->next_element += 1;
            this->dense_list.push(id);
        }
        this->alive_count += 1;

        const usz page_offset = SPARSE_LIST::get_page_offset(id);
        SparsePage<T>* page = this->ensure_page(SPARSE_LIST::get_page_index(id));
        page->dense[page_offset] = dense;
        // T is plain data here (see the struct comment); cast to void* so the
        // compiler does not flag zeroing types with non-trivial members.
        std::memset(static_cast<void*>(page->data + page_offset), 0, sizeof(T));
        return id;
    }

    // Element for `id`, or nullptr if the id is not alive (never issued,
    // deleted, or a stale generation). Mirrors flecs' entity index "alive" lookup.
    T* get_element_alive(const SparseId id) const {
        if (!this->is_alive(id)) {
            return nullptr;
        }
        return this->get_element_any(id);
    }

    // Element slot for `id` ignoring generation and liveness, so it also
    // reaches deleted slots. Only nullptr if the slot's page was never
    // allocated. Mirrors flecs' entity index "any" lookup.
    T* get_element_any(const SparseId id) const {
        const usz page_index = SPARSE_LIST::get_page_index(id);
        if (page_index >= this->sparse_pages.count) {
            return nullptr;
        }
        const SparsePage<T>& page = this->sparse_pages[page_index];
        return page.data + SPARSE_LIST::get_page_offset(id);
    }

    bool is_alive(const SparseId id) const {
        const usz page_index = SPARSE_LIST::get_page_index(id);
        if (page_index >= this->sparse_pages.count) {
            return false;
        }
        const SparsePage<T>& page = this->sparse_pages[page_index];
        const usz dense = page.dense[SPARSE_LIST::get_page_offset(id)];
        return dense < this->alive_count && this->dense_list[dense] == id;
    }

    // Id of the alive element at `dense_index`, valid for [0, alive_count).
    // Deleting an element moves the last alive id into its dense index.
    SparseId get_alive_id(const usz dense_index) const {
        return this->dense_list[dense_index];
    }

    // Kills `id`, bumping its generation and queueing the slot for reuse.
    // Returns false if the id was not alive.
    bool delete_element(const SparseId id) {
        if (!this->is_alive(id)) {
            return false;
        }

        SparsePage<T>& page = this->sparse_pages[SPARSE_LIST::get_page_index(id)];
        const usz page_offset = SPARSE_LIST::get_page_offset(id);
        const usz dense_index = page.dense[page_offset];

        this->alive_count -= 1;
        const usz last_index = this->alive_count;
        const SparseId recycled = SPARSE_LIST::increment_generation(id);

        if (dense_index != last_index) {
            // Swap the last alive id into the hole so [0, alive_count) stays dense.
            const SparseId last_id = this->dense_list[last_index];
            this->dense_list[dense_index] = last_id;
            this->dense_list[last_index] = recycled;

            SparsePage<T>& last_page = this->sparse_pages[SPARSE_LIST::get_page_index(last_id)];
            last_page.dense[SPARSE_LIST::get_page_offset(last_id)] = dense_index;
            page.dense[page_offset] = last_index;
        } else {
            this->dense_list[dense_index] = recycled;
        }
        return true;
    }

    // Kills every element. Slots and pages are kept for reuse.
    void clear() {
        for (usz i = 0; i < this->alive_count; ++i) {
            this->dense_list[i] = SPARSE_LIST::increment_generation(this->dense_list[i]);
        }
        this->alive_count = 0;
    }

    // Releases every page and the id list, returning to the fresh state.
    void free() {
        for (SparsePage<T>& page : this->sparse_pages) {
            page.free(this->allocator);
        }
        this->sparse_pages.free();
        this->dense_list.free();
        this->alive_count = 0;
        this->next_element = 0;
    }

    bool is_empty() const { return this->alive_count == 0; }

    // --- Members ------------------------------------------------------------

    BaseAllocator* allocator = nullptr;
    DynamicArray<SparsePage<T>> sparse_pages;
    DynamicArray<SparseId> dense_list;

    usz alive_count = 0;
    SparseIdLow next_element = 0;

private:
    SparsePage<T>* ensure_page(const usz page_index) {
        while (page_index >= this->sparse_pages.count) {
            SparsePage<T> page;
            page.allocate(this->allocator);
            this->sparse_pages.push(page);
        }
        return &this->sparse_pages[page_index];
    }
};
