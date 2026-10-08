#include "engine/ecs/query_scan.hpp"

#include "engine/ecs/archetype.hpp"
#include "engine/ecs/archetype_candidates.hpp"
#include "engine/ecs/archetype_matcher.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/entity_index.hpp"
#include "engine/ecs/world.hpp"
#include "engine/memory/temporal_allocator.hpp"

#include <cstdio>

namespace QUERY_SCAN {

namespace {

struct ScanState {
    World* world = nullptr;
    QueryTerm* terms = nullptr;
    usz term_count = 0;
    ArchetypeMatcher matcher;

    // The archetypes to test, fixed at begin(). `candidates` is the id list
    // of the with-term record that is linked to the fewest archetypes (see
    // pick_candidates); every match has to be in it, so the matcher only
    // sees that subset. nullptr means no term had a usable record and the
    // walk goes over the world's dense list instead, `archetype_count` being
    // the alive count seen at begin(). `cursor` indexes whichever is used.
    ArchetypeCandidate* candidates = nullptr;
    usz cursor = 0;
    usz archetype_count = 0;
    // The term whose record the candidates came from, or term_count: its
    // column in the current candidate is known without a lookup.
    usz record_term = 0;
    usz candidate_column = 0;
    bool has_candidate_column = false;

    // Chunk buffers the QueryIter points into.
    void** columns = nullptr;
    Id* ids = nullptr;
    EntityId this_var = 0;
};

// Splits the terms into the matcher's with / without lists on `allocator`.
// False, with an error printed, for a term the scan cannot evaluate. The
// scratch lists go on `allocator` too: `allocator` is usually a
// TemporalAllocator, and a nested one here would rewind the arena past the
// matcher's own lists when it went out of scope.
bool build_matcher(const QueryTerm* terms, const usz term_count, BaseAllocator* allocator, ArchetypeMatcher* out) {
    Id* with = allocator->allocate_array<Id>(term_count + 1);
    Id* without = allocator->allocate_array<Id>(term_count + 1);
    usz with_count = 0;
    usz without_count = 0;
    for (usz i = 0; i < term_count; i++) {
        const QueryTerm& term = terms[i];
        if (term.id == 0) {
            fprintf(stderr, "[ecs] error: query term %llu has id 0 (did a type fail to register?)\n", i);
            return false;
        }
        if (!term.is_on_this() || term.is_optional() || term.is_or()) {
            fprintf(stderr, "[ecs] error: query term %llu (%llx) is not a plain term on the matched entity; the uncached scan only supports with() / without() terms\n",
                i, term.id);
            return false;
        }
        if (term.is_excluded()) {
            without[without_count++] = term.id;
        } else {
            with[with_count++] = term.id;
        }
    }
    *out = ArchetypeMatcher::create(allocator, with, with_count, without, without_count);
    return true;
}

// Narrows the walk to the archetypes of one with-term through
// ARCHETYPE_CANDIDATES: the record linked to the fewest archetypes, since the
// result is a subset of every with-term's archetypes. Without-terms never
// narrow anything. Leaves `candidates` nullptr when no with-term has a
// usable record, in which case the walk goes over the dense list.
void pick_candidates(World* world, ScanState* state, BaseAllocator* allocator) {
    Id* with = allocator->allocate_array<Id>(state->term_count + 1);
    usz with_count = 0;
    for (usz i = 0; i < state->term_count; i++) {
        if (!state->terms[i].is_excluded()) {
            with[with_count++] = state->terms[i].id;
        }
    }

    const ArchetypeCandidates candidates = ARCHETYPE_CANDIDATES::collect(world, with, with_count, allocator);
    state->record_term = state->term_count;
    if (candidates.narrowed) {
        state->candidates = candidates.entries;
        state->archetype_count = candidates.count;
        for (usz i = 0; i < state->term_count; i++) {
            const QueryTerm& term = state->terms[i];
            if (!term.is_excluded() && ECS::FOLD_ANY(term.id) == candidates.record_id) {
                state->record_term = i;
                break;
            }
        }
    } else {
        state->candidates = nullptr;
        state->archetype_count = world->archetypes.alive_count;
    }
}

// Column index in `archetype` of the first id matching `pattern`, or
// id_count if none. The column maps answer for concrete ids and for the
// (R, *) / (*, T) aliases; the remaining patterns scan the type.
usz resolve_column(const Archetype* archetype, const Id pattern) {
    const usz* index = archetype->columns_index.find(pattern);
    if (index != nullptr) {
        return *index;
    }
    const ArchetypeType& type = archetype->type;
    for (usz i = 0; i < type.id_count; i++) {
        if (ECS::ID_MATCHES(pattern, type.ids[i])) {
            return i;
        }
    }
    return type.id_count;
}

// Points the iterator at `archetype`: entities, per-term matched ids and
// per-output columns.
void fill_chunk(QueryIter* it, ScanState* state, Archetype* archetype) {
    it->archetype = archetype;
    it->entities = archetype->data.entities;
    it->count = archetype->data.entity_count;

    usz field = 0;
    for (usz i = 0; i < state->term_count; i++) {
        const QueryTerm& term = state->terms[i];
        if (term.is_excluded()) {
            state->ids[i] = 0;
            continue;
        }
        const usz column = i == state->record_term && state->has_candidate_column
            ? state->candidate_column
            : resolve_column(archetype, ECS::FOLD_ANY(term.id));
        if (column == archetype->type.id_count) {
            // The matcher accepted the archetype, so every with-term is held;
            // a column can only be missing if the term list and the matcher
            // disagree, which they never do.
            state->ids[i] = 0;
            if (term.is_output()) {
                state->columns[field++] = nullptr;
            }
            continue;
        }
        state->ids[i] = archetype->type.ids[column];
        if (term.is_output()) {
            // nullptr for a tag column, which stores nothing.
            state->columns[field++] = archetype->data.columns[column].data;
        }
    }
}

bool next(QueryIter* it) {
    ScanState* state = static_cast<ScanState*>(it->state);
    const SparseList<Archetype>& archetypes = state->world->archetypes;

    while (state->cursor < state->archetype_count) {
        Archetype* archetype;
        if (state->candidates != nullptr) {
            // A candidate destroyed during the walk is simply gone.
            const ArchetypeCandidate& candidate = state->candidates[state->cursor];
            archetype = ARCHETYPE_CANDIDATES::archetype_of(state->world, candidate);
            state->cursor++;
            if (archetype == nullptr) {
                continue;
            }
            state->candidate_column = candidate.column;
            state->has_candidate_column = true;
        } else {
            // An archetype destroyed during the walk shrinks the alive span,
            // so the snapshot is a ceiling, not the bound.
            if (state->cursor >= archetypes.alive_count) {
                break;
            }
            archetype = archetypes.get_element_any(archetypes.get_alive_id(state->cursor));
            state->cursor++;
        }
        if (archetype->data.entity_count == 0) {
            // Empty tables (the root always) have nothing to yield.
            continue;
        }
        if (!state->matcher.matches(archetype)) {
            continue;
        }
        fill_chunk(it, state, archetype);
        return true;
    }

    it->archetype = nullptr;
    it->entities = nullptr;
    it->count = 0;
    return false;
}

bool next_nothing(QueryIter* it) {
    it->archetype = nullptr;
    it->entities = nullptr;
    it->count = 0;
    return false;
}

} // namespace

QueryIter begin(World* world, const QueryTerm* terms, const usz term_count, BaseAllocator* allocator) {
    QueryIter it;
    it.world = world;
    it.next = next_nothing;

    ScanState* state = allocator->allocate_array<ScanState>(1);
    *state = ScanState { };
    if (!build_matcher(terms, term_count, allocator, &state->matcher)) {
        return it;
    }

    state->world = world;
    state->term_count = term_count;
    state->terms = allocator->allocate_array<QueryTerm>(term_count + 1);
    state->ids = allocator->allocate_array<Id>(term_count + 1);
    
    usz field_count = 0;
    for (usz i = 0; i < term_count; i++) {
        state->terms[i] = terms[i];
        state->ids[i] = 0;
        if (terms[i].is_output()) {
            field_count++;
        }
    }
    state->columns = allocator->allocate_array<void*>(field_count + 1);
    bool* shared = allocator->allocate_array<bool>(field_count + 1);
    for (usz i = 0; i < field_count; i++) {
        state->columns[i] = nullptr;
        shared[i] = false;
    }
    // Every term is on THIS: no sources, nothing shared.
    EntityId* sources = allocator->allocate_array<EntityId>(term_count + 1);
    for (usz i = 0; i < term_count; i++) {
        sources[i] = 0;
    }
    pick_candidates(world, state, allocator);

    it.columns = state->columns;
    it.shared = shared;
    it.field_count = field_count;
    it.ids = state->ids;
    it.sources = sources;
    it.term_count = term_count;
    it.vars = &state->this_var;
    it.var_count = 1;
    it.next = next;
    it.state = state;
    return it;
}

bool matches(World* world, const QueryTerm* terms, const usz term_count, const EntityId entity) {
    const EntityRecord* record = world->entity_index.get_record_alive(entity);
    if (record == nullptr || record->archetype == nullptr || record->archetype == world->root_archetype) {
        return false;
    }

    TemporalAllocator temp = TemporalAllocator::create();
    ArchetypeMatcher matcher;
    if (!build_matcher(terms, term_count, &temp, &matcher)) {
        return false;
    }
    return matcher.matches(record->archetype);
}

} // namespace QUERY_SCAN
