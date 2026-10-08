#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/memory/base_allocator.hpp"

struct Archetype;
struct World;

// Narrows "every archetype" down to the ones that can hold all of a
// matcher's `with` ids, so the matcher is only run on those.
//
// Every match holds each with id, so the ComponentRecord of any of them
// already lists a superset of the result in its columns_index. The id whose
// record is linked to the fewest archetypes gives the smallest superset and
// is the one walked. Concrete ids, concrete pairs and the (R, *) / (*, T)
// aliases all have such a record; WILDCARD, ANY and (*, *) match whole
// classes of ids and have none, so they never narrow. A with id with no
// record at all is held by no archetype, and the result is empty.
//
//     TemporalAllocator temp = TemporalAllocator::create();
//     ArchetypeCandidates candidates = ARCHETYPE_CANDIDATES::collect(world, with, with_count, &temp);
//     if (!candidates.narrowed) { /* walk world->archetypes */ }
//     for (usz i = 0; i < candidates.count; i++) {
//         Archetype* archetype = ARCHETYPE_CANDIDATES::archetype_of(world, candidates.entries[i]);
//         if (archetype != nullptr && matcher.matches(archetype)) { ... }
//     }

// One archetype of the walked record, copied from its RecordColumn: the
// pointer and the column of the record's id in it, so neither needs a
// lookup, plus the id to tell whether the archetype still exists (the list
// is a snapshot; see archetype_of).
struct ArchetypeCandidate {
    ArchetypeId id = 0;
    Archetype* archetype = nullptr;
    usz column = 0;
};

struct ArchetypeCandidates {
    // Whether `ids` is the list to walk. False when no with id has a usable
    // record (none given, or wildcards only): every archetype has to be tested.
    bool narrowed = false;
    // The with id whose record was walked (ANY folded to WILDCARD), 0 when
    // not narrowed. Walking its record again later gives the archetypes a
    // matcher over the same with ids accepted since, which is what a
    // teardown needs; see ARCHETYPE_CANDIDATES::collect_for.
    Id record_id = 0;
    // The record's archetypes copied at collect() time, in no particular
    // order, so the list is unaffected by archetypes created or destroyed
    // afterwards. Go through archetype_of() to use one.
    ArchetypeCandidate* entries = nullptr;
    usz count = 0;
};

namespace ARCHETYPE_CANDIDATES {

// Picks the narrowest record among the (non-zero) `with` ids and copies its
// archetype ids onto `allocator`. Nothing has to be freed.
ArchetypeCandidates collect(World* world, const Id* with, usz with_count, BaseAllocator* allocator);

// The archetypes currently linked to the record of `record_id` (a value
// collect() returned), copied onto `allocator`. Not narrowed for 0; an
// empty, narrowed list when the record no longer exists, since then no
// archetype holds the id.
ArchetypeCandidates collect_for(World* world, Id record_id, BaseAllocator* allocator);

// The candidate's archetype, or nullptr if it was destroyed since the list
// was collected (its id is no longer alive in World::archetypes).
Archetype* archetype_of(const World* world, const ArchetypeCandidate& candidate);

} // namespace ARCHETYPE_CANDIDATES
