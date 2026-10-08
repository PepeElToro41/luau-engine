#include "engine/ecs/query/query_scan.hpp"

#include "engine/ecs/archetype/archetype.hpp"
#include "engine/ecs/archetype/archetype_candidates.hpp"
#include "engine/ecs/archetype/archetype_listener.hpp"
#include "engine/ecs/archetype/archetype_matcher.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/entity_index.hpp"
#include "engine/ecs/world.hpp"
#include "engine/memory/temporal_allocator.hpp"

#include <cstdio>
#include <new>

namespace QUERY_SCAN {

namespace {

// --- Shared -----------------------------------------------------------------

// The terms split into the matcher's two sides, on whatever allocator
// split_terms was given.
struct TermSplit {
    Id* with = nullptr;
    usz with_count = 0;
    Id* without = nullptr;
    usz without_count = 0;
};

// Splits the terms into with / without lists on `allocator`. False, with an
// error printed, for a term neither producer can evaluate. The lists go on
// `allocator` rather than a nested TemporalAllocator because the caller's
// allocator is usually one itself, and a nested one here would rewind the
// arena past the matcher's own lists when it went out of scope.
bool split_terms(const QueryTerm* terms, const usz term_count, BaseAllocator* allocator, TermSplit* out) {
    out->with = allocator->allocate_array<Id>(term_count + 1);
    out->without = allocator->allocate_array<Id>(term_count + 1);
    out->with_count = 0;
    out->without_count = 0;
    for (usz i = 0; i < term_count; i++) {
        const QueryTerm& term = terms[i];
        if (term.id == 0) {
            fprintf(stderr, "[ecs] error: query term %llu has id 0 (did a type fail to register?)\n", i);
            return false;
        }
        if (!term.is_on_this() || term.is_optional() || term.is_or()) {
            fprintf(stderr, "[ecs] error: query term %llu (%llx) is not a plain term on the matched entity; the trivial query only supports with() / without() terms\n",
                i, term.id);
            return false;
        }
        if (term.is_excluded()) {
            out->without[out->without_count++] = term.id;
        } else {
            out->with[out->with_count++] = term.id;
        }
    }
    return true;
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

// Writes a chunk's per-term id and per-output column pointer for `archetype`
// given the column of each term in it (id_count for none).
void fill_chunk(QueryIter* it, const QueryTerm* terms, const usz term_count, const usz* columns, Id* ids, void** fields, Archetype* archetype) {
    it->archetype = archetype;
    it->entities = archetype->data.entities;
    it->count = archetype->data.entity_count;

    usz field = 0;
    for (usz i = 0; i < term_count; i++) {
        const usz column = columns[i];
        if (column == archetype->type.id_count) {
            // Excluded terms, and the never-taken case of an accepted
            // archetype missing a with-term: the matcher and the term list
            // never disagree.
            ids[i] = 0;
            if (terms[i].is_output()) {
                fields[field++] = nullptr;
            }
            continue;
        }
        ids[i] = archetype->type.ids[column];
        if (terms[i].is_output()) {
            // nullptr for a tag column, which stores nothing.
            fields[field++] = archetype->data.columns[column].data;
        }
    }
}

// Resolves the column of every term in `archetype` into `out` (term_count
// entries): excluded terms get id_count, the rest the first matching column.
void resolve_columns(const QueryTerm* terms, const usz term_count, const Archetype* archetype, usz* out) {
    for (usz i = 0; i < term_count; i++) {
        out[i] = terms[i].is_excluded()
            ? archetype->type.id_count
            : resolve_column(archetype, ECS::FOLD_ANY(terms[i].id));
    }
}

bool next_nothing(QueryIter* it) {
    it->archetype = nullptr;
    it->entities = nullptr;
    it->count = 0;
    return false;
}

// Everything a QueryIter over trivial terms needs besides its producer: the
// chunk buffers on `allocator`, no sources, one variable (THIS).
void setup_iter(QueryIter* it, World* world, const QueryTerm* terms, const usz term_count, BaseAllocator* allocator, void* state, bool (*next)(QueryIter*), EntityId* this_var) {
    usz field_count = 0;
    for (usz i = 0; i < term_count; i++) {
        if (terms[i].is_output()) {
            field_count++;
        }
    }
    Id* ids = allocator->allocate_array<Id>(term_count + 1);
    EntityId* sources = allocator->allocate_array<EntityId>(term_count + 1);
    for (usz i = 0; i < term_count; i++) {
        ids[i] = 0;
        sources[i] = 0;
    }
    void** columns = allocator->allocate_array<void*>(field_count + 1);
    bool* shared = allocator->allocate_array<bool>(field_count + 1);
    for (usz i = 0; i < field_count; i++) {
        columns[i] = nullptr;
        shared[i] = false;
    }

    it->world = world;
    it->columns = columns;
    it->shared = shared;
    it->field_count = field_count;
    it->ids = ids;
    it->sources = sources;
    it->term_count = term_count;
    it->vars = this_var;
    it->var_count = 1;
    it->next = next;
    it->state = state;
}

// --- Scan --------------------------------------------------------------------

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

    // Per-term column scratch for the current chunk.
    usz* columns = nullptr;
    EntityId this_var = 0;
};

// Narrows the walk to the archetypes of one with-term through
// ARCHETYPE_CANDIDATES: the record linked to the fewest archetypes, since the
// result is a subset of every with-term's archetypes. Without-terms never
// narrow anything. Leaves `candidates` nullptr when no with-term has a
// usable record, in which case the walk goes over the dense list.
void pick_candidates(World* world, ScanState* state, const TermSplit& split, BaseAllocator* allocator) {
    const ArchetypeCandidates candidates = ARCHETYPE_CANDIDATES::collect(world, split.with, split.with_count, allocator);
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

bool next_scan(QueryIter* it) {
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
        for (usz i = 0; i < state->term_count; i++) {
            const QueryTerm& term = state->terms[i];
            if (i == state->record_term && state->has_candidate_column) {
                state->columns[i] = state->candidate_column;
            } else if (term.is_excluded()) {
                state->columns[i] = archetype->type.id_count;
            } else {
                state->columns[i] = resolve_column(archetype, ECS::FOLD_ANY(term.id));
            }
        }
        fill_chunk(it, state->terms, state->term_count, state->columns, it->ids, it->columns, archetype);
        return true;
    }

    return next_nothing(it);
}

// --- Cache -------------------------------------------------------------------

struct CacheState {
    QueryScanCache* cache = nullptr;
    usz cursor = 0;
    // The match count at begin(): archetypes cached during the walk are not
    // visited, like the scan's snapshot.
    usz limit = 0;
    EntityId this_var = 0;
};

// Appends `archetype` with its resolved columns. The column array grows
// geometrically like the match list (resize() alone reserves exactly what
// is asked, which would copy it on every add).
void cache_add(QueryScanCache* cache, Archetype* archetype) {
    cache->matches.push(QueryScanMatch { archetype->archetype_id, archetype });
    const usz offset = cache->columns.count;
    const usz needed = offset + cache->term_count;
    if (needed > cache->columns.capacity) {
        const usz doubled = cache->columns.capacity * 2;
        cache->columns.reserve(doubled > needed ? doubled : needed);
    }
    cache->columns.resize(needed);
    resolve_columns(cache->terms, cache->term_count, archetype, cache->columns.data + offset);
}

// Drops `archetype` if it is cached: the last match and its column slice
// take its place.
void cache_remove(QueryScanCache* cache, const Archetype* archetype) {
    for (usz i = 0; i < cache->matches.count; i++) {
        if (cache->matches[i].archetype != archetype) {
            continue;
        }
        const usz last = cache->matches.count - 1;
        const usz term_count = cache->term_count;
        if (i != last) {
            cache->matches[i] = cache->matches[last];
            for (usz t = 0; t < term_count; t++) {
                cache->columns[i * term_count + t] = cache->columns[last * term_count + t];
            }
        }
        cache->matches.resize(last);
        cache->columns.resize(last * term_count);
        return;
    }
}

// The cache's archetype listener, under the first `with` id (every match
// holds it) or WILDCARD. `user_data` is the cache itself, which never moves.
void on_archetype(World* world, Archetype* archetype, const ArchetypeEvent event, void* user_data) {
    QueryScanCache* cache = static_cast<QueryScanCache*>(user_data);
    if (event == ARCHETYPE_CREATED) {
        if (archetype != world->root_archetype && cache->matcher.matches(archetype)) {
            cache_add(cache, archetype);
        }
        return;
    }
    cache_remove(cache, archetype);
}

bool next_cached(QueryIter* it) {
    CacheState* state = static_cast<CacheState*>(it->state);
    QueryScanCache* cache = state->cache;

    // A match removed during the walk shrinks the list under the snapshot.
    while (state->cursor < state->limit && state->cursor < cache->matches.count) {
        const QueryScanMatch& match = cache->matches[state->cursor];
        const usz index = state->cursor;
        state->cursor++;
        if (cache->world->archetypes.get_element_alive(match.id) != match.archetype) {
            // Destroyed since begin() without the listener hearing of it,
            // which World::free is the only way to do; never followed.
            continue;
        }
        if (match.archetype->data.entity_count == 0) {
            continue;
        }
        fill_chunk(it, cache->terms, cache->term_count, cache->columns.data + index * cache->term_count, it->ids, it->columns, match.archetype);
        return true;
    }

    return next_nothing(it);
}

} // namespace

// --- Scan --------------------------------------------------------------------

QueryIter begin(World* world, const QueryTerm* terms, const usz term_count, BaseAllocator* allocator) {
    QueryIter it;
    it.world = world;
    it.next = next_nothing;

    ScanState* state = allocator->allocate_array<ScanState>(1);
    *state = ScanState { };
    TermSplit split;
    if (!split_terms(terms, term_count, allocator, &split)) {
        return it;
    }
    state->matcher = ArchetypeMatcher::create(allocator, split.with, split.with_count, split.without, split.without_count);

    state->world = world;
    state->term_count = term_count;
    state->terms = allocator->allocate_array<QueryTerm>(term_count + 1);
    state->columns = allocator->allocate_array<usz>(term_count + 1);
    for (usz i = 0; i < term_count; i++) {
        state->terms[i] = terms[i];
        state->columns[i] = 0;
    }
    pick_candidates(world, state, split, allocator);

    setup_iter(&it, world, state->terms, term_count, allocator, state, next_scan, &state->this_var);
    return it;
}

bool matches(World* world, const QueryTerm* terms, const usz term_count, const EntityId entity) {
    const EntityRecord* record = world->entity_index.get_record_alive(entity);
    if (record == nullptr || record->archetype == nullptr || record->archetype == world->root_archetype) {
        return false;
    }

    TemporalAllocator temp = TemporalAllocator::create();
    TermSplit split;
    if (!split_terms(terms, term_count, &temp, &split)) {
        return false;
    }
    const ArchetypeMatcher matcher = ArchetypeMatcher::create(&temp, split.with, split.with_count, split.without, split.without_count);
    return matcher.matches(record->archetype);
}

// --- Cache -------------------------------------------------------------------

QueryScanCache* create_cache(World* world, const QueryTerm* terms, const usz term_count, BaseAllocator* allocator) {
    TemporalAllocator temp = TemporalAllocator::create();
    TermSplit split;
    if (!split_terms(terms, term_count, &temp, &split)) {
        QueryScanCache* cache = new (allocator->allocate_array<QueryScanCache>(1)) QueryScanCache(allocator);
        cache->world = world;
        return cache;
    }
    return create_cache(world, split.with, split.with_count, split.without, split.without_count, terms, term_count, allocator);
}

QueryScanCache* create_cache(World* world, const Id* with, const usz with_count, const Id* without, const usz without_count, const QueryTerm* terms, const usz term_count, BaseAllocator* allocator) {
    QueryScanCache* cache = new (allocator->allocate_array<QueryScanCache>(1)) QueryScanCache(allocator);
    cache->world = world;
    cache->ok = true;

    cache->terms = allocator->allocate_array<QueryTerm>(term_count + 1);
    cache->term_count = term_count;
    for (usz i = 0; i < term_count; i++) {
        cache->terms[i] = terms[i];
        if (terms[i].is_output()) {
            cache->field_count++;
        }
    }
    cache->matcher = ArchetypeMatcher::create(allocator, with, with_count, without, without_count);
    // Any archetype the matcher accepts holds every `with` id, so listening
    // under the first one loses nothing and skips unrelated tables.
    cache->listener = ARCHETYPE_LISTENER::add(world, ARCHETYPE_LISTENER::key_for(with, with_count), on_archetype, cache);

    // Only the archetypes holding the rarest `with` id can match; the rest
    // of the world is never tested.
    TemporalAllocator temp = TemporalAllocator::create();
    const ArchetypeCandidates candidates = ARCHETYPE_CANDIDATES::collect(world, with, with_count, &temp);
    if (candidates.narrowed) {
        // Most candidates usually pass: size the arrays for all of them once.
        cache->matches.reserve(candidates.count);
        cache->columns.reserve(candidates.count * term_count);
        for (usz i = 0; i < candidates.count; i++) {
            Archetype* archetype = ARCHETYPE_CANDIDATES::archetype_of(world, candidates.entries[i]);
            if (archetype != nullptr && cache->matcher.matches(archetype)) {
                cache_add(cache, archetype);
            }
        }
        return cache;
    }
    for (usz i = 0; i < world->archetypes.alive_count; i++) {
        Archetype* archetype = world->archetypes.get_element_any(world->archetypes.get_alive_id(i));
        if (archetype == world->root_archetype) {
            continue;
        }
        if (cache->matcher.matches(archetype)) {
            cache_add(cache, archetype);
        }
    }
    return cache;
}

void destroy_cache(QueryScanCache* cache) {
    if (cache == nullptr) {
        return;
    }
    if (cache->listener != 0) {
        ARCHETYPE_LISTENER::remove(cache->world, cache->listener);
    }
    if (cache->ok) {
        cache->matcher.free();
    }
    cache->matches.free();
    cache->columns.free();
    BaseAllocator* allocator = cache->allocator;
    allocator->free(cache->terms);
    allocator->free(cache);
}

QueryIter begin(QueryScanCache* cache, BaseAllocator* allocator) {
    QueryIter it;
    it.world = cache->world;
    it.next = next_nothing;
    if (!cache->ok) {
        return it;
    }

    CacheState* state = allocator->allocate_array<CacheState>(1);
    *state = CacheState { };
    state->cache = cache;
    state->limit = cache->matches.count;

    setup_iter(&it, cache->world, cache->terms, cache->term_count, allocator, state, next_cached, &state->this_var);
    return it;
}

bool matches(const QueryScanCache* cache, const EntityId entity) {
    if (!cache->ok) {
        return false;
    }
    World* world = cache->world;
    const EntityRecord* record = world->entity_index.get_record_alive(entity);
    if (record == nullptr || record->archetype == nullptr || record->archetype == world->root_archetype) {
        return false;
    }
    return cache->matcher.matches(record->archetype);
}

} // namespace QUERY_SCAN
