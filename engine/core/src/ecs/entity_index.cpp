#include "engine/ecs/entity_index.hpp"

#include "engine/utils/log.hpp"

#include <cstring>
#include <new>
#include <utility>

using namespace ENTITY_INDEX;

// --- Lifetime ----------------------------------------------------------------

EntityIndex::EntityIndex()
    : allocator(MEMORY::heap_allocator()), pages(this->allocator), dense_list(this->allocator), ranges(this->allocator) {}

EntityIndex::EntityIndex(BaseAllocator* allocator)
    : allocator(allocator), pages(allocator), dense_list(allocator), ranges(allocator) {}

void EntityIndex::free() {
    for (EntityRecord* page : this->pages) {
        if (page != nullptr) {
            this->allocator->free(page);
        }
    }
    this->pages.free();
    this->dense_list.free();
    for (EntityRange* range : this->ranges) {
        range->recycled.free();
        this->allocator->free(range);
    }
    this->ranges.free();
    this->active = nullptr;
    this->alive_count = 1;
    this->last_id = 0;
}

void EntityIndex::clear() {
    for (usz i = 1; i < this->alive_count; ++i) {
        const EntityId entity = this->dense_list[i];
        EntityRecord* record = this->get_record_any(entity);
        record->dense = 0;
        record->archetype = nullptr;
        record->archetype_row = 0;
        record->flags = 0;
        if (EntityRange* range = this->range_find(ECS::ENTITY_LOW(entity))) {
            range->recycled.push(increment_generation(entity));
        }
    }
    if (this->dense_list.count > 1) {
        this->dense_list.count = 1;
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
        // dense == 0 marks a slot that is not alive.
        std::memset(static_cast<void*>(page), 0, PAGE_SIZE * sizeof(EntityRecord));
        this->pages[page_index] = page;
    }
    return page;
}

EntityRecord* EntityIndex::register_alive(const EntityId entity) {
    EntityRecord* record = this->ensure_page(get_page_index(entity)) + get_page_offset(entity);
    record->dense = this->alive_count;
    record->archetype = nullptr;
    record->archetype_row = 0;
    record->flags = 0;
    this->dense_list.push(entity);
    this->alive_count += 1;
    const EntityIdLow id = ECS::ENTITY_LOW(entity);
    if (id > this->last_id) {
        this->last_id = id;
    }
    return record;
}

void EntityIndex::dense_remove(EntityRecord* record) {
    const usz dense = record->dense;
    const usz last = this->alive_count - 1;
    if (dense != last) {
        const EntityId last_entity = this->dense_list[last];
        this->get_record_any(last_entity)->dense = dense;
        this->dense_list[dense] = last_entity;
    }
    this->dense_list.pop();
    this->alive_count -= 1;
    record->dense = 0;
}

bool EntityIndex::take_from_pool(EntityRange* range, const EntityIdLow id) {
    for (usz i = 0; i < range->recycled.count; ++i) {
        if (ECS::ENTITY_LOW(range->recycled[i]) == id) {
            range->recycled.remove_swap(i);
            return true;
        }
    }
    return false;
}

EntityRange* EntityIndex::ensure_default_range() {
    if (this->active != nullptr) {
        return this->active;
    }
    if (this->ranges.count != 0) {
        LOG::error(LOG_CATEGORY::ECS, "entity index: ranges exist but none is active; call range_set before new_entity");
        return nullptr;
    }
    EntityRange* range = this->range_new(1, 0);
    this->active = range;
    return range;
}

void EntityIndex::migrate_pool(EntityRange* range) {
    for (usz i = 0; i < range->recycled.count;) {
        const EntityId dead = range->recycled[i];
        const EntityIdLow id = ECS::ENTITY_LOW(dead);
        if (range->contains(id)) {
            i++;
            continue;
        }
        range->recycled.remove_swap(i);
        if (EntityRange* home = this->range_find(id)) {
            home->recycled.push(dead);
        }
        // Outside every range: forgotten, like an id deleted there.
    }
}

// --- Entities ----------------------------------------------------------------

EntityId EntityIndex::new_entity(EntityRecord** out_record) {
    this->ensure_sentinel();
    EntityRange* range = this->active != nullptr ? this->active : this->ensure_default_range();
    if (range == nullptr) {
        if (out_record != nullptr) {
            *out_record = nullptr;
        }
        return 0;
    }

    EntityId entity;
    if (range->recycled.count > 0) {
        // The dead entry already carries its bumped generation.
        entity = range->recycled.last();
        range->recycled.pop();
    } else {
        const EntityIdLow id = range->cur + 1;
        if (id >= ECS::ENTITY_SIZE || (range->max != 0 && id > range->max)) {
            if (out_record != nullptr) {
                *out_record = nullptr;
            }
            return 0;
        }
        range->cur = id;
        entity = id;
    }

    EntityRecord* record = this->register_alive(entity);
    if (out_record != nullptr) {
        *out_record = record;
    }
    return entity;
}

