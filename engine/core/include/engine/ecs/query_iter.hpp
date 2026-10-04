#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"

struct World;
struct Archetype;

// Cursor over the results of any query: a sequence of chunks, each one
// archetype's rows with the data pointers of the output terms. Who produces
// the chunks does not matter to the consumer: an uncached Query<Ts...> walks
// the world's archetypes, a cached one walks its QueryCache, a DynamicQuery
// steps its program. The producer sets `next` and keeps its cursor in `state`;
// everything that consumes results (each(), the QUERY:: utilities, monitors)
// is written once against this struct.
//
//     QueryIter it = query.begin();
//     while (it.next(&it)) {
//         Position* positions = it.field<0, Position>();
//         for (usz row = 0; row < it.count; row++) { ... it.entities[row] ... }
//     }
//
// The chunk fields are valid only after next() returned true, and only until
// the next call. Structural changes to the current archetype (add / remove /
// delete on one of its entities) during a chunk invalidate the pointers.
struct QueryIter {
    World* world = nullptr;

    // --- Current chunk -------------------------------------------------------
    Archetype* archetype = nullptr;
    EntityId* entities = nullptr;
    usz count = 0;
    // Per output term, in term order: the data at row 0, or nullptr for an
    // optional term the archetype lacks. Constraint terms have no entry.
    void** columns = nullptr;
    usz field_count = 0;
    // Per term, in term order: the concrete id that matched, which differs
    // from the term's id for patterns like (Likes, *). 0 for a term that did
    // not match (optional, excluded).
    Id* ids = nullptr;
    usz term_count = 0;
    // Per query variable: the entity bound for this chunk. [0] is THIS and is
    // not meaningful per chunk (it varies per row); use entities[row].
    EntityId* vars = nullptr;
    usz var_count = 0;

    // --- Producer ------------------------------------------------------------
    // Advances to the next chunk, returning false once there are none. Safe
    // to call again after it returned false; it keeps returning false.
    bool (*next)(QueryIter* it) = nullptr;
    void* state = nullptr;

    // Data of output term N as T, for the current chunk.
    template <usz N, typename T>
    T* field() const {
        return static_cast<T*>(this->columns[N]);
    }
};

// Utilities over an iterator. Each one drives the iterator forward and
// consumes it: take a fresh one from the query for every call.
namespace QUERY {

// Number of matched entities. Sums chunk counts; never touches rows.
usz count(QueryIter& it);
// Whether no entity matches. Stops at the first non-empty chunk.
bool empty(QueryIter& it);
// The first matched entity, or 0 if none.
EntityId first(QueryIter& it);
// A matched entity picked uniformly, or 0 if none. Single pass: each chunk is
// weighed by its row count, so the iterator need not be restartable.
// `rng_state` is xorshift state and is advanced; 0 is replaced by a fixed seed.
EntityId random(QueryIter& it, u64& rng_state);

// The xorshift64* step behind random(). `state` must not be 0.
u64 next_random(u64& state);

} // namespace QUERY
