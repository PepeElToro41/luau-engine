#include "engine/ecs/archetype/archetype.hpp"

#include "engine/ecs/archetype/archetype_listener.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/world.hpp"
#include "engine/memory/temporal_allocator.hpp"
#include "engine/utils/math.hpp"

#include <cstddef>
#include <cstdio>
#include <new>


ArchetypeType ArchetypeType::clone(BaseAllocator *allocator, const usz reserve) const {
    const usz capacity = this->id_count + reserve;
    u64* copy = allocator->allocate_array<u64>(capacity);
    if (this->id_count > 0) {
        std::memcpy(copy, this->ids, this->id_count * sizeof(u64));
    }
    return ArchetypeType(copy, this->id_count);
}

ArchetypeType ArchetypeType::insert(BaseAllocator* allocator, const Id id) const {
    const ArchetypeType copy = this->clone(allocator, 1);

    usz position = this->id_count;
    for (usz i = 0; i < this->id_count; i++) {
        if (this->ids[i] == id) {
            // Already part of the type: the set does not change.
            return copy;
        }
        if (this->ids[i] > id) {
            position = i;
            break;
        }
    }

    // The ranges overlap, so this has to be a memmove.
    std::memmove(&copy.ids[position + 1], &copy.ids[position], (this->id_count - position) * sizeof(u64));
    copy.ids[position] = id;

    return ArchetypeType(copy.ids, this->id_count + 1);
}

ArchetypeType ArchetypeType::remove(BaseAllocator* allocator, const Id id) const {
    // Sized for the full type so a missing id cannot overflow the buffer.
    u64* new_ids = allocator->allocate_array<u64>(this->id_count);
    usz pushed = 0;
    for (usz i = 0; i < this->id_count; i++) {
        if (this->ids[i] != id) {
            new_ids[pushed++] = this->ids[i];
        }
    }

    return ArchetypeType(new_ids, pushed);
}

void ArchetypeType::free(BaseAllocator* allocator) {
    // clone() may hand back nullptr for an empty type; free(nullptr) is a no-op.
    allocator->free(this->ids);
    this->ids = nullptr;
    this->id_count = 0;
}

void Archetype::ensure_capacity(const usz capacity) {
    ArchetypeData& data = this->data;
    if (capacity <= data.entity_capacity) {
        return;
    }

    // Double the current capacity, but jump straight to whatever the request
    // needs if that is larger. Rounding keeps the result a power of two.
    usz new_capacity = data.entity_capacity * 2;
    if (new_capacity < ARCHETYPE_INITIAL_CAPACITY) {
        new_capacity = ARCHETYPE_INITIAL_CAPACITY;
    }
    if (new_capacity < capacity) {
        new_capacity = capacity;
    }
    new_capacity = MATH::next_power_of_two(new_capacity);
    data.entities = this->allocator->reallocate_array(data.entities, new_capacity);

    for (usz i = 0; i < data.column_count; i++) {
        ArchetypeColumn& column = data.columns[i];
        const usz length = column.type_info.length;
        if (length == 0) {
            // Tag column (column_count mirrors type.id_count): nothing to store.
            continue;
        }

        // Rows are packed at row * length and sizeof is a multiple of alignof,
        // so aligning the base address keeps every row aligned.
        usz alignment = column.type_info.alignment;
        if (alignment == 0) {
            alignment = alignof(std::max_align_t);
        }
        column.data = this->allocator->reallocate(column.data, new_capacity * length, alignment);
    }

    data.entity_capacity = new_capacity;
}

void Archetype::mark_alive() {
    if (this->alive) {
        return;
    }
    this->alive = true;
    this->world->alive_archetype_count++;
    this->world->dead_archetype_count--;
}

void Archetype::mark_dead() {
    if (!this->alive) {
        return;
    }
    this->alive = false;
    this->died_at = this->world->clock;
    this->world->alive_archetype_count--;
    this->world->dead_archetype_count++;
}

usz Archetype::push_row(const EntityId entity) {
    this->ensure_capacity(this->data.entity_count + 1);
    const usz row = this->data.entity_count++;
    this->data.entities[row] = entity;

    if (row == 0) {
        this->mark_alive();
    }
    return row;
}

void Archetype::insert_entity(World* world, const EntityId entity, EntityRecord* record) {
    (void)world;
    record->archetype = this;
    record->archetype_row = this->push_row(entity);
}

