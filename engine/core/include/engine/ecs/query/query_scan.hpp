#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/archetype/archetype_listener.hpp"
#include "engine/ecs/archetype/archetype_matcher.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/ecs/query/query_iter.hpp"
#include "engine/ecs/query/query_term.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"

struct Archetype;
struct World;

// The two producers behind Query<Ts...>: the scan, which tests the world's
// archetypes on every run, and the cache, which keeps the matched ones.
//
// The scan is a QueryIter that walks a list of candidate archetypes, tests
// each against an ArchetypeMatcher built from the terms, and resolves the
// output columns of the ones that match as it goes. The candidates come from
// the with-term whose ComponentRecord is linked to the fewest archetypes:
// every match holds that id, so its record already lists a superset of the
// result and the rest of the world is never looked at. Only a term with no
// record of its own (WILDCARD, (*, *)) falls back to the whole archetype
// list. Nothing is precomputed or kept between runs, so a query built on it
// stays a plain value; the price is one matcher test per candidate per run,
// which the signature prefilter keeps cheap.
//
// Only the trivial query shape is accepted: every term on THIS, no
// optionals, or-terms or traversal (those are DynamicQuery's). Wildcard
// patterns are fine: an output term (Likes, *) yields the column of the first
// matching pair in each archetype, and QueryIter::ids reports which. A term
// with id 0 or an unsupported shape prints an error and yields nothing.
//
// Everything the iterator needs, the terms included, is copied onto
// `allocator`, which must outlive the iteration; nothing is freed
// explicitly. The intended allocator is a TemporalAllocator the caller holds:
//
//     TemporalAllocator temp = TemporalAllocator::create();
//     QueryIter it = QUERY_SCAN::begin(&world, terms, count, &temp);
//     while (it.next(&it)) { ... }
//
// The set of archetypes is fixed when begin() is called: archetypes created
// during the walk are not visited, and the visit order is unspecified. An
// entity that moves into an archetype not visited yet is seen again, and one
// that moves out of the current chunk invalidates the chunk's pointers, so
// structural changes during a walk are best avoided until deferred
// operations exist. The root archetype stores
// no rows and is never visited, so an entity with no ids never matches, not
// even a query with no terms.
//
// The cache (QueryScanCache, behind QUERY_CACHED) is the same test done once
// per archetype instead of once per run: create_cache() tests the archetypes
// that exist and keeps the ones that pass, an archetype listener (see
// archetype_listener.hpp) under the first `with` id tests each archetype
// created afterwards and drops the ones destroyed, and begin(cache) just
// walks the list. The column of every term in every matched archetype is
// resolved when the archetype enters the list, since an archetype's type
// never changes, so a cached run does no matcher test and no column lookup
// per chunk. Empty archetypes stay in the list (whether a table has rows
// changes all the time) and are skipped at iteration like the scan does.
// Everything the cache owns is on the allocator given to create_cache();
// destroy_cache() releases it and unregisters the listener, and must run
// before World::free (which frees the listener lists but knows nothing of
// the cache). Same walk guarantees as the scan: the matches are snapshotted
// at begin(), and structural changes during a walk are best avoided.
//
// A DynamicQuery (see dynamic_query.hpp) caches through the same type: its
// program's matcher ids build the matcher, its plain THIS terms are the
// terms whose columns are kept, and the query VM walks the matches in
// SELECT's place (QUERY_VM::begin with a cache) while still evaluating the
// rest of the program per archetype.

// One matched archetype. The columns of the terms in it sit in
// QueryScanCache::columns at index * term_count.
struct QueryScanMatch {
    // Checked against World::archetypes before the archetype is touched, so
    // a pointer that outlived its table is never followed.
    ArchetypeId id = 0;
    Archetype* archetype = nullptr;
};

struct QueryScanCache {
    World* world = nullptr;
    // Owns the terms, the matcher's lists, the arrays and the cache itself.
    BaseAllocator* allocator = nullptr;
    // False when a term was rejected at create_cache() (error printed): the
    // cache then matches nothing and has no listener.
    bool ok = false;

    // The terms whose columns are kept per match. For a cache built from a
    // term list they are the whole query; for one built from id lists they
    // are whatever the caller asked for and may include optional terms.
    QueryTerm* terms = nullptr;
    usz term_count = 0;
    // Terms with TERM_OUTPUT: the size of a chunk's column list.
    usz field_count = 0;

    ArchetypeMatcher matcher;
    ArchetypeListenerId listener = 0;

    DynamicArray<QueryScanMatch> matches;
    // term_count entries per match: the column index of each term in the
    // archetype, or the archetype's id count for an excluded term.
    DynamicArray<usz> columns;

    explicit QueryScanCache(BaseAllocator* allocator) : allocator(allocator), matches(allocator), columns(allocator) {}
};

namespace QUERY_SCAN {

// --- Scan --------------------------------------------------------------------

QueryIter begin(World* world, const QueryTerm* terms, usz term_count, BaseAllocator* allocator);

// Whether `entity` is in the result set: alive, outside the root, and in an
// archetype that passes the same test begin() applies. Uses scratch memory
// only.
bool matches(World* world, const QueryTerm* terms, usz term_count, EntityId entity);

// --- Cache -------------------------------------------------------------------

// Builds the cache for `terms` on `allocator`, filled with every archetype
// that matches now and listening for the ones to come. Always returns a
// cache; one whose terms were rejected has `ok` false and yields nothing.
QueryScanCache* create_cache(World* world, const QueryTerm* terms, usz term_count, BaseAllocator* allocator);
// The same with the matcher given as id lists (as a QueryProgram keeps
// them) and `terms` only naming the columns to keep per match, excluded
// terms getting the id-count sentinel and optional ones the column or the
// sentinel. Nothing is validated: the ids are the caller's matcher.
QueryScanCache* create_cache(World* world, const Id* with, usz with_count, const Id* without, usz without_count, const QueryTerm* terms, usz term_count, BaseAllocator* allocator);
// Unregisters the listener and releases everything, the cache included.
// nullptr is a no-op.
void destroy_cache(QueryScanCache* cache);

// A cursor over the cached matches, built on `allocator` like the scan's.
QueryIter begin(QueryScanCache* cache, BaseAllocator* allocator);
// Whether `entity` is in the result set now, through the cache's matcher.
bool matches(const QueryScanCache* cache, EntityId entity);

} // namespace QUERY_SCAN
