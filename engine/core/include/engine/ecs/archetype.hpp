#pragma once

#include "engine/defines.hpp"
#include "engine/templates/hash_map.hpp"
#include "engine/ecs/archetype_signature.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/memory/base_allocator.hpp"

#include <cstring>
#include <functional>

struct World;
struct EntityRecord;

// The set of component ids that defines an archetype. `ids` is a view over an
// array that must be kept sorted ascending: equality relies on it so the
// comparison is a single pass, and it keeps the hash order-independent.
struct ArchetypeType {
    u64* ids = nullptr;
    usz id_count = 0;

    ArchetypeType() = default;
    ArchetypeType(u64* ids_array, const usz count) : ids(ids_array), id_count(count) {}

    ArchetypeType clone(BaseAllocator* allocator, usz reserve = 0) const;
    ArchetypeType insert(BaseAllocator* allocator, Id id) const;
    ArchetypeType remove(BaseAllocator* allocator, Id id) const;
    void free(BaseAllocator* allocator);

    u64 hash() const {
        u64 hashed_number = 2166136261u;

        for (usz i = 0; i < this->id_count; i++) {
            hashed_number ^= this->ids[i];
            hashed_number *= 16777619u;
        }
        return hashed_number;
    }

    // Both types are the same set of ids. Assumes both id arrays are sorted.
    bool equals(const ArchetypeType& other) const {
        if (this->id_count != other.id_count) {
            return false;
        }
        for (usz i = 0; i < this->id_count; i++) {
            if (this->ids[i] != other.ids[i]) {
                return false;
            }
        }
        return true;
    }

    bool operator==(const ArchetypeType& other) const { return this->equals(other); }
    bool operator!=(const ArchetypeType& other) const { return !this->equals(other); }
};

// Lets ArchetypeType be used as a HashMap key with the default hasher.
template <>
struct std::hash<ArchetypeType> {
    usz operator()(const ArchetypeType& type) const noexcept { return type.hash(); }
};


struct ArchetypeColumn {
    void* data;
    TypeInfo type_info;

    // linter bugs and thinks it should be marked as const
    // ReSharper disable once CppMemberFunctionMayBeConst
    void write(const usz row, const void* new_data) {
        const usz length = this->type_info.length;
        char* column_data = static_cast<char*>(this->data);
        std::memcpy(&column_data[row * length], new_data, length);
    }
    void* read(const usz row) const {
        const usz length = this->type_info.length;
        char* column_data = static_cast<char*>(this->data);
        return &column_data[row * length];
    }
};

// Capacity every archetype starts with; growth doubles from there, so the
// capacity is always a power of two.
constexpr usz ARCHETYPE_INITIAL_CAPACITY = 16;

// Row storage of an archetype: one entity id per row plus one column per
// component id with data. Every column holds `entity_capacity` rows.
struct ArchetypeData {
    EntityId* entities;
    ArchetypeColumn* columns;

    usz column_count;
    usz entity_count;
    usz entity_capacity;
};

// A table of entities that all share the same ArchetypeType.
struct Archetype {
    World* world;
    BaseAllocator* allocator;

    HashMap<Id, usz> columns_index;
    HashMap<Id, ArchetypeColumn*> columns_map;

    bool alive = false;
    ArchetypeData data { };

    ArchetypeId archetype_id = 0;
    ArchetypeType type { };
    // Mask + bloom summary of `type`, built by create_archetype and read by
    // ArchetypeMatcher so most archetypes are accepted or rejected without
    // scanning the ids.
    ArchetypeSignature signature { };

    HashMap<Id, Archetype*> forward_edges;
    HashMap<Id, Archetype*> backwards_edges;
    // for exclusive relationships, used to removed forward edges in the swapped pairs, cause a swapped pair will only have forward edges
    HashMap<Id, Id> swapped_edges;
    // Reverse of swapped_edges, kept on the destination: old id (the pair the
    // source still holds) -> source archetype whose swap forward edge leads
    // here. Lets destroy() drop those forward edges, which have no backwards
    // edge to find them through.
    HashMap<Id, Archetype*> swapped_backwards_edges;

    // Builds the archetype for `archetype_type` on the world's allocator: the
    // type is cloned (it usually comes from a TemporalAllocator), one column
    // per id is set up and the initial rows are allocated. An empty type is the
    // root archetype and stores nothing. The id is set by create_archetype.
    explicit Archetype(World* world);

    ArchetypeColumn* get_column(const Id id) const {
        const auto column = this->columns_map.find(id);
        if (column) {
            return *column;
        }
        return nullptr;
    }
    bool contains(const Id id) const {
        return this->columns_map.contains(id);
    }

    // Grows the entity array and every column so at least `capacity` rows fit.
    // Capacity starts at ARCHETYPE_INITIAL_CAPACITY, doubles on growth, and is
    // always a power of two. Never shrinks.
    void ensure_capacity(usz capacity);

    // `alive` tracks whether the archetype holds any entity row. push_row sets
    // it on the first row and delete_entity clears it on the last.
    void mark_alive();
    void mark_dead();

    // Appends `entity` as a new row and points its record (archetype and
    // row) at it. Component data for the row is left uninitialized for the
    // caller to write.
    void insert_entity(World* world, EntityId entity, EntityRecord* record);
    // Removes the entity's row. The last row is swapped into the hole, and
    // that entity's record is updated to its new row.
    void delete_entity(const World* world, EntityId entity, EntityRecord* record);
    // Moves the entity from this archetype to `destination`, copying every
    // component both types share, then removes its row here and points the
    // record at the destination.
    void move_entity(const World* world, Archetype* destination, EntityId entity, EntityRecord* record);

    // Archetype with `id` added. Creates the add/remove edge pair on first use.
    Archetype* traverse_add(World* world, Id id);
    // Archetype with `old_id` (at index `column` of this type) replaced by
    // `new_id`, for exclusive relationships. Only a forward edge is recorded:
    // removing `new_id` from the destination does not lead back here.
    Archetype* traverse_swap(World* world, Id old_id, Id new_id, usz column);
    // Archetype with `id` removed. Creates the remove/add edge pair on first use.
    Archetype* traverse_remove(World* world, Id id);

    static Archetype* create_archetype(World* world, ArchetypeType archetype_type);
    static Archetype* ensure_archetype(World* world, ArchetypeType archetype_type);

    // Tears the archetype down: unlinks every edge into and out of it, removes
    // it from each component record and from the world's archetype index,
    // releases its storage and type, and frees its slot in the world's
    // archetype list. The archetype must be empty (entity_count == 0) and its
    // slot must still be alive under archetype_id; otherwise an error is
    // printed and nothing is touched. The root goes through the same path:
    // it has no rows or columns, so only its edges, maps and type are released.
    void destroy();

    // Releases only what this archetype owns: row storage, column and edge
    // maps, and the cloned type. Nothing else is told, so neighbouring
    // archetypes, component records and the world index keep dangling
    // pointers to it. destroy() calls this after unlinking; World::free()
    // calls it directly on every archetype since the whole graph goes away.
    void free();

private:
    // Grows if needed and appends `entity` as a new row, returning the row.
    usz push_row(EntityId entity);
};
