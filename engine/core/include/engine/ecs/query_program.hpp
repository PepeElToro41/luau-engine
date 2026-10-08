#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/archetype_matcher.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/ecs/query_term.hpp"
#include "engine/memory/base_allocator.hpp"

struct World;

// The compiled form of a DynamicQuery: a small list of opcodes the query VM
// (see query_vm.hpp) steps through in a loop, plus the cheap tests that run
// before the VM gets involved. Modeled on the flecs query engine.
//
// Compilation splits the terms in two:
//
//   - Terms the matched entity's own archetype can answer by itself (source
//     THIS, no variable to bind, no traversal, not optional, not in an
//     or-chain) go into the program's ArchetypeMatcher, the minimum an
//     archetype has to satisfy before the VM runs on it. Terms that need the
//     VM still contribute what they can to it: (Likes, $food) on THIS adds
//     (Likes, *), and a term that walks `up(R)` from THIS adds (R, *), since
//     the entity needs a parent to walk to. The matcher's with ids also pick
//     the candidate archetypes (see archetype_candidates.hpp), so a query
//     never looks at archetypes that cannot match.
//   - Everything else becomes an op: binding variables from pair sides,
//     terms on other sources (a fixed entity or a variable), traversal, not
//     terms whose id is only known at run time, or-chains and optionals.
//
// Ops run in program order; every op that succeeds hands control to the next
// one and every op that fails hands it back to the previous one, which
// resumes its own iteration (the `redo`), so a query backtracks without a
// stack. Variables are bound by the op that first sees them and unbound again
// when that op runs out of matches. The compiler orders the ops so that a
// term's source variable is bound by an earlier op: terms are taken in the
// order given, and one whose variables are not bound yet waits for a term
// that binds them. The query ends with YIELD, which emits one chunk per
// successful pass and then backtracks for the next one.
//
// THIS is always bound by SELECT, from the candidate archetypes; a pair side
// cannot be THIS, and a variable used as a source has to be bound by a pair
// side of an earlier term. A term that walks up only looks at the ancestors
// of its source, never at the source itself, and takes the first ancestor
// holding the id in depth-first order over the (relation, *) pairs; with
// several parents the others are not visited.

constexpr u32 QUERY_OP_NONE = 0xFFFFFFFFu;
// Variables a program can have, THIS included: the compiler tracks which are
// bound in a 64-bit mask.
constexpr usz QUERY_MAX_VARS = 64;

enum QueryOpKind : u8 {
    // Binds THIS to each candidate archetype in turn, skipping the ones the
    // matcher rejects and the empty ones. The first op whenever any term is
    // on THIS. If the VM was started for a single entity, it checks that
    // entity's archetype instead.
    QUERY_OP_SELECT,
    // The source holds an id matching the term. The source is bound; the
    // id's variable sides may not be: then the op walks the source's type
    // and binds them from each matching id, one per pass.
    QUERY_OP_AND,
    // The source holds no id matching the term. Every variable is bound.
    QUERY_OP_NOT,
    // Like AND, but the id is looked for on the ancestors reached by walking
    // the term's `traverse` relation up from the source. The source itself
    // is not looked at. A not-term with traversal (without<X>().up()) holds
    // when no ancestor has the id.
    QUERY_OP_UP,
    // An or-chain: `term_count` consecutive terms of which the first one
    // that holds wins. Alternatives bind nothing.
    QUERY_OP_OR,
    // Emits the current bindings as a chunk, then fails so the previous op
    // resumes.
    QUERY_OP_YIELD,
};

struct QueryOp {
    QueryOpKind kind = QUERY_OP_YIELD;
    // AND / UP: when nothing matches, succeed once anyway with the term's
    // result cleared (id 0, column nullptr) and its variables unbound.
    bool optional = false;
    // Index of the term the op evaluates, QUERY_OP_NONE for SELECT and YIELD.
    // An OR op covers terms [term, term + term_count).
    u32 term = QUERY_OP_NONE;
    u32 term_count = 1;
    // Variables the op binds from the matched id's relation / target, or
    // QUERY_OP_NONE when that side is concrete or bound by an earlier op.
    u32 bind_first = QUERY_OP_NONE;
    u32 bind_second = QUERY_OP_NONE;
};

struct QueryProgram {
    BaseAllocator* allocator = nullptr;
    // False when compile() rejected the terms; the VM then yields nothing.
    bool ok = false;

    // The terms, copied, in the order given to compile().
    QueryTerm* terms = nullptr;
    usz term_count = 0;
    // Per term: the index of its field among the output terms, or
    // QUERY_OP_NONE for a constraint term.
    u32* term_fields = nullptr;
    usz field_count = 0;
    // Variables, THIS included.
    usz var_count = 0;

    // The prefilter every THIS archetype has to pass, and the with ids it
    // was built from (for the candidate list).
    ArchetypeMatcher matcher;
    Id* with_ids = nullptr;
    usz with_count = 0;
    // Terms on THIS that no op evaluates: the matcher answered them (or they
    // are optional and plain). YIELD resolves their column in the bound
    // archetype.
    u32* this_terms = nullptr;
    usz this_term_count = 0;
    // Whether the program starts with SELECT, i.e. some term is on THIS.
    bool binds_this = false;

    QueryOp* ops = nullptr;
    usz op_count = 0;

    // Releases everything. The program is empty and not ok afterwards.
    void free();
};

namespace QUERY_PROGRAM {

// Compiles `terms` into a program on `allocator`. `var_count` is the number
// of variables the terms may refer to, THIS included (QueryBuilder::var_count).
// Errors are printed and leave `ok` false: an id of 0, a variable index out
// of range or above QUERY_MAX_VARS, THIS used as a pair side, an excluded
// term that is also optional or that would bind a variable, an or-chain
// alternative that is optional or binds, traversal through a relation that
// is not TRAVERSABLE, and a variable that is never bound (as a source, or in
// a not-term) by any term. The program must be freed either way.
QueryProgram compile(World* world, const QueryTerm* terms, usz term_count, usz var_count, BaseAllocator* allocator);

// Name of an opcode, for diagnostics and tests.
const char* op_name(QueryOpKind kind);

} // namespace QUERY_PROGRAM
