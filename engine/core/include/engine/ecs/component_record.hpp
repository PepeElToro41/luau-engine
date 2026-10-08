#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/ecs/hooks.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/templates/hash_map.hpp"
#include "engine/templates/sparse_list.hpp"

#include <new>

struct ComponentRecord;
struct HierarchyNode;
struct World;

// Snapshot of the traits on the id's entity (the relation, for a pair) taken
// when the record is created. Setting a trait after the id has been used does
// not update existing records: ENTITY::add / remove print a warning when
// EXCLUSIVE or TRAVERSABLE changes on an id that already has records.
// Deletion policies are read off the trait entity at delete time instead
// (see entity_cleanup.hpp), so they never go stale.
enum ComponentRecordFlags : u32 {
    IS_COMPONENT = 1 << 0,            // the id carries data (has a TypeInfo), not just a tag
    IS_EXCLUSIVE = 1 << 1,            // an entity can hold at most one pair with this relation
    ON_DELETE_DELETE = 1 << 2,        // (ON_DELETE, DELETE): deleting the id's entity deletes its holders
    IS_TRAVERSABLE = 1 << 3,          // queries may walk up through this relation
    ON_DELETE_TARGET_DELETE = 1 << 4, // (ON_DELETE_TARGET, DELETE): deleting a pair's target deletes its holders
    ON_DELETE_PANIC = 1 << 5,         // (ON_DELETE, PANIC): the id's entity cannot be deleted
    ON_DELETE_TARGET_PANIC = 1 << 6,  // (ON_DELETE_TARGET, PANIC): a pair's target cannot be deleted while the pair is held
};

// Extra bookkeeping only wildcard pair records (R, *) and (*, T) carry, so
// they can reach every concrete pair they cover. Filled by
// component_record_ensure when a concrete pair record is created and emptied
// again by component_record_delete. Keys are the concrete pair ids.
struct PairRecord {
    // On (R, *): every concrete (R, T) record.
    HashMap<Id, ComponentRecord*> first_records;
    // On (*, T): every concrete (R, T) record.
    HashMap<Id, ComponentRecord*> second_records;
    // On (*, T): the (R, T) records whose relation is traversable.
    HashMap<Id, ComponentRecord*> trav_records;

    void initialize(BaseAllocator* allocator);
    void free();
};

// Per-id bookkeeping for the world: which archetypes contain the id, which
// column holds it in each, and what the id is (component, tag, pair, ...).
// Records live in a SparseList, so they arrive zeroed: call initialize()
// before use and free() before deleting the slot.
struct ComponentRecord {
    World* world;
    BaseAllocator* allocator;

    u32 archetype_count = 0;
    SparseId sparse_id = 0;      // slot in the world's component record list
    
    Id id = 0;
    u32 flags = 0;               // ComponentRecordFlags
    TypeInfo type_info {}; // nullptr for tags
    // Only allocated for wildcard pair ids (R, *) and (*, T).
    PairRecord* pair_record = nullptr;
    // Hooks registered on exactly this id (see World::hook_added). Allocated
    // on first registration; nullptr means none.
    HookList* hooks = nullptr;
    // For a concrete pair (R, T): the records for (R, *) and (*, T), set by
    // component_record_ensure so hook dispatch reaches them without a lookup.
    // nullptr for plain ids and for wildcard pairs.
    ComponentRecord* first_wildcard = nullptr;
    ComponentRecord* second_wildcard = nullptr;
    // For a concrete pair (R, T) with R traversable: the cached depth of its
    // holders and the records that depend on it (see hierarchy.hpp). Owned by
    // HIERARCHY::, which creates it with the record. nullptr otherwise.
    HierarchyNode* hierarchy = nullptr;

    // archetype id -> index of this id's column inside that archetype
    HashMap<ArchetypeId, usz> columns_index;

    explicit ComponentRecord(World* world);

    bool is_component() const { return (this->flags & IS_COMPONENT) != 0; }
    bool is_exclusive() const { return (this->flags & IS_EXCLUSIVE) != 0; }
    bool is_traversable() const { return (this->flags & IS_TRAVERSABLE) != 0; }
    bool deletes_on_delete() const { return (this->flags & ON_DELETE_DELETE) != 0; }
    bool deletes_on_delete_target() const { return (this->flags & ON_DELETE_TARGET_DELETE) != 0; }
    bool panics_on_delete() const { return (this->flags & ON_DELETE_PANIC) != 0; }
    bool panics_on_delete_target() const { return (this->flags & ON_DELETE_TARGET_PANIC) != 0; }

    // Releases what the record owns: its maps, pair record and hook lists.
    // Nothing else is told, so the world's indexes and hook counts keep
    // pointing at it; component_record_delete does the full teardown.
    void destroy();

    // Points this record at `column` of `archetype` unless the archetype is
    // already registered, in which case nothing changes. Meant for wildcard
    // records, so (R, *) and (*, T) keep pointing at the first matching pair
    // column of each archetype. Returns true if the entry was added.
    bool append_to_pair(ArchetypeId archetype, usz column);

    // The hook list, allocating it on first use.
    HookList* ensure_hooks();
    // The pair record, allocating it on first use. Only for wildcard pairs.
    PairRecord* ensure_pair_record();

    static ComponentRecord* component_record_create(World* world, Id id);
    static ComponentRecord* component_record_ensure(World* world, Id id);
    static ComponentRecord* component_record_find(World* world, Id id);

    // Removes `record` from the world: unregisters it from its wildcard
    // records, drops it from the component index, forgets the hooks
    // registered on it (adjusting the world's hook counts) and frees its slot.
    // The id must no longer be held by any archetype (archetype_count == 0)
    // and a wildcard record must no longer cover any concrete pair; otherwise
    // an error is printed, nothing is touched and false is returned.
    static bool component_record_delete(World* world, ComponentRecord* record);
};
