#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/ecs/query/query_term.hpp"
#include "engine/ecs/world.hpp"
#include "engine/templates/dynamic_array.hpp"

#include <concepts>
#include <type_traits>

// Collects the terms of a DynamicQuery, the engine-evaluated query: the
// front end for anything World::query<Ts...>() cannot say.
//
//     QueryBuilder builder = world.query_build();
//     builder.term<Position>()                            // output
//            .term<Velocity>().optional()                 // output, may be missing
//            .with<Alive>()                               // constraint, never fetched
//            .with<Likes>(food).optional()                // binds $food when present
//            .with<Name>().src(parent).up(ECS::CHILD_OF)  // on another source
//            .without<Dead>();
//
// term() adds an output term, with() / without() add constraint terms; all
// three come in the same shapes as the World entity operations, plus shapes
// that take a QueryVar in place of an id. optional(), bor(), src() and up()
// modify the term added last, so with the variadic with(a, b) they apply to
// `b` only. Variables come from var(); THIS (QUERY_THIS) is always variable 0.
//
// The builder owns its term list on the world's allocator: call free() when
// done with it, or build() to turn it into a DynamicQuery (which consumes
// it). Terms are recorded as given; validating them (TRAVERSABLE relations
// for up(), or-chains, variables never bound) is build()'s job.
struct DynamicQuery;

struct QueryBuilder {
    World* world = nullptr;
    u32 flags = QUERY_NONE;
    DynamicArray<QueryTerm> terms;
    // Names of the variables var() handed out; entry i names variable i + 1.
    DynamicArray<char*> var_names;

    QueryBuilder(World* world, u32 flags);

    // Releases the term list and the variable names. The builder is empty and
    // reusable afterwards.
    void free();

    // Compiles the terms into a DynamicQuery (see dynamic_query.hpp) and
    // empties the builder: the variable names move to the query, the terms
    // are released. Terms that cannot be compiled are reported and leave the
    // query not ok (is_ok() false, nothing matches); free() it either way.
    DynamicQuery build();

    // --- Output terms --------------------------------------------------------
    template <typename T>
    QueryBuilder& term();
    template <typename First, typename Second>
    QueryBuilder& term();
    template <typename First>
    QueryBuilder& term(EntityId second);
    template <typename First>
    QueryBuilder& term(QueryVar second);
    QueryBuilder& term(Id id);
    QueryBuilder& term(Id first, QueryVar second);
    QueryBuilder& term(QueryVar first, Id second);

    // --- Constraint terms ----------------------------------------------------
    template <typename... Ids>
        requires (sizeof...(Ids) > 0) && (std::convertible_to<Ids, Id> && ...)
    QueryBuilder& with(Ids... ids);
    template <typename... Us>
    QueryBuilder& with();
    template <typename First>
    QueryBuilder& with(EntityId second);
    template <typename First>
    QueryBuilder& with(QueryVar second);
    QueryBuilder& with(Id first, QueryVar second);
    QueryBuilder& with(QueryVar first, Id second);

    template <typename... Ids>
        requires (sizeof...(Ids) > 0) && (std::convertible_to<Ids, Id> && ...)
    QueryBuilder& without(Ids... ids);
    template <typename... Us>
    QueryBuilder& without();
    template <typename First>
    QueryBuilder& without(EntityId second);
    template <typename First>
    QueryBuilder& without(QueryVar second);
    QueryBuilder& without(Id first, QueryVar second);
    QueryBuilder& without(QueryVar first, Id second);

    // --- Modifiers of the last term ------------------------------------------
    // Each prints an error and does nothing if no term was added yet.
    QueryBuilder& optional();
    QueryBuilder& bor();
    // The term is evaluated on `source` instead of the matched entity.
    QueryBuilder& src(EntityId source);
    QueryBuilder& src(QueryVar source);
    // The id may also be found by walking `relation` up from the source.
    QueryBuilder& up(Id relation = ECS::CHILD_OF);

