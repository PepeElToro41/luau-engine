#pragma once

#include "engine/defines.hpp"
#include "engine/templates/sparse_list.hpp"

// Shared ECS handle types. An Id is any value that can appear in an archetype
// type: a component, a tag entity, or a pair.
using EntityId = u64;
// The two halves of an EntityId: low 32 bits are the id, high 32 the generation.
using EntityIdLow = u64;
using EntityGeneration = u64;
using ComponentId = u64;
using Id = u64;
using ArchetypeId = SparseId;

// Size and alignment of a component's storage in an archetype column.
struct TypeInfo {
    usz length = 0;
    usz alignment = 0;
};