void Archetype::delete_entity(const World* world, const EntityId entity, EntityRecord* record) {
    (void)entity;
    ArchetypeData& data = this->data;
    const usz row = record->archetype_row;
    const usz last_row = data.entity_count - 1;

    if (row != last_row) {
        // Fill the hole with the last row so rows stay packed.
        const EntityId moved_entity = data.entities[last_row];
        data.entities[row] = moved_entity;

        for (usz i = 0; i < data.column_count; i++) {
            ArchetypeColumn& column = data.columns[i];
            if (column.type_info.length == 0) {
                continue;
            }
            column.write(row, column.read(last_row));
        }

        EntityRecord* moved_record = world->entity_index.get_record_any(moved_entity);
        moved_record->archetype_row = row;
    } else {
        if (last_row == 0) {
            this->mark_dead();
        }
    }

    data.entity_count = last_row;
}

void Archetype::move_entity(const World* world, Archetype* destination, const EntityId entity, EntityRecord* record) {
    const usz source_row = record->archetype_row;
    const usz destination_row = destination->push_row(entity);

    // Both types are sorted and column i belongs to type.ids[i], so a single
    // merge walk finds every id the two archetypes share.
    const ArchetypeType& source_type = this->type;
    const ArchetypeType& destination_type = destination->type;
    usz source_index = 0;
    usz destination_index = 0;
    while (source_index < source_type.id_count && destination_index < destination_type.id_count) {
        const Id source_id = source_type.ids[source_index];
        const Id destination_id = destination_type.ids[destination_index];

        if (source_id < destination_id) {
            source_index++;
            continue;
        }
        if (source_id > destination_id) {
            destination_index++;
            continue;
        }

        const ArchetypeColumn& source_column = this->data.columns[source_index];
        ArchetypeColumn& destination_column = destination->data.columns[destination_index];
        if (source_column.type_info.length > 0) {
            destination_column.write(destination_row, source_column.read(source_row));
        }
        source_index++;
        destination_index++;
    }

    // The record still points at the source row, which delete_entity relies on.
    this->delete_entity(world, entity, record);
    record->archetype = destination;
    record->archetype_row = destination_row;
}

Archetype::Archetype(World* world) :
    world(world),
    allocator(world->allocator),
    columns_index(world->allocator),
    columns_map(world->allocator),
    observers(world->allocator),
    forward_edges(world->allocator),
    backwards_edges(world->allocator),
    swapped_edges(world->allocator),
    swapped_backwards_edges(world->allocator)
{}

