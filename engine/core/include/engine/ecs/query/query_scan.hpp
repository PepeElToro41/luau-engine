#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/ecs/query/query_iter.hpp"
#include "engine/ecs/query/query_term.hpp"
#include "engine/memory/base_allocator.hpp"

struct World;

// The uncached producer behind Query<Ts...>: a QueryIter that walks a list
// of candidate archetypes, tests each against an ArchetypeMatcher built from
// the terms, and resolves the output columns of the ones that match as it
// goes. The candidates come from the with-term whose ComponentRecord is
// linked to the fewest archetypes: every match holds that id, so its record
// already lists a superset of the result and the rest of the world is never
// looked at. Only a term with no record of its own (WILDCARD, (*, *)) falls
// back to the whole archetype list. Nothing is precomputed or kept between
// runs, so a query built on it stays a plain value; the price is one matcher
// test per candidate per run, which the signature prefilter keeps cheap.
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
// during the walk are not visited, and the visit order is unspecified. An entity that moves into an archetype
// not visited yet is seen again, and one that moves out of the current chunk
// invalidates the chunk's pointers, so structural changes during a walk are
// best avoided until deferred operations exist. The root archetype stores
// no rows and is never visited, so an entity with no ids never matches, not
// even a query with no terms.
namespace QUERY_SCAN {

QueryIter begin(World* world, const QueryTerm* terms, usz term_count, BaseAllocator* allocator);

// Whether `entity` is in the result set: alive, outside the root, and in an
// archetype that passes the same test begin() applies. Uses scratch memory
// only.
bool matches(World* world, const QueryTerm* terms, usz term_count, EntityId entity);

} // namespace QUERY_SCAN
