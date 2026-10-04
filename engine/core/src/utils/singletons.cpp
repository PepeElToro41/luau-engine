#include "engine/utils/singletons.hpp"

#include "engine/memory/heap_allocator.hpp"

Singletons::Singletons(BaseAllocator* allocator) :
    allocator(allocator),
    entries(allocator)
{}

Singletons::Singletons() : Singletons(MEMORY::heap_allocator()) {}

void Singletons::free() {
    for (auto& entry : this->entries) {
        entry.value.destroy(entry.value.value);
        this->allocator->free(entry.value.value);
    }
    this->entries.free();
}
