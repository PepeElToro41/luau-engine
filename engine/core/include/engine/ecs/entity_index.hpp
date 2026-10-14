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
    // Some traversable pair (R, this entity) has a component record, so
    // other entities may hang below it in a hierarchy and a move of this
    // entity may change their depth (see hierarchy.hpp). Set and cleared by
    // HIERARCHY:: as those records come and go.
    ENTITY_RECORD_TRAVERSABLE_TARGET = 1 << 1,
};

// Where an entity lives. `dense` is its index in EntityIndex::dense_list
// (0 means the entity is not alive: never registered, or dead), `archetype`
// the table that stores its components (nullptr until it is inserted into
// one), `archetype_row` its row inside that table and `flags`
// EntityRecordFlags. The index clears archetype, row and flags whenever an
// id is issued or killed.
struct EntityRecord {
    usz dense = 0;
    Archetype* archetype = nullptr;
    usz archetype_row = 0;
    u32 flags = 0;
};

// A band of ids new_entity() may hand out, with its own pool of dead ids.
// Ranges are created once (range_new / set_range) and live until the index
// is freed: a range owns the dead ids that fell inside it, so it can never
// go away while they may still come back. One range is active at a time
// and is the only one new_entity() issues from, fresh or recycled, which
// is what keeps a recycled id from ever escaping the active band. Ranges
// never overlap and are kept sorted by `min`.
struct EntityRange {
    EntityIdLow min = 0; // inclusive
    EntityIdLow max = 0; // inclusive; 0 = unbounded
    // The last id issued from (or registered into) this range; the next
    // fresh one is cur + 1. min - 1 while none was.
    EntityIdLow cur = 0;
    // Dead ids of the range, each already carrying its next generation.
    DynamicArray<EntityId> recycled;

    explicit EntityRange(BaseAllocator* allocator) : recycled(allocator) {}

    bool contains(const EntityIdLow id) const { return id >= this->min && (this->max == 0 || id <= this->max); }
};

// Paged entity index modeled on the flecs entity index. Records are stored in
// fixed pages that never move, so an EntityRecord* stays valid for as long as
// the page exists. Pages are allocated on first use; `pages` holds nullptr for
// pages nothing has touched, so an id range starting at a high id is cheap.
//
// `dense_list` holds every alive id and nothing else. Index 0 is a reserved
// sentinel; indices [1, alive_count) are the alive entities, in no
// particular order (deleting swaps the last one into the hole). Dead ids
// wait in the recycle pool of the range they fall in (see EntityRange); an
// id outside every range (registered through make_alive(): a component id,
// a built-in, an id cloned from another world) is simply forgotten on
// delete, never reissued.
//
// Without an explicit range, the first new_entity() creates the default
// range [1, unbounded). World::init sets [ECS::FIRST_USER_ENTITY_ID,
// unbounded) instead, and anything that needs ids of its own carves a
// range above: set_range(min, max) is the one-call form (make or find the
// range starting at `min`, trim whatever it cuts into, activate it), and
// range_new / range_set the two steps for code that keeps several ranges
// live and switches between them (a server's ids and a client's).
//
// Storage is lazy and there is no destructor; call free().
struct EntityIndex {
    BaseAllocator* allocator = nullptr;
    DynamicArray<EntityRecord*> pages;
    DynamicArray<EntityId> dense_list;

    usz alive_count = 1;     // includes the reserved sentinel at index 0
    EntityIdLow last_id = 0; // highest id ever issued or registered
    // Every range created so far, sorted by min, non-overlapping; the one
    // new_entity() issues from. Both owned here and released by free().
    DynamicArray<EntityRange*> ranges;
    EntityRange* active = nullptr;

    EntityIndex();
    explicit EntityIndex(BaseAllocator* allocator);

    // --- Entities -----------------------------------------------------------

    // Hands out an entity from the active range: a dead id of that range
    // when one is waiting, otherwise its next fresh id. If `out_record` is
    // given it receives the entity's record, saving a lookup. Returns 0 (and
    // a null record) if the range is exhausted, or if ranges exist but none
    // is active.
    EntityId new_entity(EntityRecord** out_record = nullptr);

    // Registers `entity` under the exact id given, allocating its record if
    // needed and reviving it if dead (taking it out of its range's pool). If
    // it is already alive its generation is overwritten with the one in
    // `entity`. A range the id falls in moves its cursor past it, so a later
    // new_entity() never issues the same id. Returns the record.
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

    // Record of the alive entity sharing `id`'s low bits, whatever
    // generation `id` carries, with that entity's full id written to
    // `out_entity`; nullptr (and 0) if the id is not alive. One lookup where
    // get_current() followed by get_record_alive() would be two: the way to
    // reach the entity behind a pair side.
    EntityRecord* resolve_low(EntityIdLow id, EntityId* out_entity) const;

    // Number of alive entities, and the id of the alive entity at `index`
    // (valid for [0, count())). Deleting an entity moves the last alive id into
    // its position.
    usz count() const;
    EntityId get_alive_id(usz index) const;
    bool is_empty() const;

    // Kills `entity`, bumping its generation. An id inside a range joins
    // that range's pool for reuse; one outside every range is dropped.
    // Returns false if the entity was not alive.
    bool delete_entity(EntityId entity);

    // --- Ranges -------------------------------------------------------------

    // Creates the range [min, max] (max 0: unbounded) without activating
    // it. Its cursor starts past any id already alive inside it. nullptr
    // (with an error) for a min of 0, a max below min, or a range that
    // overlaps an existing one.
    EntityRange* range_new(EntityIdLow min, EntityIdLow max);
    // Makes `range` (one of this index's) the one new_entity() issues from.
    // False for a range this index does not own.
    bool range_set(EntityRange* range);
    EntityRange* range_active() const { return this->active; }
    // The range `id` falls in, or nullptr.
    EntityRange* range_find(EntityIdLow id) const;

    // Restricts the ids new_entity() issues to [min, max] in one call: the
    // range starting at `min` if one exists (its max replaced), else a new
    // one, activated. A max of 0 means unbounded; a min of 0 means "start
    // after the highest id issued so far". A range the new one cuts into is
    // trimmed to end before `min` and the new one is clamped to end before
    // the next range, so ranges stay disjoint; dead ids the trim moves out
    // of their range follow into the one that now covers them.
    void set_range(EntityIdLow min, EntityIdLow max);

    // Kills every entity. Records, pages and ranges are kept for reuse.
    void clear();

    // Releases every page, the id list and every range, returning to the
    // fresh state.
    void free();

private:
    void ensure_sentinel();
    EntityRecord* ensure_page(usz page_index);

    // Appends `entity` to the alive span and returns its record, cleared.
    EntityRecord* register_alive(EntityId entity);
    // Swaps the entity with the last alive one and pops it off the alive
    // span; the record's dense goes to 0.
    void dense_remove(EntityRecord* record);
    // The range that holds `id` in its pool, searched by low id; removes it
    // from there. False if no pool has it.
    bool take_from_pool(EntityRange* range, EntityIdLow id);
    // The default range [1, unbounded), created on the first new_entity()
    // of an index that was given none.
    EntityRange* ensure_default_range();
    // Hands every recycled id of `range` that no longer falls inside it to
    // the range that does, or drops it.
    void migrate_pool(EntityRange* range);
};