EntityRecord* EntityIndex::make_alive(const EntityId entity) {
    this->ensure_sentinel();

    const EntityIdLow id = ECS::ENTITY_LOW(entity);
    EntityRecord* record = this->ensure_page(get_page_index(id)) + get_page_offset(id);
    if (record->dense != 0) {
        // Already alive: adopt the generation the caller asked for.
        this->dense_list[record->dense] = entity;
        return record;
    }

    // Dead or never seen. Its range, if any, must neither hold it in its
    // pool nor issue it fresh later.
    if (EntityRange* range = this->range_find(id)) {
        this->take_from_pool(range, id);
        if (id > range->cur) {
            range->cur = id;
        }
    }
    return this->register_alive(entity);
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

EntityRecord* EntityIndex::resolve_low(const EntityIdLow id, EntityId* out_entity) const {
    EntityRecord* record = this->get_record_any(id);
    if (record == nullptr || record->dense == 0 || record->dense >= this->alive_count) {
        *out_entity = 0;
        return nullptr;
    }
    *out_entity = this->dense_list[record->dense];
    return record;
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
    this->dense_remove(record);
    if (EntityRange* range = this->range_find(ECS::ENTITY_LOW(entity))) {
        range->recycled.push(increment_generation(entity));
    }
    return true;
}

// --- Ranges ------------------------------------------------------------------

EntityRange* EntityIndex::range_find(const EntityIdLow id) const {
    // Sorted by min: the last range starting at or before id is the only
    // candidate.
    usz low = 0;
    usz high = this->ranges.count;
    while (low < high) {
        const usz middle = (low + high) / 2;
        if (this->ranges[middle]->min <= id) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low == 0) {
        return nullptr;
    }
    EntityRange* range = this->ranges[low - 1];
    return range->contains(id) ? range : nullptr;
}

EntityRange* EntityIndex::range_new(const EntityIdLow min, const EntityIdLow max) {
    if (min == 0 || (max != 0 && max < min)) {
        LOG::error(LOG_CATEGORY::ECS, "entity index: range [%llu, %llu] is empty or starts at 0", static_cast<unsigned long long>(min),
            static_cast<unsigned long long>(max));
        return nullptr;
    }
    // Where it sorts, and whether its neighbors leave room for it.
    usz position = 0;
    while (position < this->ranges.count && this->ranges[position]->min < min) {
        position++;
    }
    const EntityRange* before = position > 0 ? this->ranges[position - 1] : nullptr;
    const EntityRange* after = position < this->ranges.count ? this->ranges[position] : nullptr;
    const bool overlaps_before = before != nullptr && (before->max == 0 || before->max >= min);
    const bool overlaps_after = after != nullptr && (max == 0 || max >= after->min);
    if (overlaps_before || overlaps_after) {
        const EntityRange* other = overlaps_before ? before : after;
        LOG::error(LOG_CATEGORY::ECS, "entity index: range [%llu, %llu] overlaps the existing range [%llu, %llu]", static_cast<unsigned long long>(min),
            static_cast<unsigned long long>(max), static_cast<unsigned long long>(other->min), static_cast<unsigned long long>(other->max));
        return nullptr;
    }

    EntityRange* range = this->allocator->allocate_array<EntityRange>(1);
    new (range) EntityRange(this->allocator);
    range->min = min;
    range->max = max;
    // Every id ever issued or registered is at or below last_id, alive, dead
    // in some pool or dropped: the cursor starts past all of them, so a
    // fresh id never revives a stale handle. A range created entirely below
    // last_id has no fresh ids to give, only what dies inside it later.
    range->cur = min - 1;
    if (this->last_id >= min) {
        range->cur = max != 0 && this->last_id > max ? max : this->last_id;
    }
    this->ranges.push(range);
    for (usz i = this->ranges.count - 1; i > position; --i) {
        this->ranges[i] = this->ranges[i - 1];
    }
    this->ranges[position] = range;
    return range;
}

bool EntityIndex::range_set(EntityRange* range) {
    for (EntityRange* owned : this->ranges) {
        if (owned == range) {
            this->active = range;
            return true;
        }
    }
    return false;
}

void EntityIndex::set_range(EntityIdLow min, EntityIdLow max) {
    if (min == 0) {
        min = this->last_id + 1;
    }
    if (max != 0 && max < min) {
        max = min;
    }

    // The range starting here, if any: its max moves.
    usz position = 0;
    while (position < this->ranges.count && this->ranges[position]->min < min) {
        position++;
    }
    EntityRange* after = position < this->ranges.count ? this->ranges[position] : nullptr;
    if (after != nullptr && after->min == min) {
        EntityRange* next = position + 1 < this->ranges.count ? this->ranges[position + 1] : nullptr;
        if (next != nullptr && (max == 0 || max >= next->min)) {
            max = next->min - 1;
        }
        after->max = max;
        this->migrate_pool(after);
        this->active = after;
        return;
    }

    // A new range: the one it starts inside ends before it, and it ends
    // before the next one.
    EntityRange* holder = position > 0 ? this->ranges[position - 1] : nullptr;
    if (holder != nullptr && (holder->max == 0 || holder->max >= min)) {
        holder->max = min - 1;
    }
    if (after != nullptr && (max == 0 || max >= after->min)) {
        max = after->min - 1;
    }
    EntityRange* range = this->range_new(min, max);
    if (range == nullptr) {
        return;
    }
    if (holder != nullptr) {
        // Dead ids the trim pushed out of the holder now belong here.
        this->migrate_pool(holder);
    }
    this->active = range;
}
