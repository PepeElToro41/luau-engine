#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/ecs_types.hpp"

// The building blocks every query is made of. Both front ends produce a flat
// list of QueryTerm: World::query<Ts...>() from its template list plus its
// with() / without() ids, World::query_build() from whatever the builder was
// told. Everything below the front ends (matching, iteration, caching,
// monitors, observers) only ever sees terms.

// Creation-time options, shared by World::query<Ts...>(flags) and
// World::query_build(flags). Options that must be known before a query first
// runs belong here rather than in a chain method.
enum QueryFlags : u32 {
    QUERY_NONE = 0,
    // Keep the list of matched archetypes up to date instead of scanning the
    // world's archetypes on every iteration. Costs a matcher test per query
    // whenever an archetype is created, and memory the handle has to give
    // back (Query<Ts...>::cleanup(); DynamicQuery::free() or cleanup()).
    // Both front ends cache through QueryScanCache (see query_scan.hpp): the
    // simple query walks it directly, the DynamicQuery's SELECT walks it and
    // the rest of its program still runs. Monitors do not need it; they keep
    // their own matcher (see monitor.hpp).
    QUERY_CACHED = 1 << 0,
};

// A query variable: a slot the engine binds to an entity while it evaluates a
// DynamicQuery. Index 0 is THIS, the entity being matched; QueryBuilder::var()
// hands out the rest. A default-constructed QueryVar means "no variable",
// which is how a term says a side is a concrete id.
struct QueryVar {
    static constexpr u32 NONE = 0xFFFFFFFFu;

    u32 index = NONE;

    bool is_set() const { return this->index != NONE; }
    bool operator==(const QueryVar& other) const { return this->index == other.index; }
    bool operator!=(const QueryVar& other) const { return this->index != other.index; }
};

constexpr QueryVar QUERY_THIS { 0 };

enum QueryTermFlags : u32 {
    // The term's data is delivered to the iteration callback (query<T>,
    // term<T>()). Without it the term is only a constraint (with(),
    // without()) and no column is ever resolved for it.
    TERM_OUTPUT = 1 << 0,
    // The source need not hold the id. An output term then yields nullptr for
    // archetypes without it; a variable-binding term leaves the variable unbound.
    TERM_OPTIONAL = 1 << 1,
    // The source must not hold the id (without()).
    TERM_EXCLUDE = 1 << 2,
    // This term or the next one must hold (bor()); the last term of a chain
    // has the flag clear.
    TERM_OR = 1 << 3,
    // The id may be found on an entity reached by following `traverse` up
    // from the source (up()).
    TERM_UP = 1 << 4,
    // up() that also orders the results: the matched entities come out by
    // ascending depth along `traverse`, so an ancestor's archetype is always
    // yielded before its descendants' (cascade()). Always with TERM_UP; one
    // per query, on THIS.
    TERM_CASCADE = 1 << 5,
    // Reverses a cascade: deepest first (desc()).
    TERM_DESC = 1 << 6,
};

struct QueryTerm {
    // The id or pattern to look for on the source. A side that is a variable
    // is written as WILDCARD here, so a matcher can still prefilter on the
    // concrete side: (Likes, $food) is stored as (Likes, *) plus second_var.
    Id id = 0;
    // Where to look: a variable (THIS by default) or, when src_var is not set,
    // the concrete entity `src`.
    Id src = 0;
    QueryVar src_var = QUERY_THIS;
    // Variables bound from the id that actually matched: first_var to the
    // plain id or the pair's relation, second_var to the pair's target.
    QueryVar first_var;
    QueryVar second_var;
    // Relation walked up from the source when TERM_UP is set; 0 otherwise.
    Id traverse = 0;
    // QueryTermFlags.
    u32 flags = 0;

    bool is_output() const { return (this->flags & TERM_OUTPUT) != 0; }
    bool is_optional() const { return (this->flags & TERM_OPTIONAL) != 0; }
    bool is_excluded() const { return (this->flags & TERM_EXCLUDE) != 0; }
    bool is_or() const { return (this->flags & TERM_OR) != 0; }
    bool traverses() const { return (this->flags & TERM_UP) != 0; }
    bool cascades() const { return (this->flags & TERM_CASCADE) != 0; }
    bool descends() const { return (this->flags & TERM_DESC) != 0; }
    // The term reads only the matched entity's own archetype: THIS source,
    // no traversal. Every term of a World::query<Ts...>() is.
    bool is_on_this() const { return this->src_var == QUERY_THIS && !this->traverses(); }
    // Some side of the id is a variable to bind.
    bool binds() const { return this->first_var.is_set() || this->second_var.is_set(); }

    // A term on THIS for `id` with the given flags, the shape both front ends
    // start from.
    static QueryTerm make(const Id id, const u32 flags) {
        QueryTerm term;
        term.id = id;
        term.flags = flags;
        return term;
    }
};