Archetype* Archetype::create_archetype(World* world, const ArchetypeType archetype_type) {
    // The sparse list only zeroes the slot, so construct the archetype in place.
    const ArchetypeId archetype_id = world->archetypes.new_element();
    Archetype* new_archetype = world->archetypes.get_element_any(archetype_id);
    new (new_archetype) Archetype(world);

    new_archetype->archetype_id = archetype_id;
    new_archetype->type = archetype_type.clone(world->allocator);
    new_archetype->signature = ArchetypeSignature::build(new_archetype->type.ids, new_archetype->type.id_count);

    const usz column_count = archetype_type.id_count;
    if (column_count > 0) {
        ArchetypeData& data = new_archetype->data;
        data.columns = world->allocator->allocate_array<ArchetypeColumn>(column_count);
        data.column_count = column_count;
        new_archetype->records = world->allocator->allocate_array<ComponentRecord*>(column_count);

        // Column i stores type.ids[i]; delete_entity and move_entity rely on
        // that one-to-one order, so tags get a (zero-length) column too.
        for (usz i = 0; i < column_count; i++) {
            const Id id = new_archetype->type.ids[i];
            ComponentRecord* record = ComponentRecord::component_record_ensure(world, id);

            ArchetypeColumn& column = data.columns[i];
            column.data = nullptr;
            // Zero length for tags, which is what every row loop checks.
            column.type_info = record->type_info;
            new_archetype->records[i] = record;

            new_archetype->columns_index.insert(id, i);
            new_archetype->columns_map.insert(id, &column);

            // Let the record reach this archetype and the column holding its id.
            record->link_archetype(new_archetype, i);

            // Ids are sorted and a pair packs its relation in the high bits,
            // so the first (R, t) seen is the lowest t and the first (r, T)
            // the lowest r. Each wildcard record (and this archetype, under
            // the wildcard id) points at that first column only.
            if (ECS::IS_PAIR(id)) {
                const EntityIdLow first = ECS::PAIR_FIRST(id);
                const EntityIdLow second = ECS::PAIR_SECOND(id);
                if (first != ECS::WILDCARD && second != ECS::WILDCARD) {
                    const Id first_wildcard = ECS::PAIR(first, ECS::WILDCARD);
                    const Id second_wildcard = ECS::PAIR(ECS::WILDCARD, second);

                    ComponentRecord* first_record = ComponentRecord::component_record_ensure(world, first_wildcard);
                    if (first_record->link_archetype(new_archetype, i)) {
                        new_archetype->columns_index.insert(first_wildcard, i);
                        new_archetype->columns_map.insert(first_wildcard, &column);
                    }

                    ComponentRecord* second_record = ComponentRecord::component_record_ensure(world, second_wildcard);
                    if (second_record->link_archetype(new_archetype, i)) {
                        new_archetype->columns_index.insert(second_wildcard, i);
                        new_archetype->columns_map.insert(second_wildcard, &column);
                    }
                }
            }
        }

        // ensure_capacity reallocates from nullptr, so this is the first allocation.
        new_archetype->ensure_capacity(ARCHETYPE_INITIAL_CAPACITY);
    } else {
        new_archetype->data.entities = nullptr;
        new_archetype->data.columns = nullptr;
        new_archetype->data.column_count = 0;
        new_archetype->data.entity_count = 0;
        new_archetype->data.entity_capacity = 0;
    }

    // A new archetype is empty, so it starts dead as of now (a cleanup()
    // before its first row reclaims it). The root is alive for good.
    if (column_count > 0) {
        new_archetype->died_at = world->clock;
        world->dead_archetype_count++;
    } else {
        new_archetype->alive = true;
        world->alive_archetype_count++;
    }

    // The key points at the archetype's own cloned ids, not the caller's.
    world->archetype_index.insert(new_archetype->type, new_archetype);
    // Whoever keeps state per archetype (monitors, cached queries) learns
    // about the new table now that it is complete.
    ARCHETYPE_LISTENER::fire(world, new_archetype, ARCHETYPE_CREATED);
    return new_archetype;
}

Archetype* Archetype::ensure_archetype(World *world, ArchetypeType archetype_type) {
    const auto existing = world->archetype_index.find(archetype_type);
    if (existing) {
        return *existing;
    }

    return Archetype::create_archetype(world, archetype_type);
}


Archetype* Archetype::traverse_add(World* world, const Id id) {
    const auto destination_edge = this->forward_edges.find(id);
    if (destination_edge) {
        return *destination_edge;
    }

    TemporalAllocator temp = TemporalAllocator::create();
    const ArchetypeType destination_type = this->type.insert(&temp, id);

    Archetype* destination = Archetype::ensure_archetype(world, destination_type);

    this->forward_edges.insert(id, destination);
    if (destination != this) {
        // `id` was already here: removing it from this archetype does not lead back to itself.
        destination->backwards_edges.insert(id, this);
    }
    return destination;
}

Archetype* Archetype::traverse_swap(World* world, const Id old_id, const Id new_id, const usz column) {
    const auto destination_edge = this->forward_edges.find(new_id);
    if (destination_edge) {
        return *destination_edge;
    }

    // Pairs pack the relation in the high 32 bits, so every (R, *) pair sorts
    // into the same range and an exclusive relation has only one of them: the
    // new pair takes the exact slot of the old one and the type stays sorted.
    TemporalAllocator temp = TemporalAllocator::create();
    const ArchetypeType destination_type = this->type.clone(&temp);
    destination_type.ids[column] = new_id;

    Archetype* destination = Archetype::ensure_archetype(world, destination_type);

    // Forward edge only. The destination still differs from us by `old_id`, so
    // removing `new_id` from it must not come back here; `swapped_edges`
    // remembers which forward edges have no matching backwards edge.
    this->forward_edges.insert(new_id, destination);
    this->swapped_edges.insert(new_id, old_id);
    destination->swapped_backwards_edges.insert(old_id, this);
    return destination;
}

Archetype* Archetype::traverse_remove(World* world, const Id id) {
    const auto destination_edge = this->backwards_edges.find(id);
    if (destination_edge) {
        return *destination_edge;
    }

    TemporalAllocator temp = TemporalAllocator::create();
    const ArchetypeType destination_type = this->type.remove(&temp, id);

    Archetype* destination = Archetype::ensure_archetype(world, destination_type);

    this->backwards_edges.insert(id, destination);
    if (destination != this) {
        // `id` was not here: adding it to this archetype does not lead back to itself.
        destination->forward_edges.insert(id, this);
    }
    return destination;
}

