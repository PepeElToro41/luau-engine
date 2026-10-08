#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/ecs/query/query_iter.hpp"
#include "engine/ecs/query/query_program.hpp"
#include "engine/ecs/query/query_term.hpp"
#include "engine/ecs/query/query_vm.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/memory/temporal_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"

#include <cstddef>
#include <cstdio>
#include <type_traits>
#include <utility>

struct World;

// The engine-evaluated query: what QueryBuilder::build() returns. It owns a
// compiled QueryProgram (see query_program.hpp) on the world's allocator and
// runs it through the query VM (see query_vm.hpp) on every iteration.
//
//     QueryBuilder builder = world.query_build();
//     QueryVar food = builder.var("food");
//     builder.term<Position>()
//            .with<Likes>(food)
//            .term<Health>().src(food).optional()
//            .without<Dead>();
//     DynamicQuery query = builder.build();
//
//     query.each<Position, Health*>([&](EntityId entity, Position& position, Health* food_health) {
//         // food_health is shared by the chunk and nullptr when the food has none
//     });
//     query.iter<Position, Health*>([&](QueryIter& it, Position* positions, Health** food_health) { ... });
//     query.free();
//
// The output types are given per call, since the query has no template
// list: one per output term in term order, a plain type for a field that is
// always there (delivered by reference) and a pointer type for one that may
// be missing (optional, or an or-chain alternative; delivered as a pointer,
// nullptr when missing). A field whose term has a source (src(), a variable
// or up()) holds one element shared by the chunk; each() hands every row
// that same element. Variables are read off the iterator: it.vars[var.index]
// is the entity bound for the chunk, 0 when unbound.
//
// The handle owns memory: call free(). It cannot be copied; move it. A query
// whose terms were rejected at build() has is_ok() false and matches
// nothing; the errors were printed by build().
struct DynamicQuery {
    World* world = nullptr;
    u32 flags = QUERY_NONE;
    QueryProgram program;
    // Names of the variables the builder handed out; entry i names variable
    // i + 1 (THIS is variable 0).
    DynamicArray<char*> var_names;

    DynamicQuery() = default;
    DynamicQuery(const DynamicQuery&) = delete;
    DynamicQuery& operator=(const DynamicQuery&) = delete;
    DynamicQuery(DynamicQuery&& other) noexcept;
    DynamicQuery& operator=(DynamicQuery&& other) noexcept;

    // Releases the program and the names. The query is empty afterwards.
    void free();

    bool is_ok() const { return this->program.ok; }
    usz field_count() const { return this->program.field_count; }
    usz term_count() const { return this->program.term_count; }
    usz var_count() const { return this->program.var_count; }
    const QueryTerm* terms() const { return this->program.terms; }

    // The variable called `name`, "this" for THIS, unset if unknown; and the
    // name of a variable, nullptr if unknown.
    QueryVar var(const char* name) const;
    const char* var_name(QueryVar variable) const;

    // --- Iteration -----------------------------------------------------------
    // Calls fn(EntityId, Fields...) for every row of every chunk, where
    // Fields are the output types in term order as described above: T& for
    // a plain T, T* for a T*. With no types, fn(EntityId). A mismatch
    // between the type count and the fields prints an error and iterates
    // nothing; a reference field that turns out missing skips its chunk with
    // an error. Chunks with no rows (a query with no term on THIS) call
    // nothing.
    template <typename... Ts, typename Fn>
    void each(Fn&& fn);
    // Calls fn(QueryIter&, Ts*...) once per chunk with the row-0 pointers of
    // the fields (pointer types are passed as pointers too, so Health* gives
    // Health*); check it.shared[N] before indexing a field by row.
    template <typename... Ts, typename Fn>
    void iter(Fn&& fn);
    // A cursor built on `allocator`, for hand-driven loops and the QUERY::
    // utilities; pass a TemporalAllocator kept alive while it is used.
    QueryIter begin(BaseAllocator* allocator);

