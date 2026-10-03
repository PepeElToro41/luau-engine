#include "engine/ecs/entity_index.hpp"

#include <cstring>
#include <utility>

using namespace ENTITY_INDEX;

// --- Lifetime ----------------------------------------------------------------

EntityIndex::EntityIndex()
    : allocator(MEMORY::heap_allocator()), pages(this->allocator), dense_list(this->allocator) {}

EntityIndex::EntityIndex(BaseAllocator* allocator)
    : allocator(allocator), pages(allocator), dense_list(allocator) {}

void EntityIndex::free() {
    for (EntityRecord* page : this->pages) {
        if (page != nullptr) {
            this->allocator->free(page);
        }
    }
    this->pages.free();
    this->dense_list.free();
    this->alive_count = 1;
    this->last_id = 0;
    this->range_min = 0;
    this->range_max = 0;
}

void EntityIndex::clear() {
    for (usz i = 1; i < this->alive_count; ++i) {
        const EntityId entity = this->dense_list[i];
        EntityRecord* record = this->get_record_any(entity);
        record->archetype = nullptr;
        record->archetype_row = 0;
        record->flags = 0;
        this->dense_list[i] = increment_generation(entity);
    }
    this->alive_count = 1;
}

// --- Internal helpers --------------------------------------------------------

void EntityIndex::ensure_sentinel() {
    if (this->dense_list.count == 0) {
        this->dense_list.push(0);
    }
}

EntityRecord* EntityIndex::ensure_page(const usz page_index) {
    // Pages in between stay nullptr, so a range starting at a high id only
    // costs a pointer per skipped page rather than a full page of records.
    while (page_index >= this->pages.count) {
        this->pages.push(nullptr);
    }

    EntityRecord* page = this->pages[page_index];
    if (page == nullptr) {
        page = this->allocator->allocate_array<EntityRecord>(PAGE_SIZE);
        // dense == 0 marks a slot that was never registered.
        std::memset(static_cast<void*>(page), 0, PAGE_SIZE * sizeof(EntityRecord));
        this->pages[page_index] = page;
    }
    return page;
}

bool EntityIndex::in_range(const EntityIdLow id) const {
    if (id < this->range_min) {
        return false;
    }
    return this->range_max == 0 || id <= this->range_max;
}

void EntityIndex::dense_swap_recycle(EntityRecord* record, const EntityId entity) {
    const usz dense = record->dense;
    this->alive_count -= 1;
    const usz swap_index = this->alive_count;

    const EntityId swap_entity = this->dense_list[swap_index];
    EntityRecord* swap_record = this->get_record_any(swap_entity);
    swap_record->dense = dense;
    record->dense = swap_index;

    this->dense_list[dense] = swap_entity;
    this->dense_list[swap_index] = increment_generation(entity);
}

void EntityIndex::dense_swap_delete(EntityRecord* record, const EntityId entity) {
    // First swap: move the entity to the end of the alive span.
    this->dense_swap_recycle(record, entity);

    // Second swap: move the tail of the dead pool into its place and pop.
    const usz index = record->dense;
    const usz last = this->dense_list.count - 1;
    if (index != last) {
        const EntityId last_entity = this->dense_list[last];
        this->get_record_any(last_entity)->dense = index;
        this->dense_list[index] = last_entity;
    }
    this->dense_list.pop();
    record->dense = 0;
}

// --- Entities ----------------------------------------------------------------

