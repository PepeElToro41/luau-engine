#include "engine/ecs/component_record.hpp"

#include "engine/ecs/archetype/archetype.hpp"

#include "engine/ecs/hierarchy.hpp"

#include "engine/ecs/ecs.hpp"
#include "engine/ecs/entity.hpp"
#include "engine/ecs/world.hpp"

#include <cstdio>

namespace {

// TypeInfo an alive entity carries as its ECS::COMPONENT data, or nullptr when
// it has none (a tag, a wildcard, or a dead id). Falls back to the world's
// type_info_index, which bootstraps ECS::COMPONENT itself: its TypeInfo must
// be known before the column that would hold it can be created.
const TypeInfo* type_info_of(const World* world, const EntityId entity) {
    if (entity == 0) {
        return nullptr;
    }
    const TypeInfo* type_info = static_cast<const TypeInfo*>(ENTITY::get(world, entity, ECS::COMPONENT));
    if (type_info == nullptr) {
        type_info = world->get_type_info(entity);
    }
    if (type_info == nullptr || type_info->length == 0) {
        return nullptr;
    }
    return type_info;
}

// Flags derived from the traits on `trait_entity`: the relation for a pair,
// the id itself otherwise.
u32 trait_flags(const World* world, const EntityId trait_entity) {
    u32 flags = 0;
    if (trait_entity == 0) {
        return flags;
    }
    if (ENTITY::has(world, trait_entity, ECS::EXCLUSIVE)) {
        flags |= IS_EXCLUSIVE;
    }
    if (ENTITY::has(world, trait_entity, ECS::TRAVERSABLE)) {
        flags |= IS_TRAVERSABLE;
    }
    if (ENTITY::has(world, trait_entity, ECS::PAIR(ECS::ON_DELETE, ECS::DELETE))) {
        flags |= ON_DELETE_DELETE;
    }
    if (ENTITY::has(world, trait_entity, ECS::PAIR(ECS::ON_DELETE_TARGET, ECS::DELETE))) {
        flags |= ON_DELETE_TARGET_DELETE;
    }
    if (ENTITY::has(world, trait_entity, ECS::PAIR(ECS::ON_DELETE, ECS::PANIC))) {
        flags |= ON_DELETE_PANIC;
    }
    if (ENTITY::has(world, trait_entity, ECS::PAIR(ECS::ON_DELETE_TARGET, ECS::PANIC))) {
        flags |= ON_DELETE_TARGET_PANIC;
    }
    return flags;
}

} // namespace

void PairRecord::initialize(BaseAllocator* allocator) {
    new (&this->first_records) HashMap<Id, ComponentRecord*>(allocator);
    new (&this->second_records) HashMap<Id, ComponentRecord*>(allocator);
    new (&this->trav_records) HashMap<Id, ComponentRecord*>(allocator);
}

void PairRecord::free() {
    this->first_records.free();
    this->second_records.free();
    this->trav_records.free();
}

ComponentRecord::ComponentRecord(World* world) :
    world(world),
    allocator(world->allocator),
    columns_index(world->allocator),
    archetype_list(world->allocator),
    archetype_index(world->allocator)
{}


void ComponentRecord::destroy() {
    if (this->pair_record != nullptr) {
        this->pair_record->free();
        this->allocator->free(this->pair_record);
        this->pair_record = nullptr;
    }
    if (this->hooks != nullptr) {
        this->hooks->free();
        this->allocator->free(this->hooks);
        this->hooks = nullptr;
    }
    HIERARCHY::free_node(this);
    this->first_wildcard = nullptr;
    this->second_wildcard = nullptr;
    this->columns_index.free();
    this->archetype_list.free();
    this->archetype_index.free();
}

HookList* ComponentRecord::ensure_hooks() {
    if (this->hooks == nullptr) {
        this->hooks = this->allocator->allocate_array<HookList>(1);
        this->hooks->initialize(this->allocator);
    }
    return this->hooks;
}

PairRecord* ComponentRecord::ensure_pair_record() {
    if (this->pair_record == nullptr) {
        this->pair_record = this->allocator->allocate_array<PairRecord>(1);
        this->pair_record->initialize(this->allocator);
    }
    return this->pair_record;
}

bool ComponentRecord::link_archetype(Archetype* archetype, const usz column) {
    const ArchetypeId id = archetype->archetype_id;
    if (this->columns_index.contains(id)) {
        return false;
    }
    this->columns_index.insert(id, column);
    this->archetype_index.insert(id, this->archetype_list.count);
    this->archetype_list.push(RecordColumn { archetype, column });
    return true;
}

bool ComponentRecord::unlink_archetype(const ArchetypeId archetype) {
    const usz* found = this->archetype_index.find(archetype);
    if (found == nullptr) {
        return false;
    }
    const usz position = *found;
    this->columns_index.remove(archetype);
    this->archetype_index.remove(archetype);
    this->archetype_list.remove_swap(position);
    if (position < this->archetype_list.count) {
        // The former last entry now sits at `position`.
        this->archetype_index.insert(this->archetype_list[position].archetype->archetype_id, position);
    }
    return true;
}

