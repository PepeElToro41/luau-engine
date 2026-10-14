#include "engine/ecs/world_clone.hpp"

#include "engine/ecs/archetype/archetype.hpp"
#include "engine/ecs/component_record.hpp"
#include "engine/ecs/component_registry.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/entity.hpp"
#include "engine/ecs/entity_index.hpp"
#include "engine/ecs/hierarchy.hpp"
#include "engine/memory/temporal_allocator.hpp"
#include "engine/utils/log.hpp"

#include <cstring>

// --- Cloner --------------------------------------------------------------------

void CLONE::expose(World& world, const Id id, const CloneFn clone, void* user_data) {
    if (id == 0 || ECS::IS_PAIR(id) || clone == nullptr || !world.alive(id)) {
        return;
    }
    COMPONENT_REGISTRY::preregister<Cloner>("Cloner");
    Cloner cloner;
    cloner.clone = clone;
    cloner.user_data = user_data;
    world.set(id, cloner);
}

const Cloner* CLONE::of(World& world, const Id id) {
    if (id == 0 || ECS::IS_PAIR(id) || !world.alive(id)) {
        return nullptr;
    }
    // Looked up without id<Cloner>(), which would register the component on
    // a world that never exposed one (the source of a clone, for instance).
    const EntityId* cloner_id = world.type_index.find(TYPE_ID::get<Cloner>());
    if (cloner_id == nullptr) {
        return nullptr;
    }
    return static_cast<const Cloner*>(world.get(id, *cloner_id));
}

// --- Clone ---------------------------------------------------------------------

namespace {

// Whether the alive entity behind a pair's side carries component data.
bool side_has_data(World& world, const EntityId entity) {
    if (entity == 0) {
        return false;
    }
    const TypeInfo* info = static_cast<const TypeInfo*>(world.get(entity, ECS::COMPONENT));
    return info != nullptr && info->length != 0;
}

// The Cloner that copies the data of `id` in an archetype column: the
// component entity's for a plain id, and for a pair (R, T) the one on the
// side whose data the pair stores (R's when R carries data, else T's).
const Cloner* cloner_of(World& source, const Id id) {
    if (!ECS::IS_PAIR(id)) {
        return CLONE::of(source, id);
    }
    const EntityId first = source.pair_first(id);
    const EntityId second = source.pair_second(id);
    if (side_has_data(source, first)) {
        return CLONE::of(source, first);
    }
    return side_has_data(source, second) ? CLONE::of(source, second) : nullptr;
}

WorldCloneResult fail(const char* error) {
    WorldCloneResult result;
    result.ok = false;
    result.error = error;
    LOG::error(LOG_CATEGORY::ECS, "world clone: %s", error);
    return result;
}

// Adds `id` to every entity of every archetype of `record` (the id's
// column tells which concrete pair an exclusive wildcard record points at).
void add_to_holders(World& destination, const ComponentRecord* record) {
    if (record == nullptr) {
        return;
    }
    for (const RecordColumn& column : record->archetype_list) {
        const Archetype* archetype = column.archetype;
        const Id id = archetype->type.ids[column.column];
        for (usz row = 0; row < archetype->data.entity_count; ++row) {
            ENTITY::add(&destination, archetype->data.entities[row], id);
        }
    }
}

} // namespace

