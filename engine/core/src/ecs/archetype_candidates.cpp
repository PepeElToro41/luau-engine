#include "engine/ecs/archetype_candidates.hpp"

#include "engine/ecs/component_record.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/world.hpp"

namespace ARCHETYPE_CANDIDATES {

namespace {

// Whether `pattern` (ANY-folded) has a ComponentRecord whose columns_index
// lists every archetype holding a matching id. WILDCARD and (*, *) match
// whole classes of ids and have none.
bool pattern_has_record(const Id pattern) {
    if (!ECS::IS_PAIR(pattern)) {
        return !ECS::IS_WILDCARD(pattern);
    }
    return !(ECS::IS_WILDCARD(ECS::PAIR_FIRST(pattern)) && ECS::IS_WILDCARD(ECS::PAIR_SECOND(pattern)));
}

ArchetypeCandidates copy_record(const ComponentRecord* record, BaseAllocator* allocator) {
    ArchetypeCandidates out;
    out.narrowed = true;
    out.record_id = record->id;
    out.ids = allocator->allocate_array<ArchetypeId>(record->columns_index.count + 1);
    out.count = 0;
    for (const auto& entry : record->columns_index) {
        out.ids[out.count++] = entry.key;
    }
    return out;
}

// A narrowed list with nothing in it, for an id no archetype holds.
ArchetypeCandidates empty(const Id record_id, BaseAllocator* allocator) {
    ArchetypeCandidates out;
    out.narrowed = true;
    out.record_id = record_id;
    out.ids = allocator->allocate_array<ArchetypeId>(1);
    out.count = 0;
    return out;
}

} // namespace

ArchetypeCandidates collect(World* world, const Id* with, const usz with_count, BaseAllocator* allocator) {
    const ComponentRecord* best = nullptr;

    for (usz i = 0; i < with_count; i++) {
        if (with[i] == 0) {
            continue;
        }
        const Id pattern = ECS::FOLD_ANY(with[i]);
        if (!pattern_has_record(pattern)) {
            continue;
        }
        const ComponentRecord* record = ComponentRecord::component_record_find(world, pattern);
        if (record == nullptr) {
            return empty(pattern, allocator);
        }
        if (best == nullptr || record->archetype_count < best->archetype_count) {
            best = record;
        }
    }

    if (best == nullptr) {
        return ArchetypeCandidates { };
    }
    return copy_record(best, allocator);
}

ArchetypeCandidates collect_for(World* world, const Id record_id, BaseAllocator* allocator) {
    if (record_id == 0) {
        return ArchetypeCandidates { };
    }
    const ComponentRecord* record = ComponentRecord::component_record_find(world, record_id);
    if (record == nullptr) {
        return empty(record_id, allocator);
    }
    return copy_record(record, allocator);
}

} // namespace ARCHETYPE_CANDIDATES