EntityId EntityIndex::new_entity(EntityRecord** out_record) {
    this->ensure_sentinel();

    if (this->alive_count < this->dense_list.count) {
        // Recycling: the dead entry already carries its bumped generation.
        const EntityId entity = this->dense_list[this->alive_count];
        this->alive_count += 1;
        if (out_record != nullptr) {
            *out_record = this->get_record_any(entity);
        }
        return entity;
    }

    const EntityIdLow id = this->last_id + 1;
    if (id >= ECS::ENTITY_SIZE || (this->range_max != 0 && id > this->range_max)) {
        if (out_record != nullptr) {
            *out_record = nullptr;
        }
        return 0;
    }
    this->last_id = id;

    this->dense_list.push(id);
    EntityRecord* record = this->ensure_page(get_page_index(id)) + get_page_offset(id);
    record->dense = this->alive_count;
    record->archetype = nullptr;
    record->archetype_row = 0;
    record->flags = 0;
    this->alive_count += 1;

    if (out_record != nullptr) {
        *out_record = record;
    }
    return id;
}

EntityRecord* EntityIndex::make_alive(const EntityId entity) {
    this->ensure_sentinel();

    const EntityIdLow id = ECS::ENTITY_LOW(entity);
    EntityRecord* record = this->ensure_page(get_page_index(id)) + get_page_offset(id);
    usz dense = record->dense;

    if (dense != 0) {
        if (dense < this->alive_count) {
            // Already alive: adopt the generation the caller asked for.
            this->dense_list[dense] = entity;
            return record;
        }
    } else {
        // Never registered: append to the dead pool, then revive below.
        this->dense_list.push(entity);
        dense = this->dense_list.count - 1;
        record->dense = dense;
        if (id > this->last_id) {
            this->last_id = id;
        }
    }

    // Dead: swap with the first dead slot and grow the alive span over it.
    const usz swap_index = this->alive_count;
    const EntityId swap_entity = this->dense_list[swap_index];
    EntityRecord* swap_record = this->get_record_any(swap_entity);
    swap_record->dense = dense;
    record->dense = swap_index;

    this->dense_list[dense] = swap_entity;
    this->dense_list[swap_index] = entity;
    this->alive_count += 1;

    record->archetype = nullptr;
    record->archetype_row = 0;
    record->flags = 0;
    return record;
}

EntityRecord* EntityIndex::get_record_any(const EntityId entity) const {
    const usz page_index = get_page_index(entity);
    if (page_index >= this->pages.count) {
        return nullptr;
    }
    EntityRecord* page = this->pages[page_index];
    if (page == nullptr) {
        return nullptr;
    }
    return page + get_page_offset(entity);
}

EntityRecord* EntityIndex::get_record_alive(const EntityId entity) const {
    EntityRecord* record = this->get_record_any(entity);
    if (record == nullptr || record->dense == 0 || record->dense >= this->alive_count) {
        return nullptr;
    }
    if (this->dense_list[record->dense] != entity) {
        return nullptr;
    }
    return record;
}

bool EntityIndex::is_alive(const EntityId entity) const {
    return this->get_record_alive(entity) != nullptr;
}

EntityId EntityIndex::get_current(const EntityId entity) const {
    EntityRecord* record = this->get_record_any(entity);
    if (record == nullptr || record->dense == 0 || record->dense >= this->alive_count) {
        return 0;
    }
    return this->dense_list[record->dense];
}

usz EntityIndex::count() const {
    return this->alive_count - 1;
}

EntityId EntityIndex::get_alive_id(const usz index) const {
    return this->dense_list[index + 1];
}

bool EntityIndex::is_empty() const {
    return this->alive_count == 1;
}

bool EntityIndex::delete_entity(const EntityId entity) {
    EntityRecord* record = this->get_record_alive(entity);
    if (record == nullptr) {
        return false;
    }

    record->archetype = nullptr;
    record->archetype_row = 0;
    record->flags = 0;
    if (this->in_range(ECS::ENTITY_LOW(entity))) {
        this->dense_swap_recycle(record, entity);
    } else {
        this->dense_swap_delete(record, entity);
    }
    return true;
}

void EntityIndex::set_range(EntityIdLow min, const EntityIdLow max) {
    if (min == 0) {
        min = this->last_id + 1;
    }
    if (this->last_id < min) {
        this->last_id = min - 1;
    }
    this->range_min = min;
    this->range_max = max;
}