    // --- Variables -----------------------------------------------------------
    // The variable called `name`, creating it on first use. "this" is
    // QUERY_THIS. Returns an unset QueryVar for a null or empty name.
    QueryVar var(const char* name);
    // Name of `variable`, "this" for QUERY_THIS, nullptr if unknown.
    const char* var_name(QueryVar variable) const;
    // Variables in use, THIS included.
    usz var_count() const { return this->var_names.count + 1; }

    // Number of output terms, the fields an iteration callback receives.
    usz field_count() const;

private:
    // Appends a term on THIS. A 0 id (failed type registration) is still
    // recorded so build() can report it.
    QueryBuilder& add(Id id, QueryVar first_var, QueryVar second_var, u32 term_flags);
    // Pair (first, $second) and ($first, second), stored with WILDCARD on the
    // variable side.
    QueryBuilder& add_pair_second_var(Id first, QueryVar second, u32 term_flags);
    QueryBuilder& add_pair_first_var(QueryVar first, Id second, u32 term_flags);
    // The term the modifiers apply to, or nullptr (with an error printed).
    QueryTerm* last(const char* modifier);
};

// --- Output terms ------------------------------------------------------------

template <typename T>
QueryBuilder& QueryBuilder::term() {
    static_assert(!std::is_empty_v<ECS::StorageType<T>>, "term<T>: T carries no data; use with<T>()");
    return this->add(this->world->template id<T>(), QueryVar {}, QueryVar {}, TERM_OUTPUT);
}

template <typename First, typename Second>
QueryBuilder& QueryBuilder::term() {
    static_assert(!std::is_empty_v<typename ECS::Pair<First, Second>::type>, "term<First, Second>: the pair carries no data; use with<First, Second>()");
    return this->add(this->world->template pair<First, Second>(), QueryVar {}, QueryVar {}, TERM_OUTPUT);
}

template <typename First>
QueryBuilder& QueryBuilder::term(const EntityId second) {
    return this->add(this->world->template pair<First>(second), QueryVar {}, QueryVar {}, TERM_OUTPUT);
}

template <typename First>
QueryBuilder& QueryBuilder::term(const QueryVar second) {
    return this->add_pair_second_var(this->world->template id<First>(), second, TERM_OUTPUT);
}

// --- Constraint terms --------------------------------------------------------

template <typename... Ids>
    requires (sizeof...(Ids) > 0) && (std::convertible_to<Ids, Id> && ...)
QueryBuilder& QueryBuilder::with(Ids... ids) {
    (this->add(static_cast<Id>(ids), QueryVar {}, QueryVar {}, 0), ...);
    return *this;
}

template <typename... Us>
QueryBuilder& QueryBuilder::with() {
    (this->add(this->world->template id<Us>(), QueryVar {}, QueryVar {}, 0), ...);
    return *this;
}

template <typename First>
QueryBuilder& QueryBuilder::with(const EntityId second) {
    return this->add(this->world->template pair<First>(second), QueryVar {}, QueryVar {}, 0);
}

template <typename First>
QueryBuilder& QueryBuilder::with(const QueryVar second) {
    return this->add_pair_second_var(this->world->template id<First>(), second, 0);
}

template <typename... Ids>
    requires (sizeof...(Ids) > 0) && (std::convertible_to<Ids, Id> && ...)
QueryBuilder& QueryBuilder::without(Ids... ids) {
    (this->add(static_cast<Id>(ids), QueryVar {}, QueryVar {}, TERM_EXCLUDE), ...);
    return *this;
}

template <typename... Us>
QueryBuilder& QueryBuilder::without() {
    (this->add(this->world->template id<Us>(), QueryVar {}, QueryVar {}, TERM_EXCLUDE), ...);
    return *this;
}

template <typename First>
QueryBuilder& QueryBuilder::without(const EntityId second) {
    return this->add(this->world->template pair<First>(second), QueryVar {}, QueryVar {}, TERM_EXCLUDE);
}

template <typename First>
QueryBuilder& QueryBuilder::without(const QueryVar second) {
    return this->add_pair_second_var(this->world->template id<First>(), second, TERM_EXCLUDE);
}
