#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/ecs/query/query_iter.hpp"
#include "engine/ecs/query/query_program.hpp"
#include "engine/ecs/query/query_scan.hpp"
#include "engine/memory/base_allocator.hpp"

struct World;

// The producer that runs a QueryProgram: a stackless, backtracking VM in a
// single loop. Every call to the iterator's next() resumes the program where
// the last YIELD left it: the YIELD op fails on resume, which sends control
// back to the previous op; that op continues its own iteration (a SELECT
// moves to the next archetype, an AND that binds a variable to the next
// matching pair, ...) and, when it has nothing left, fails in turn. Control
// moves forward again as soon as an op succeeds, and the walk is over once
// the first op has nothing left.
//
//     TemporalAllocator temp = TemporalAllocator::create();
//     QueryIter it = QUERY_VM::begin(&world, &program, &temp);
//     while (it.next(&it)) {
//         EntityId food = it.vars[food_var.index];
//         Position* positions = it.field<0, Position>();
//         ...
//     }
//
// A chunk is one archetype (or a one-row range of it) with one binding of
// every other variable: an archetype with two (Likes, *) pairs comes out
// twice for a query that binds $food. A chunk's `vars` are the bound
// entities (0 when a variable is unbound, which only optional terms allow),
// `ids` / `sources` / `columns` are per term and per field as query_iter.hpp
// describes, and a field with a source is shared. A query with no term on
// THIS binds nothing to it: it yields one chunk with no rows (count 0) if
// its terms hold and none otherwise.
//
// A program that is only SELECT + YIELD (plain terms, the common case) runs
// in a trivial mode: next() is one loop over the candidate archetypes with
// the chunk filled in place, no op dispatch, no backtracking. The results
// are the same; it is just the scan's cost.
//
// With a QueryScanCache built from the program (DynamicQuery::ensure_cache),
// SELECT walks the cached matches instead of the candidates: no matcher
// test, and the columns of the plain THIS terms come from the cache instead
// of a lookup per archetype. The rest of the program runs as before.
//
// Everything the iterator needs is on `allocator`, which must outlive the
// iteration; nothing is freed explicitly. The candidate archetypes (or the
// cached matches) are fixed when begin() is called, like QUERY_SCAN; pair
// targets and sources are looked up live, so structural changes during a
// walk have the same caveats.
namespace QUERY_VM {

// Depth at which an up() walk gives up and reports a cycle.
constexpr usz MAX_TRAVERSAL_DEPTH = 1024;

QueryIter begin(World* world, const QueryProgram* program, BaseAllocator* allocator);
// The same over `cache`, whose matcher is the program's and whose terms are
// the program's plain THIS terms in order (program->this_terms). nullptr
// means no cache.
QueryIter begin(World* world, const QueryProgram* program, const QueryScanCache* cache, BaseAllocator* allocator);

// Like begin(), with THIS bound to `entity` alone instead of to every
// candidate archetype: SELECT checks that entity's archetype against the
// matcher and yields one-row chunks. An entity that is not alive or has no
// ids yields nothing. The point of it is matches(); it is public for
// callers that want the bindings too.
QueryIter begin_for(World* world, const QueryProgram* program, EntityId entity, BaseAllocator* allocator);

// Whether `entity` is in the result set: begin_for() yields at least once.
// Uses scratch memory only.
bool matches(World* world, const QueryProgram* program, EntityId entity);

} // namespace QUERY_VM
