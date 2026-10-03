#include "engine/ecs/entity.hpp"

#include "engine/ecs/archetype.hpp"
#include "engine/ecs/component_record.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/entity_cleanup.hpp"
#include "engine/ecs/entity_index.hpp"
#include "engine/ecs/hooks.hpp"
#include "engine/ecs/world.hpp"

#include <cstdio>

// The root archetype stores nothing: no entity rows and no columns. An entity
// with no ids has record->archetype == root and archetype_row == 0, and the
// root's data is never touched (no insert_entity / delete_entity /
// move_entity / ensure_capacity on it). Every path below that would move an
// entity into or out of the root handles it by hand instead.

namespace ENTITY {

namespace {

bool is_root(const World* world, const Archetype* archetype) {
    return archetype == world->root_archetype;
}

// Alive record for `entity`, or nullptr. Entities created through create() or
// make_alive() always have an archetype; one registered straight through
// EntityIndex::make_alive has none and is treated as not usable here.
EntityRecord* alive_record(const World* world, const EntityId entity) {
    EntityRecord* record = world->entity_index.get_record_alive(entity);
    if (record == nullptr || record->archetype == nullptr) {
        return nullptr;
    }
    return record;
}

// Moves `entity` from `source` to `destination`, where either end may be the
// root. Leaving the root appends a fresh row (there is nothing to copy);
// arriving at the root drops the row and only re-points the record.
void move(World* world, Archetype* source, Archetype* destination, const EntityId entity, EntityRecord* record) {
    if (source == destination) {
        return;
    }
    if (is_root(world, source)) {
        destination->insert_entity(world, entity, record);
        return;
    }
    if (is_root(world, destination)) {
        source->delete_entity(world, entity, record);
        record->archetype = destination;
        record->archetype_row = 0;
        return;
    }
    source->move_entity(world, destination, entity, record);
}

// IS_EXCLUSIVE and IS_TRAVERSABLE are snapshotted into an id's component
// records when those are created (see ComponentRecordFlags), so changing the
// trait once the id has records leaves them, and the archetypes built from
// them, on the old behavior. Deletion policies are read live and do not need
// this. Not fatal: the trait is applied anyway, this only points out that
// existing pairs will not follow it.
// TODO: route through the engine log once there is one; stdout for now.
void warn_if_trait_changes_used_id(World* world, const EntityId entity, const Id trait, const char* action) {
    if (trait != ECS::EXCLUSIVE && trait != ECS::TRAVERSABLE) {
        return;
    }
    const bool used = ComponentRecord::component_record_find(world, entity) != nullptr
        || ComponentRecord::component_record_find(world, ECS::PAIR(entity, ECS::WILDCARD)) != nullptr;
    if (!used) {
        return;
    }
    printf("[ecs] warning: %s %s on entity %llx after it was already used as an id; existing component records and archetypes keep the previous behavior\n",
        action, trait == ECS::EXCLUSIVE ? "EXCLUSIVE" : "TRAVERSABLE", entity);
}

// Archetype `source` ends up in after adding the pair `id`, or nullptr if the
// pair cannot be held: one side is a wildcard, or the relation / target is not
// alive (the pair only stores low ids, so a dead end would silently bind to
// whatever later reuses that slot).
//
// An exclusive relation allows a single (R, *) per entity. When `source`
// already holds (R, T') the new target replaces it through a swap edge, which
// keeps the pair in the same column so the destination type stays sorted;
// the replaced pair is reported through `swapped_out` (0 if none).
Archetype* pair_destination(World* world, Archetype* source, const Id id, Id* swapped_out) {
    *swapped_out = 0;
    if (ECS::PAIR_HAS_WILDCARD(id)) {
        return nullptr;
    }
    if (world->pair_first(id) == 0 || world->pair_second(id) == 0) {
        return nullptr;
    }

    // Creating the archetype would ensure this record anyway; doing it first
    // gives us the relation's traits (exclusive, ...) for the pair.
    const ComponentRecord* record = ComponentRecord::component_record_ensure(world, id);
    if (record->is_exclusive()) {
        // The (R, *) alias points at the one (R, T') column, if any. The root
        // has no columns, so a fresh entity falls through to a plain add.
        const Id relation_wildcard = ECS::PAIR(ECS::PAIR_FIRST(id), ECS::WILDCARD);
        const usz* column = source->columns_index.find(relation_wildcard);
        if (column != nullptr) {
            const Id old_id = source->type.ids[*column];
            *swapped_out = old_id;
            return source->traverse_swap(world, old_id, id, *column);
        }
    }
    return source->traverse_add(world, id);
}

} // namespace

// --- Lifecycle ---------------------------------------------------------------

EntityId create(World* world) {
    EntityRecord* record = nullptr;
    const EntityId entity = world->entity_index.new_entity(&record);
    if (entity == 0) {
        return 0;
    }
    record->archetype = world->root_archetype;
    record->archetype_row = 0;
    return entity;
}

void make_alive(World* world, const EntityId entity) {
    EntityRecord* record = world->entity_index.make_alive(entity);
    if (record->archetype == nullptr) {
        // Fresh or revived: the index already zeroed the row.
        record->archetype = world->root_archetype;
        record->archetype_row = 0;
    }
}

bool delete_entity(World* world, const EntityId entity) {
    EntityRecord* record = world->entity_index.get_record_alive(entity);
    if (record == nullptr) {
        return false;
    }
    if ((record->flags & ENTITY_RECORD_DELETING) != 0) {
        // A cascade led back to an entity already being deleted up the stack.
        return false;
    }
    if (!ENTITY_CLEANUP::can_delete(world, entity)) {
        return false;
    }
    record->flags |= ENTITY_RECORD_DELETING;

    // Other entities first: remove or delete whatever refers to this one
    // (ENTITY_CLEANUP). Children die while the parent is still intact, so
    // their removed hooks can read it. Records live in fixed pages, so
    // `record` stays valid; the cleanup may have moved the entity though (a
    // pair pointing at itself was removed), so its archetype is re-read.
    ENTITY_CLEANUP::on_delete(world, entity);

    if (record->archetype != nullptr && !is_root(world, record->archetype)) {
        Archetype* archetype = record->archetype;
        // Removed hooks run while the row is still there, so callbacks can
        // read the data that is about to go away.
        if (world->hook_counts[HOOK_REMOVED] != 0) {
            const ArchetypeType& type = archetype->type;
            for (usz i = 0; i < type.id_count; i++) {
                HOOKS::fire(world, HOOK_REMOVED, entity, type.ids[i]);
            }
        }
        archetype->delete_entity(world, entity, record);
    }
    // Clears the record's archetype, row and flags as well.
    world->entity_index.delete_entity(entity);
    return true;
}

void clear(World* world, const EntityId entity) {
    EntityRecord* record = alive_record(world, entity);
    if (record == nullptr || is_root(world, record->archetype)) {
        return;
    }

    Archetype* source = record->archetype;
    // Same order as delete_entity: hooks see the row, then the row goes.
    if (world->hook_counts[HOOK_REMOVED] != 0) {
        const ArchetypeType& type = source->type;
        for (usz i = 0; i < type.id_count; i++) {
            HOOKS::fire(world, HOOK_REMOVED, entity, type.ids[i]);
        }
    }
    move(world, source, world->root_archetype, entity, record);
}

bool is_alive(const World* world, const EntityId entity) {
    return world->entity_index.is_alive(entity);
}

// --- Queries -----------------------------------------------------------------

bool has(const World* world, const EntityId entity, const Id id) {
    const EntityRecord* record = alive_record(world, entity);
    if (record == nullptr) {
        return false;
    }
    // The root has no columns, so this is false there without a special case.
    return record->archetype->contains(id);
}

void* get(const World* world, const EntityId entity, const Id id) {
    const EntityRecord* record = alive_record(world, entity);
    if (record == nullptr) {
        return nullptr;
    }

    const ArchetypeColumn* column = record->archetype->get_column(id);
    if (column == nullptr || column->type_info.length == 0) {
        // Not on the entity (always the case in the root), or a tag.
        return nullptr;
    }
    return column->read(record->archetype_row);
}

// --- Mutations ---------------------------------------------------------------

// Moves the entity to the archetype with `id`, firing only the removed hook
// for a pair an exclusive relation swaps out. Returns false if nothing
// changed: the entity already has `id`, or the pair cannot be held. Callers
// fire HOOK_ADDED themselves, so set() can write the data first.
static bool add_id(World* world, const EntityId entity, EntityRecord* record, const Id id) {
    Archetype* source = record->archetype;
    if (source->contains(id)) {
        return false;
    }

    Archetype* destination = nullptr;
    Id swapped_out = 0;
    if (ECS::IS_PAIR(id)) {
        destination = pair_destination(world, source, id, &swapped_out);
        if (destination == nullptr) {
            return false;
        }
    } else {
        // Patterns are matched against, never held: storing one would break
        // every lookup on the type. Debug-only, like the rest of the caller
        // mistakes that cannot corrupt memory.
        ENGINE_ASSERT(!ECS::IS_WILDCARD(id) && id != ECS::THIS,
            "cannot add %llx to entity %llx: WILDCARD, ANY and THIS are query patterns, not ids an entity can hold", id, entity);
        // A plain id is a full entity id; a dead or stale one is refused
        // rather than stored (see entity.hpp).
        if (!world->entity_index.is_alive(id)) {
            return false;
        }
        warn_if_trait_changes_used_id(world, entity, id, "adding");
        destination = source->traverse_add(world, id);
    }

    if (swapped_out != 0) {
        // The exclusive relation drops its old target; report that while the
        // old data is still readable.
        HOOKS::fire(world, HOOK_REMOVED, entity, swapped_out);
    }
    move(world, source, destination, entity, record);
    return true;
}

void add(World* world, const EntityId entity, const Id id) {
    EntityRecord* record = alive_record(world, entity);
    if (record == nullptr) {
        return;
    }
    if (!add_id(world, entity, record, id)) {
        return;
    }

    // add() is for tags and data-less pairs. A component added this way has
    // no value to hand back, and the row's bytes are whatever the slot held
    // before (often a previous entity's data), so zero them rather than let
    // a hook or a get() read garbage. set() is the way to add a component.
    const ArchetypeColumn* column = record->archetype->get_column(id);
    if (column != nullptr && column->type_info.length != 0) {
        // TODO: route through the engine log once there is one; stdout for now.
        printf("[ecs] warning: add() used for id %llx on entity %llx, which carries %llu bytes of data; use set() for components. The value was zeroed\n",
            id, entity, column->type_info.length);
        std::memset(column->read(record->archetype_row), 0, column->type_info.length);
    }
    HOOKS::fire(world, HOOK_ADDED, entity, id);
}

void remove(World* world, const EntityId entity, const Id id) {
    EntityRecord* record = alive_record(world, entity);
    if (record == nullptr) {
        return;
    }

    Archetype* source = record->archetype;
    if (!source->contains(id)) {
        // Covers the root: it holds no ids, so there is nothing to remove.
        return;
    }
    if (!ECS::IS_PAIR(id)) {
        warn_if_trait_changes_used_id(world, entity, id, "removing");
    }
    // Removed hooks run before the move so the data is still readable.
    HOOKS::fire(world, HOOK_REMOVED, entity, id);
    // Removing the last id resolves to the root; move() handles that end.
    Archetype* destination = source->traverse_remove(world, id);
    move(world, source, destination, entity, record);
}

void set(World* world, const EntityId entity, const Id id, const void* data) {
    EntityRecord* record = alive_record(world, entity);
    if (record == nullptr) {
        return;
    }

    // contains() and get_column() also answer for wildcards, aliasing the
    // first matching (R, T) column. Writing through that alias would be
    // ambiguous, so wildcard pairs are refused outright.
    if (ECS::IS_PAIR(id) && ECS::PAIR_HAS_WILDCARD(id)) {
        return;
    }

    // Records live in fixed pages, so `record` stays valid across the move;
    // add_id() refreshes its archetype and row (and leaves the root if
    // needed). For an exclusive pair this may swap out the old target first.
    // The added hook is held back until the data is written, so it sees the
    // value rather than uninitialized memory.
    bool added = false;
    if (!record->archetype->contains(id)) {
        added = add_id(world, entity, record, id);
        if (!added) {
            // Refused (dead-ended pair); nothing to write into.
            return;
        }
    }

    ArchetypeColumn* column = record->archetype->get_column(id);
    if (column != nullptr && column->type_info.length != 0) {
        column->write(record->archetype_row, data);
    }

    // Exactly one event: added when the id is new (its data now written),
    // changed when an existing id was overwritten. Tags store nothing, so an
    // existing tag reports no change.
    if (added) {
        HOOKS::fire(world, HOOK_ADDED, entity, id);
    } else if (column != nullptr && column->type_info.length != 0) {
        HOOKS::fire(world, HOOK_CHANGED, entity, id);
    }
}

void modified(World* world, const EntityId entity, const Id id) {
    if (world->hook_counts[HOOK_CHANGED] == 0) {
        return;
    }
    const EntityRecord* record = alive_record(world, entity);
    if (record == nullptr) {
        return;
    }
    if (ECS::IS_PAIR(id) && ECS::PAIR_HAS_WILDCARD(id)) {
        // The column map aliases (R, *) to one (R, T); refuse like set() does.
        return;
    }
    const ArchetypeColumn* column = record->archetype->get_column(id);
    if (column == nullptr || column->type_info.length == 0) {
        // Not held (always the case in the root), or a tag.
        return;
    }
    HOOKS::fire(world, HOOK_CHANGED, entity, id);
}

} // namespace ENTITY