void Archetype::destroy() {
    World* world = this->world;

    // The slot must still be alive and carry the generation in archetype_id,
    // or this object is stale and nothing here can be trusted.
    if (world->archetypes.get_element_alive(this->archetype_id) != this) {
        fprintf(stderr, "[ecs] error: destroying archetype %llu whose slot is dead or recycled\n",
            this->archetype_id);
        return;
    }
    if (this->data.entity_count != 0) {
        fprintf(stderr, "[ecs] error: destroying archetype %llu that still holds %llu entities\n",
            this->archetype_id, this->data.entity_count);
        return;
    }

    // Listeners see the archetype whole, before any of it is unlinked.
    ARCHETYPE_LISTENER::fire(world, this, ARCHETYPE_DESTROYED);

    // --- Edges ---------------------------------------------------------------
    // Forward edge id -> destination. A regular add edge has a matching
    // backwards edge on the destination; a swap edge instead has a reverse
    // link in the destination's swapped_backwards_edges.
    for (auto& edge : this->forward_edges) {
        Archetype* destination = edge.value;
        if (destination == this) {
            continue;
        }
        const Id* old_id = this->swapped_edges.find(edge.key);
        if (old_id != nullptr) {
            Archetype** source = destination->swapped_backwards_edges.find(*old_id);
            if (source != nullptr && *source == this) {
                destination->swapped_backwards_edges.remove(*old_id);
            }
            continue;
        }
        Archetype** backwards = destination->backwards_edges.find(edge.key);
        if (backwards != nullptr && *backwards == this) {
            destination->backwards_edges.remove(edge.key);
        }
    }

    // Backwards edge id -> destination (us without id); it holds the forward edge.
    for (auto& edge : this->backwards_edges) {
        Archetype* destination = edge.value;
        if (destination == this) {
            continue;
        }
        Archetype** forward = destination->forward_edges.find(edge.key);
        if (forward != nullptr && *forward == this) {
            destination->forward_edges.remove(edge.key);
        }
    }

    // Swap sources point here through a forward edge keyed by the id they
    // swapped in. The swap keeps the column, so that id sits in our type at
    // the same index the old id has in the source.
    for (auto& edge : this->swapped_backwards_edges) {
        const Id old_id = edge.key;
        Archetype* source = edge.value;
        const usz* column = source->columns_index.find(old_id);
        if (column == nullptr || *column >= this->type.id_count) {
            continue;
        }
        const Id new_id = this->type.ids[*column];
        Archetype** forward = source->forward_edges.find(new_id);
        if (forward != nullptr && *forward == this) {
            source->forward_edges.remove(new_id);
            source->swapped_edges.remove(new_id);
        }
    }

    // --- Component records ---------------------------------------------------
    // columns_index also holds the wildcard ids linked through link_archetype.
    for (auto& entry : this->columns_index) {
        ComponentRecord* record = ComponentRecord::component_record_find(world, entry.key);
        if (record != nullptr) {
            record->unlink_archetype(this->archetype_id);
        }
    }

    // --- World index ---------------------------------------------------------
    // The key is our own cloned ids, so this has to happen before type.free().
    world->archetype_index.remove(this->type);
    if (world->root_archetype == this) {
        world->root_archetype = nullptr;
    }

    this->free();

    // Leaves whichever count it was in: the dead one, since it is empty,
    // except for the root.
    if (this->alive) {
        world->alive_archetype_count--;
    } else {
        world->dead_archetype_count--;
    }
    this->alive = false;
    world->archetypes.delete_element(this->archetype_id);
}

void Archetype::free() {
    // All nullptr for the root, which allocates nothing.
    ArchetypeData& data = this->data;
    for (usz i = 0; i < data.column_count; i++) {
        this->allocator->free(data.columns[i].data);
    }
    this->allocator->free(data.columns);
    this->allocator->free(data.entities);
    data = ArchetypeData { };
    this->allocator->free(this->records);
    this->records = nullptr;

    this->columns_index.free();
    this->columns_map.free();
    this->observers.monitors.free();
    this->observers.observers.free();
    this->observers.observer_terms.free();
    this->forward_edges.free();
    this->backwards_edges.free();
    this->swapped_edges.free();
    this->swapped_backwards_edges.free();
    this->type.free(this->allocator);
    this->signature = ArchetypeSignature { };
}