    // Number of matched rows, whether there are none, the first row's entity
    // (or 0), and one picked uniformly (or 0). A query with no term on THIS
    // has no rows, so it counts 0 and is empty even when its terms hold.
    usz count();
    bool empty();
    EntityId first();
    EntityId random(u64& rng_state);
    // Whether `entity` is in the result set now: whether the program yields
    // with THIS bound to it alone. For a query with no term on THIS, whether
    // the query holds at all.
    bool matches(EntityId entity);

private:
    // Field N of the chunk for type T at `row`, T& or T* (nullptr when the
    // field is missing). `Shared` picks the chunk's one element instead of
    // the row, for the chunks that have a shared field; the others take the
    // plain path with nothing to check per row.
    template <usz N, typename T, bool Shared>
    static decltype(auto) element(const QueryIter& it, const usz row) {
        using Element = std::remove_pointer_t<T>;
        Element* column = it.template field<N, Element>();
        if constexpr (std::is_pointer_v<T>) {
            if (column == nullptr) {
                return static_cast<Element*>(nullptr);
            }
        }
        if constexpr (Shared) {
            if (it.shared[N]) {
                if constexpr (std::is_pointer_v<T>) {
                    return column;
                } else {
                    return *column;
                }
            }
        }
        if constexpr (std::is_pointer_v<T>) {
            return column + row;
        } else {
            return column[row];
        }
    }

    template <typename Fn, typename... Ts, std::size_t... Is>
    static bool each_chunk(QueryIter& it, Fn& fn, std::index_sequence<Is...>) {
        // A reference field cannot stand in for a missing column.
        const bool missing = ((!std::is_pointer_v<Ts> && it.columns[Is] == nullptr) || ...);
        if (missing) {
            return false;
        }
        const bool any_shared = (it.shared[Is] || ...);
        if (any_shared) {
            for (usz row = 0; row < it.count; row++) {
                fn(it.entities[row], DynamicQuery::element<Is, Ts, true>(it, row)...);
            }
        } else {
            for (usz row = 0; row < it.count; row++) {
                fn(it.entities[row], DynamicQuery::element<Is, Ts, false>(it, row)...);
            }
        }
        return true;
    }

    template <typename Fn, typename... Ts, std::size_t... Is>
    static void iter_chunk(QueryIter& it, Fn& fn, std::index_sequence<Is...>) {
        fn(it, static_cast<std::remove_pointer_t<Ts>*>(it.columns[Is])...);
    }

    bool check_fields(usz type_count, const char* what) const;
};

// --- Iteration ---------------------------------------------------------------

template <typename... Ts, typename Fn>
void DynamicQuery::each(Fn&& fn) {
    static_assert(std::is_invocable_v<Fn&, EntityId, std::conditional_t<std::is_pointer_v<Ts>, Ts, std::add_lvalue_reference_t<Ts>>...>,
        "each<Ts...>(fn): fn must take (EntityId, T& for each plain T, T* for each pointer T)");
    if (!this->check_fields(sizeof...(Ts), "each")) {
        return;
    }
    TemporalAllocator temp = TemporalAllocator::create();
    QueryIter it = this->begin(&temp);
    bool reported = false;
    while (it.next(&it)) {
        if (!DynamicQuery::each_chunk<Fn, Ts...>(it, fn, std::index_sequence_for<Ts...> { }) && !reported) {
            fprintf(stderr, "[ecs] error: each(): a field given as a plain type is missing in a chunk (optional term or or-chain); take it as a pointer\n");
            reported = true;
        }
    }
}

template <typename... Ts, typename Fn>
void DynamicQuery::iter(Fn&& fn) {
    static_assert(std::is_invocable_v<Fn&, QueryIter&, std::remove_pointer_t<Ts>*...>,
        "iter<Ts...>(fn): fn must take (QueryIter&, T*...) for the output types");
    if (!this->check_fields(sizeof...(Ts), "iter")) {
        return;
    }
    TemporalAllocator temp = TemporalAllocator::create();
    QueryIter it = this->begin(&temp);
    while (it.next(&it)) {
        DynamicQuery::iter_chunk<Fn, Ts...>(it, fn, std::index_sequence_for<Ts...> { });
    }
}