ComponentRecord* ComponentRecord::component_record_create(World* world, const Id id) {
    // The sparse list only zeroes the slot, so construct the record in place.
    const SparseId sparse_id = world->component_records.new_element();
    ComponentRecord* record = world->component_records.get_element_any(sparse_id);
    new (record) ComponentRecord(world);

    record->sparse_id = sparse_id;
    record->id = id;

    const TypeInfo* type_info = nullptr;
    if (ECS::IS_PAIR(id)) {
        // Pairs only store low ids; resolve the alive entities behind them.
        const EntityId first = world->pair_first(id);
        const EntityId second = world->pair_second(id);

        // Traits (exclusive, traversable, on-delete) live on the relation.
        record->flags |= trait_flags(world, first);

        // A wildcard pair covers ids with different data; it carries none itself.
        if (!ECS::PAIR_HAS_WILDCARD(id)) {
            // The relation's data wins; a tag relation takes the target's data.
            type_info = type_info_of(world, first);
            if (type_info == nullptr) {
                type_info = type_info_of(world, second);
            }
        }
    } else {
        // A plain id is the entity itself, generation included.
        const EntityId entity = world->entity_index.is_alive(id) ? id : 0;
        record->flags |= trait_flags(world, entity);
        type_info = type_info_of(world, entity);
    }

    if (type_info != nullptr) {
        record->flags |= IS_COMPONENT;
        record->type_info = *type_info;
    }

    world->component_index.insert(id, record);
    return record;
}

ComponentRecord* ComponentRecord::component_record_ensure(World* world, const Id id) {
    ComponentRecord** existing = world->component_index.find(id);
    if (existing != nullptr) {
        return *existing;
    }
    ComponentRecord* record = ComponentRecord::component_record_create(world, id);

    // A concrete pair (R, T) is reachable through its wildcards (R, *) and
    // (*, T), so make sure both exist. Wildcard pairs stop here: their own
    // wildcard would be (*, *), and recursing on them would loop.
    if (ECS::IS_PAIR(id)) {
        const EntityIdLow first = ECS::PAIR_FIRST(id);
        const EntityIdLow second = ECS::PAIR_SECOND(id);
        if (first != ECS::WILDCARD && second != ECS::WILDCARD) {
            record->first_wildcard = ComponentRecord::component_record_ensure(world, ECS::PAIR(first, ECS::WILDCARD));
            record->second_wildcard = ComponentRecord::component_record_ensure(world, ECS::PAIR(ECS::WILDCARD, second));

            // Let both wildcards enumerate this pair (see PairRecord).
            record->first_wildcard->ensure_pair_record()->first_records.insert(id, record);
            PairRecord* second_pairs = record->second_wildcard->ensure_pair_record();
            second_pairs->second_records.insert(id, record);
            if (record->is_traversable()) {
                second_pairs->trav_records.insert(id, record);
                // The target can now have children; the record gets its
                // hierarchy node (see hierarchy.hpp).
                HIERARCHY::on_target_record_created(world, record);
            }
        }
    }
    return record;
}

ComponentRecord* ComponentRecord::component_record_find(World* world, const Id id) {
    ComponentRecord** record = world->component_index.find(id);
    if (record) {
        return *record;
    }
    return nullptr;
}

bool ComponentRecord::component_record_delete(World* world, ComponentRecord* record) {
    if (record->archetype_count() != 0) {
        fprintf(stderr, "[ecs] error: deleting component record %llx still held by %llu archetypes\n",
            static_cast<unsigned long long>(record->id), static_cast<unsigned long long>(record->archetype_count()));
        return false;
    }
    const PairRecord* pairs = record->pair_record;
    if (pairs != nullptr && (!pairs->first_records.is_empty() || !pairs->second_records.is_empty())) {
        fprintf(stderr, "[ecs] error: deleting wildcard record %llx that still covers %llu pairs\n",
            record->id, static_cast<u64>(pairs->first_records.count + pairs->second_records.count));
        return false;
    }

    // A concrete pair is listed on both of its wildcards.
    if (record->first_wildcard != nullptr && record->first_wildcard->pair_record != nullptr) {
        record->first_wildcard->pair_record->first_records.remove(record->id);
    }
    if (record->second_wildcard != nullptr && record->second_wildcard->pair_record != nullptr) {
        record->second_wildcard->pair_record->second_records.remove(record->id);
        if (record->second_wildcard->pair_record->trav_records.remove(record->id)) {
            // Unlinks the node from its parents and frees it; maybe the last
            // traversable pair pointing at the target.
            HIERARCHY::on_target_record_deleted(world, record);
        }
    }

    // The hooks go with the record; keep the world's totals honest.
    if (record->hooks != nullptr) {
        for (u32 kind = 0; kind < HOOK_KIND_COUNT; kind++) {
            world->hook_counts[kind] -= static_cast<u32>(record->hooks->lists[kind].count);
        }
    }

    world->component_index.remove(record->id);
    const SparseId sparse_id = record->sparse_id;
    record->destroy();
    world->component_records.delete_element(sparse_id);
    return true;
}
