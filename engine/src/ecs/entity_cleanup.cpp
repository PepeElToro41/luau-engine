#include "engine/ecs/entity_cleanup.hpp"

#include "engine/ecs/archetype.hpp"
#include "engine/ecs/component_record.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/entity.hpp"
#include "engine/ecs/entity_index.hpp"
#include "engine/ecs/world.hpp"
#include "engine/memory/temporal_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"

#include <cstdio>

// Everything here works on snapshots: archetype ids and entity ids are copied
// out before anything is mutated, and every pointer is re-resolved through the
// world after a step that may have cascaded. A nested deletion can move rows,
// destroy archetypes and create new ones, so no Archetype* or row index is
// trusted across an ENTITY::remove or ENTITY::delete_entity call.
//
// Snapshots live on a TemporalAllocator and are reserved to their exact size
// up front, so each one costs the arena a single block that is released when
// the function returns. Nested cascades nest their scopes the same way.

namespace ENTITY_CLEANUP {

namespace {

// Whether (trait, policy) is set on `entity`.
bool has_policy(const World* world, const Id trait, const Id policy, const EntityId entity) {
    return ENTITY::has(world, entity, ECS::PAIR(trait, policy));
}

// Whether (trait, DELETE) is set on `entity`.
bool policy_deletes(const World* world, const Id trait, const EntityId entity) {
    return has_policy(world, trait, ECS::DELETE, entity);
}

// Same, for the alive entity behind a pair side (a low id).
bool policy_deletes_low(const World* world, const Id trait, const EntityIdLow low) {
    const EntityId entity = world->entity_index.get_current(low);
    return entity != 0 && policy_deletes(world, trait, entity);
}

// Whether the entities of an archetype with `type` are deleted (rather than
// having the matching ids removed) when `pattern`'s entity goes away.
bool deletes_holders(const World* world, const ArchetypeType& type, const Id pattern) {
    if (!ECS::IS_PAIR(pattern)) {
        // E as a component or tag (the pattern is E's full id): its own ON_DELETE.
        return policy_deletes(world, ECS::ON_DELETE, pattern);
    }
    if (ECS::PAIR_SECOND(pattern) == ECS::WILDCARD) {
        // (E, *): E is the relation, again its own ON_DELETE.
        return policy_deletes_low(world, ECS::ON_DELETE, ECS::PAIR_FIRST(pattern));
    }
    // (*, E): ON_DELETE_TARGET of every relation pointing at E; DELETE wins.
    for (usz i = 0; i < type.id_count; i++) {
        const Id id = type.ids[i];
        if (ECS::ID_MATCHES(pattern, id) && policy_deletes_low(world, ECS::ON_DELETE_TARGET, ECS::PAIR_FIRST(id))) {
            return true;
        }
    }
    return false;
}

// Removes every id matching `pattern` from `entity`, one ENTITY::remove at a
// time. Stops early if a removed hook deletes the entity.
void remove_matching(World* world, const EntityId entity, const Id pattern) {
    const EntityRecord* record = world->entity_index.get_record_alive(entity);
    if (record == nullptr || record->archetype == nullptr) {
        return;
    }
    // Each pass removes one id, so the type can shrink at most this often. A
    // hook that keeps re-adding a matching id would otherwise loop forever.
    usz budget = record->archetype->type.id_count;
    while (budget-- > 0) {
        const ArchetypeType& type = record->archetype->type;
        Id match = 0;
        for (usz i = 0; i < type.id_count; i++) {
            if (ECS::ID_MATCHES(pattern, type.ids[i])) {
                match = type.ids[i];
                break;
            }
        }
        if (match == 0) {
            return;
        }
        ENTITY::remove(world, entity, match);
        record = world->entity_index.get_record_alive(entity);
        if (record == nullptr || record->archetype == nullptr) {
            return;
        }
    }
}

// Empties the archetype `archetype_id` of every entity, applying the policy
// for `pattern`. Returns false if some entity would not leave it: a hook that
// re-adds the dying id, or the like.
bool clear_archetype(World* world, const ArchetypeId archetype_id, const Id pattern) {
    TemporalAllocator temp = TemporalAllocator::create();
    DynamicArray<EntityId> entities(&temp);
    bool cleared = true;

    // A nested cascade may drop rows into this archetype while we work on it
    // (an entity losing some other id lands in a smaller type), so keep
    // taking snapshots until it is empty.
    while (true) {
        const Archetype* archetype = world->archetypes.get_element_alive(archetype_id);
        if (archetype == nullptr || archetype->data.entity_count == 0) {
            break;
        }
        const bool deletes = deletes_holders(world, archetype->type, pattern);

        entities.clear();
        entities.reserve(archetype->data.entity_count);
        for (usz row = 0; row < archetype->data.entity_count; row++) {
            entities.push(archetype->data.entities[row]);
        }

        for (const EntityId entity : entities) {
            const EntityRecord* record = world->entity_index.get_record_alive(entity);
            if (record == nullptr) {
                // Already deleted by an earlier step of this cascade.
                continue;
            }
            if (deletes && (record->flags & ENTITY_RECORD_DELETING) == 0) {
                if (ENTITY::delete_entity(world, entity)) {
                    continue;
                }
                // Refused (a PANIC policy on this entity): it stays, but the
                // ids pointing at the deleted entity still have to go.
            }
            // REMOVE, a refused delete, or an entity already on its way out
            // (it holds a pair that points at itself, or at an ancestor
            // being deleted): take the ids away so its row leaves here.
            remove_matching(world, entity, pattern);
        }

        // Every entity in the snapshot must have left; if one is still here
        // nothing we do will move it and looping would never end.
        archetype = world->archetypes.get_element_alive(archetype_id);
        if (archetype == nullptr) {
            break;
        }
        for (const EntityId entity : entities) {
            const EntityRecord* record = world->entity_index.get_record_alive(entity);
            if (record != nullptr && record->archetype == archetype) {
                fprintf(stderr, "[ecs] error: on_delete could not clear %llx from entity %llx (re-added by a hook?)\n",
                    pattern, entity);
                cleared = false;
                break;
            }
        }
        if (!cleared) {
            break;
        }
    }
    return cleared;
}

// Empties and destroys every archetype holding an id matching `pattern`;
// `record` is the pattern's own ComponentRecord, whose columns_index lists
// exactly those archetypes. Runs in passes: the archetypes an entity lands in
// after losing one of two matching pairs, or that a nested cascade creates,
// still match and are picked up by the next pass.
void clear_pattern(World* world, ComponentRecord* record, const Id pattern) {
    TemporalAllocator temp = TemporalAllocator::create();
    DynamicArray<ArchetypeId> archetype_ids(&temp);

    while (record->archetype_count != 0) {
        archetype_ids.clear();
        archetype_ids.reserve(record->columns_index.count);
        for (const auto& entry : record->columns_index) {
            archetype_ids.push(entry.key);
        }

        bool stuck = false;
        for (const ArchetypeId archetype_id : archetype_ids) {
            if (!clear_archetype(world, archetype_id, pattern)) {
                stuck = true;
                continue;
            }
            // clear_archetype re-resolves after every step, so this is either
            // gone already (a nested cascade emptied and destroyed it) or
            // empty and ours to destroy.
            Archetype* archetype = world->archetypes.get_element_alive(archetype_id);
            if (archetype != nullptr && archetype->data.entity_count == 0) {
                archetype->destroy();
            }
        }
        if (stuck) {
            break;
        }
    }
}

// Deletes every concrete pair record a wildcard record covers, then the
// wildcard record itself. `use_first` picks first_records (for (E, *)) over
// second_records (for (*, E)).
void delete_pair_records(World* world, const Id wildcard, const bool use_first) {
    ComponentRecord* record = ComponentRecord::component_record_find(world, wildcard);
    if (record == nullptr) {
        return;
    }
    if (record->pair_record != nullptr) {
        // Deleting a record removes it from this map, so copy the keys first.
        TemporalAllocator temp = TemporalAllocator::create();
        DynamicArray<Id> ids(&temp);
        
        const HashMap<Id, ComponentRecord*>& records = use_first ? record->pair_record->first_records : record->pair_record->second_records;
        ids.reserve(records.count);
        for (const auto& entry : records) {
            ids.push(entry.key);
        }
        for (const Id id : ids) {
            ComponentRecord* pair = ComponentRecord::component_record_find(world, id);
            if (pair != nullptr) {
                ComponentRecord::component_record_delete(world, pair);
            }
        }
    }
    ComponentRecord::component_record_delete(world, record);
}

// Whether some entity holds the record's id. Archetypes stay registered on a
// record after they empty out, so archetype_count alone cannot tell.
bool record_in_use(const World* world, const ComponentRecord* record) {
    for (const auto& entry : record->columns_index) {
        const Archetype* archetype = world->archetypes.get_element_alive(entry.key);
        if (archetype != nullptr && archetype->data.entity_count != 0) {
            return true;
        }
    }
    return false;
}

} // namespace

bool can_delete(World* world, const EntityId entity) {
    if (has_policy(world, ECS::ON_DELETE, ECS::PANIC, entity)) {
        fprintf(stderr, "[ecs] error: cannot delete entity %llx: it has (ON_DELETE, PANIC)\n", entity);
        return false;
    }

    // (ON_DELETE_TARGET, PANIC) on a relation protects its targets while the
    // pairs are held. The (*, E) record lists every (R, E) in use.
    const ComponentRecord* as_target = ComponentRecord::component_record_find(world, ECS::PAIR(ECS::WILDCARD, entity));
    if (as_target == nullptr || as_target->pair_record == nullptr) {
        return true;
    }
    for (const auto& entry : as_target->pair_record->second_records) {
        const ComponentRecord* pair = entry.value;
        if (!record_in_use(world, pair)) {
            continue;
        }
        const EntityId relation = world->pair_first(pair->id);
        if (relation != 0 && has_policy(world, ECS::ON_DELETE_TARGET, ECS::PANIC, relation)) {
            fprintf(stderr, "[ecs] error: cannot delete entity %llx: it is the target of (%llx, %llx) and the relation has (ON_DELETE_TARGET, PANIC)\n",
                entity, relation, entity);
            return false;
        }
    }
    return true;
}

void on_delete(World* world, const EntityId entity) {
    // As a plain id E is keyed by its full id; a pair only has room for its
    // low id on either side.
    const Id relation_wildcard = ECS::PAIR(entity, ECS::WILDCARD);
    const Id target_wildcard = ECS::PAIR(ECS::WILDCARD, entity);

    // Move every other entity off the ids that mention E. The records are
    // looked up per pattern: a plain entity that was never used as an id has
    // none and costs nothing here.
    if (ComponentRecord* record = ComponentRecord::component_record_find(world, entity)) {
        clear_pattern(world, record, entity);
    }
    if (ComponentRecord* record = ComponentRecord::component_record_find(world, relation_wildcard)) {
        clear_pattern(world, record, relation_wildcard);
    }
    if (ComponentRecord* record = ComponentRecord::component_record_find(world, target_wildcard)) {
        clear_pattern(world, record, target_wildcard);
    }

    // No archetype mentions E any more, so the records for it can go. A
    // record that is somehow still in use is reported and left in place.
    delete_pair_records(world, relation_wildcard, true);
    delete_pair_records(world, target_wildcard, false);
    if (ComponentRecord* record = ComponentRecord::component_record_find(world, entity)) {
        ComponentRecord::component_record_delete(world, record);
    }
}

} // namespace ENTITY_CLEANUP
