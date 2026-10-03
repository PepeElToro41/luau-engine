#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"

// The id / generation layout of an EntityId lives in ecs.hpp (ECS::ENTITY_BITS,
// ECS::ENTITY_LOW, ...). This namespace only adds the generation arithmetic
// and paging the index needs.
namespace ENTITY_INDEX {

constexpr usz PAGE_SIZE_BITS = 10;
constexpr usz PAGE_SIZE = 1ull << PAGE_SIZE_BITS; // 1024 entities per page
constexpr usz PAGE_MASK = PAGE_SIZE - 1;

constexpr EntityGeneration entity_generation(const EntityId id) {
    return id >> ECS::ENTITY_BITS;
}
constexpr EntityId append_generation(const EntityIdLow id, const EntityGeneration generation) {
    return id | (generation << ECS::ENTITY_BITS);
}

constexpr usz get_page_index(const EntityId id) {
    return ECS::ENTITY_LOW(id) >> PAGE_SIZE_BITS;
}
constexpr usz get_page_offset(const EntityId id) {
    return id & PAGE_MASK;
}

// Next generation for an entity. Generation 0 is what a freshly created id
// carries, so on wrap-around we skip to 1: a recycled id never equals a fresh one.
constexpr EntityId increment_generation(const EntityId entity) {
    const EntityIdLow id = ECS::ENTITY_LOW(entity);
    const EntityGeneration new_generation = entity_generation(entity) + 1;

    if (new_generation < ECS::GENERATION_SIZE) {
        return append_generation(id, new_generation);
    }
    return append_generation(id, 1);
}

} // namespace ENTITY_INDEX

struct Archetype;

enum EntityRecordFlags : u32 {
    // ENTITY::delete_entity is running for this entity further up the stack. A
    // nested destroy of the same entity (a cycle in a cascade) is a no-op.
    ENTITY_RECORD_DELETING = 1 << 0,
};

// Where an entity lives. `dense` is its index in EntityIndex::dense_list
// (0 means the entity was never registered), `archetype` the table that
// stores its components (nullptr until it is inserted into one),
// `archetype_row` its row inside that table and `flags` EntityRecordFlags.
// The index clears archetype, row and flags whenever an id is issued or
// killed.
struct EntityRecord {
    usz dense = 0;
    Archetype* archetype = nullptr;
    usz archetype_row = 0;
    u32 flags = 0;
};

// Paged entity index modeled on the flecs entity index. Records are stored in
// fixed pages that never move, so an EntityRecord* stays valid for as long as
// the page exists. Pages are allocated on first use; `pages` holds nullptr for
// pages nothing has touched, so an id range starting at a high id is cheap.
//
// `dense_list` holds every registered id. Index 0 is a reserved sentinel;
// indices [1, alive_count) are alive; the rest are dead ids waiting to be
// recycled, already carrying their next generation.
//
// An optional id range restricts which ids new_entity() may hand out. Ids
// outside the range (registered through make_alive()) are removed from the
// dense list entirely on delete instead of being recycled.
//
// Storage is lazy and there is no destructor; call free().
struct EntityIndex {
    BaseAllocator* allocator = nullptr;
    DynamicArray<EntityRecord*> pages;
    DynamicArray<EntityId> dense_list;

    usz alive_count = 1;       // includes the reserved sentinel at index 0
    EntityIdLow last_id = 0;   // highest fresh id issued so far
    EntityIdLow range_min = 0;
    EntityIdLow range_max = 0; // 0 = unbounded
    
    EntityIndex();
    explicit EntityIndex(BaseAllocator* allocator);

    // --- Entities -----------------------------------------------------------

    // Hands out an entity, recycling a dead id when one is available and
    // otherwise issuing the next fresh id. If `out_record` is given it receives
    // the entity's record, saving a lookup. Returns 0 (and a null record) if
    // the range is exhausted.
    EntityId new_entity(EntityRecord** out_record = nullptr);

    // Registers `entity` under the exact id given, allocating its record if
    // needed and reviving it if dead. If it is already alive its generation is
    // overwritten with the one in `entity`. Returns the record.
    EntityRecord* make_alive(EntityId entity);

    // Record for `entity`, or nullptr if it is not alive (never registered,
    // deleted, or a stale generation).
    EntityRecord* get_record_alive(EntityId entity) const;

    // Record slot for `entity` ignoring generation and liveness, so it also
    // reaches dead ids. Only nullptr if the slot's page was never allocated.
    EntityRecord* get_record_any(EntityId entity) const;

    bool is_alive(EntityId entity) const;

    // The alive id sharing `entity`'s low bits regardless of the generation
    // passed in, or 0 if that id is not alive.
    EntityId get_current(EntityId entity) const;

    // Number of alive entities, and the id of the alive entity at `index`
    // (valid for [0, count())). Deleting an entity moves the last alive id into
    // its position.
    usz count() const;
    EntityId get_alive_id(usz index) const;
    bool is_empty() const;

    // Kills `entity`, bumping its generation. In-range ids are queued for
    // reuse; out-of-range ids are dropped from the dense list. Returns false
    // if the entity was not alive.
    bool delete_entity(EntityId entity);

    // Restricts the ids new_entity() issues to [min, max]. A max of 0 means
    // unbounded; a min of 0 means "start after the highest id issued so far".
    void set_range(EntityIdLow min, EntityIdLow max);

    // Kills every entity. Records and pages are kept for reuse.
    void clear();

    // Releases every page and the id list, returning to the fresh state.
    void free();

private:
    void ensure_sentinel();
    EntityRecord* ensure_page(usz page_index);
    bool in_range(EntityIdLow id) const;

    // Swaps the entity to the end of the alive span and leaves its bumped id
    // there so new_entity() can recycle it.
    void dense_swap_recycle(EntityRecord* record, EntityId entity);

    // Swaps the entity to the end of the alive span, then swaps it with the
    // tail of dense_list and pops it, so the id is forgotten entirely.
    void dense_swap_delete(EntityRecord* record, EntityId entity);
};