WorldCloneResult WORLD_CLONE::clone(World& source, World& destination, const WorldCloneDesc& desc) {
    if (&source == &destination) {
        return fail("a world cannot be cloned into itself");
    }
    if (source.root_archetype == nullptr || destination.root_archetype == nullptr) {
        return fail("both worlds must be init()ed");
    }
    if (destination.entity_index.count() != ECS::REST || destination.type_index.count != 0 || destination.component_names.count != 0) {
        return fail("the destination must be a fresh world: init()ed, with no entity created and no component registered");
    }

    // --- The component tiers ---------------------------------------------------
    // Both tiers come over as they are: the preregistered one would resolve
    // to the same ids anyway, the runtime one is what the archetypes below
    // are typed with. The owner table and the counter follow so later
    // registrations in the destination never land on a cloned id.
    for (auto& entry : source.type_index) {
        destination.type_index.insert(entry.key, entry.value);
    }
    for (auto& entry : source.component_names) {
        destination.component_names.insert(entry.key, entry.value);
    }
    memcpy(destination.component_owner, source.component_owner, sizeof(destination.component_owner));
    destination.runtime_next_id = source.runtime_next_id;

    // --- Every entity, under its id and generation ----------------------------
    // The built-ins are alive in both already; adopting generation 0 again
    // changes nothing for them.
    const usz count = source.entity_index.count();
    for (usz i = 0; i < count; ++i) {
        destination.make_alive(source.entity_index.get_alive_id(i));
    }

    // --- Component sizes ---------------------------------------------------------
    // A record created for an id reads the id's TypeInfo off its entity, so
    // every (COMPONENT, info) goes in before any archetype needs it. Through
    // ENTITY::set: these few entities move archetype, which is cheap, and
    // COMPONENT describes itself in both worlds already.
    if (const ComponentRecord* record = ComponentRecord::component_record_find(&source, ECS::COMPONENT)) {
        for (const RecordColumn& column : record->archetype_list) {
            const Archetype* archetype = column.archetype;
            const ArchetypeColumn& infos = archetype->data.columns[column.column];
            for (usz row = 0; row < archetype->data.entity_count; ++row) {
                const EntityId entity = archetype->data.entities[row];
                if (entity == ECS::COMPONENT) {
                    continue;
                }
                ENTITY::set(&destination, entity, ECS::COMPONENT, infos.read(row));
            }
        }
    }

    // --- Relation traits -------------------------------------------------------
    // Likewise snapshotted when a pair's record is created: exclusivity,
    // traversability and the deletion policies of every relation go in
    // before any pair with it. ENTITY::add is a no-op for the built-ins,
    // which init() already gave theirs.
    add_to_holders(destination, ComponentRecord::component_record_find(&source, ECS::EXCLUSIVE));
    add_to_holders(destination, ComponentRecord::component_record_find(&source, ECS::TRAVERSABLE));
    add_to_holders(destination, ComponentRecord::component_record_find(&source, ECS::PAIR(ECS::ON_DELETE, ECS::WILDCARD)));
    add_to_holders(destination, ComponentRecord::component_record_find(&source, ECS::PAIR(ECS::ON_DELETE_TARGET, ECS::WILDCARD)));

    // --- The archetypes, in bulk -------------------------------------------------
    WorldCloneResult result;
    for (usz i = 0; i < source.archetypes.alive_count; ++i) {
        const Archetype* from = source.archetypes.get_element_any(source.archetypes.get_alive_id(i));
        if (from == source.root_archetype || from->data.entity_count == 0) {
            // Empty tables are rebuilt on demand, like anywhere else.
            continue;
        }
        Archetype* to = Archetype::ensure_archetype(&destination, from->type);
        const usz column_count = from->type.id_count;
        for (usz c = 0; c < column_count; ++c) {
            if (to->data.columns[c].type_info.length != from->data.columns[c].type_info.length) {
                LOG::error(LOG_CATEGORY::ECS, "world clone: id %llx stores %llu bytes in the source and %llu in the destination",
                    static_cast<unsigned long long>(from->type.ids[c]), static_cast<unsigned long long>(from->data.columns[c].type_info.length),
                    static_cast<unsigned long long>(to->data.columns[c].type_info.length));
                return fail("a component has another size in the destination");
            }
        }
        // The copier of every column, resolved once per archetype.
        TemporalAllocator temp = TemporalAllocator::create();
        Cloner* cloners = temp.allocate_array<Cloner>(column_count);
        for (usz c = 0; c < column_count; ++c) {
            const Cloner* cloner = from->data.columns[c].type_info.length != 0 ? cloner_of(source, from->type.ids[c]) : nullptr;
            cloners[c] = cloner != nullptr ? *cloner : Cloner {};
        }

        to->ensure_capacity(to->data.entity_count + from->data.entity_count);
        for (usz row = 0; row < from->data.entity_count; ++row) {
            const EntityId entity = from->data.entities[row];
            EntityRecord* record = destination.entity_index.get_record_alive(entity);
            Archetype* previous = record->archetype;
            if (previous != to) {
                // Most entities come from the root; the component and
                // relation entities placed above move with what they got.
                if (previous == destination.root_archetype) {
                    to->insert_entity(&destination, entity, record);
                } else {
                    previous->move_entity(&destination, to, entity, record);
                }
            }
            const usz to_row = record->archetype_row;
            for (usz c = 0; c < column_count; ++c) {
                const ArchetypeColumn& from_column = from->data.columns[c];
                const usz length = from_column.type_info.length;
                if (length == 0) {
                    continue;
                }
                const void* value = from_column.read(row);
                void* copy = to->data.columns[c].read(to_row);
                if (cloners[c].clone != nullptr) {
                    cloners[c].clone(value, copy, length, cloners[c].user_data);
                } else {
                    memcpy(copy, value, length);
                }
            }
            if (previous != to) {
                // What ENTITY::move would have told the hierarchy caches.
                HIERARCHY::on_move(&destination, entity, record, previous, to);
            }
        }
        result.archetypes += 1;
    }
    result.entities = count;

    if (desc.fence) {
        // The range init() gave the destination now ends below the fence and
        // stays inactive: a cloned id deleted in the clone pools there and
        // is never issued again while the fence range is the active one.
        destination.set_range(0, 0);
        result.fence = destination.range_min();
    }
    result.ok = true;
    return result;
}
