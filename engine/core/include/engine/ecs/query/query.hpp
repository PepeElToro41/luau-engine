#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/ecs/query/monitor.hpp"
#include "engine/ecs/query/observer.hpp"
#include "engine/ecs/query/query_iter.hpp"
#include "engine/ecs/query/query_scan.hpp"
#include "engine/ecs/query/query_term.hpp"
#include "engine/ecs/world.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/memory/temporal_allocator.hpp"

#include <concepts>
#include <cstddef>
#include <cstdio>
#include <type_traits>
#include <utility>

// The trivial query: a flat conjunction over the matched entity's own
// archetype, spelled entirely as types and ids.
//
//     Query<Position, Velocity> movers = world.query<Position, Velocity>()
//         .with<Alive>()
//         .with(world.pair<Likes>(bob))
//         .without<Dead>();
//
// The template list is the output: each type there carries data and is
// delivered to the iteration callback, in order. with() and without() only
// constrain the match and are never fetched; they come in the same shapes as
// the World entity operations (raw ids, types including ECS::Pair<R, T>, and
// typed relation with runtime target). Anything else, optionals, variables,
// traversal, or-terms, sources other than the matched entity, belongs to
// World::query_build().
//
//     movers.each([](EntityId entity, Position& position, Velocity& velocity) { ... });
//     movers.iter([](QueryIter& it, Position* positions, Velocity* velocities) {
//         for (usz row = 0; row < it.count; row++) { ... }
//     });
//     usz n = movers.count();
//     ObserverId watching = movers.monitor(on_enter_leave, &state);
//     ObserverId dirtying = movers.observe(on_moved_or_changed, &state);
//
// The handle is a plain copyable value with nothing to destroy: the extra
// ids live in a fixed array, and every run scans the world's archetypes
// afresh through QUERY_SCAN, with its scratch state on a TemporalAllocator.
// The QUERY_CACHED flag is accepted but not acted on yet; terms() is the
// full description the cache will consume.

// How many ids with() and how many without() can hold. Past that an error is
// printed and the id is ignored; use World::query_build() for bigger queries.
constexpr usz QUERY_MAX_EXTRA_TERMS = 8;

template <typename... Ts>
struct Query {
    static_assert((!std::is_empty_v<ECS::StorageType<Ts>> && ...),
        "query<...>: every type in the list is an output and must carry data; put tags and data-less pairs in with<>()");

    static constexpr usz output_count = sizeof...(Ts);
    // Slots terms() may need.
    static constexpr usz max_term_count = output_count + 2 * QUERY_MAX_EXTRA_TERMS;

    World* world = nullptr;
    u32 flags = QUERY_NONE;
    Id with_ids[QUERY_MAX_EXTRA_TERMS] = {};
    Id without_ids[QUERY_MAX_EXTRA_TERMS] = {};
    usz with_count = 0;
    usz without_count = 0;

    // --- Constraints ---------------------------------------------------------
    // Ids the matched entity must hold. A 0 id is ignored, like everywhere else.
    template <typename... Ids>
        requires (sizeof...(Ids) > 0) && (std::convertible_to<Ids, Id> && ...)
    Query& with(Ids... ids) {
        (this->push_with(static_cast<Id>(ids)), ...);
        return *this;
    }
    template <typename... Us>
    Query& with() {
        (this->push_with(this->world->template id<Us>()), ...);
        return *this;
    }
    template <typename First>
    Query& with(const EntityId second) {
        this->push_with(this->world->template pair<First>(second));
        return *this;
    }

    // Ids the matched entity must not hold.
    template <typename... Ids>
        requires (sizeof...(Ids) > 0) && (std::convertible_to<Ids, Id> && ...)
    Query& without(Ids... ids) {
        (this->push_without(static_cast<Id>(ids)), ...);
        return *this;
    }
    template <typename... Us>
    Query& without() {
        (this->push_without(this->world->template id<Us>()), ...);
        return *this;
    }
    template <typename First>
    Query& without(const EntityId second) {
        this->push_without(this->world->template pair<First>(second));
        return *this;
    }

    // --- Iteration -----------------------------------------------------------
    // Calls fn(EntityId, Ts&...), or fn(Ts&...) if that is what fn takes, for
    // every matched entity, archetype by archetype in whatever order the
    // world holds them. Structural changes to the entity being visited (or
    // to others in its archetype) during the call invalidate the references;
    // see query_scan.hpp for what a walk tolerates.
    template <typename Fn>
    void each(Fn&& fn);
    // Calls fn(QueryIter&, Ts*...) once per matched archetype with the
    // chunk's column pointers (row 0 of each output); loop over it.count
    // rows and it.entities yourself.
    template <typename Fn>
    void iter(Fn&& fn);
    // A cursor over the results built on `allocator`, for hand-driven loops
    // and the QUERY:: utilities. Pass a TemporalAllocator you keep alive for
    // as long as the iterator is used; nothing has to be freed:
    //
    //     TemporalAllocator temp = TemporalAllocator::create();
    //     QueryIter it = query.begin(&temp);
    //     while (it.next(&it)) { Position* positions = it.field<0, Position>(); ... }
    QueryIter begin(BaseAllocator* allocator);

    // Number of matched entities, whether there are none, the first one (or
    // 0), and one picked uniformly at random (or 0; `rng_state` is advanced,
    // 0 seeds it). Each is a scan of the archetypes; see QUERY::.
    usz count();
    bool empty();
    EntityId first();
    EntityId random(u64& rng_state);
    // Whether `entity` is in the result set right now. An entity with no ids
    // never is: the root stores no rows and is not scanned.
    bool matches(EntityId entity);

    // --- Monitors ------------------------------------------------------------
    // Calls `callback` whenever an entity enters or leaves this query's
    // result set from now on (see monitor.hpp for timing and what a callback
    // may do). The monitor is independent of the handle: later with() /
    // without() calls do not affect it. Returns an id for World::unmonitor(),
    // or 0 on error.
    ObserverId monitor(MonitorCallback callback, void* user_data = nullptr);

    // --- Observers -----------------------------------------------------------
    // Calls `callback` whenever an entity matching this query moves
    // archetype because of one of the query's ids, or has the data of one of
    // the output types written (see observer.hpp for the events, their
    // timing and what a callback may do). The observer is independent of the
    // handle. Returns an id for World::unobserve(), or 0 on error.
    ObserverId observe(ObserverCallback callback, void* user_data = nullptr);

    // --- Terms ---------------------------------------------------------------
    // Writes the query as terms into `out`, which needs max_term_count slots:
    // the outputs first in template order (TERM_OUTPUT), then the with() ids,
    // then the without() ids (TERM_EXCLUDE). Every term is on THIS. Returns
    // how many were written. Registers the output types on first use.
    usz terms(QueryTerm* out) {
        usz count = 0;
        ((out[count++] = QueryTerm::make(this->world->template id<Ts>(), TERM_OUTPUT)), ...);
        for (usz i = 0; i < this->with_count; i++) {
            out[count++] = QueryTerm::make(this->with_ids[i], 0);
        }
        for (usz i = 0; i < this->without_count; i++) {
            out[count++] = QueryTerm::make(this->without_ids[i], TERM_EXCLUDE);
        }
        return count;
    }

private:
    template <typename Fn, std::size_t... Is>
    static void each_chunk(QueryIter& it, Fn& fn, std::index_sequence<Is...>) {
        for (usz row = 0; row < it.count; row++) {
            if constexpr (std::is_invocable_v<Fn&, EntityId, ECS::StorageType<Ts>&...>) {
                fn(it.entities[row], it.template field<Is, ECS::StorageType<Ts>>()[row]...);
            } else {
                static_assert(std::is_invocable_v<Fn&, ECS::StorageType<Ts>&...>,
                    "each(fn): fn must take (EntityId, Ts&...) or (Ts&...) for the query's output types");
                fn(it.template field<Is, ECS::StorageType<Ts>>()[row]...);
            }
        }
    }

    template <typename Fn, std::size_t... Is>
    static void iter_chunk(QueryIter& it, Fn& fn, std::index_sequence<Is...>) {
        static_assert(std::is_invocable_v<Fn&, QueryIter&, ECS::StorageType<Ts>*...>,
            "iter(fn): fn must take (QueryIter&, Ts*...) for the query's output types");
        fn(it, it.template field<Is, ECS::StorageType<Ts>>()...);
    }

    void push_with(const Id id) {
        Query::push_id(this->with_ids, this->with_count, id, "with");
    }
    void push_without(const Id id) {
        Query::push_id(this->without_ids, this->without_count, id, "without");
    }

    static void push_id(Id* list, usz& count, const Id id, const char* side) {
        if (id == 0) {
            return;
        }
        if (count == QUERY_MAX_EXTRA_TERMS) {
            fprintf(stderr, "[ecs] error: query %s() holds at most %llu ids; %llx was ignored. Use World::query_build() for larger queries\n",
                side, static_cast<unsigned long long>(QUERY_MAX_EXTRA_TERMS), static_cast<unsigned long long>(id));
            return;
        }
        list[count] = id;
        count++;
    }
};

// --- Iteration ---------------------------------------------------------------

template <typename... Ts>
template <typename Fn>
void Query<Ts...>::each(Fn&& fn) {
    TemporalAllocator temp = TemporalAllocator::create();
    QueryIter it = this->begin(&temp);
    while (it.next(&it)) {
        Query::each_chunk(it, fn, std::index_sequence_for<Ts...> { });
    }
}

template <typename... Ts>
template <typename Fn>
void Query<Ts...>::iter(Fn&& fn) {
    TemporalAllocator temp = TemporalAllocator::create();
    QueryIter it = this->begin(&temp);
    while (it.next(&it)) {
        Query::iter_chunk(it, fn, std::index_sequence_for<Ts...> { });
    }
}

template <typename... Ts>
QueryIter Query<Ts...>::begin(BaseAllocator* allocator) {
    QueryTerm terms[max_term_count];
    const usz count = this->terms(terms);
    return QUERY_SCAN::begin(this->world, terms, count, allocator);
}

template <typename... Ts>
usz Query<Ts...>::count() {
    TemporalAllocator temp = TemporalAllocator::create();
    QueryIter it = this->begin(&temp);
    return QUERY::count(it);
}

template <typename... Ts>
bool Query<Ts...>::empty() {
    TemporalAllocator temp = TemporalAllocator::create();
    QueryIter it = this->begin(&temp);
    return QUERY::empty(it);
}

template <typename... Ts>
EntityId Query<Ts...>::first() {
    TemporalAllocator temp = TemporalAllocator::create();
    QueryIter it = this->begin(&temp);
    return QUERY::first(it);
}

template <typename... Ts>
EntityId Query<Ts...>::random(u64& rng_state) {
    TemporalAllocator temp = TemporalAllocator::create();
    QueryIter it = this->begin(&temp);
    return QUERY::random(it, rng_state);
}

template <typename... Ts>
bool Query<Ts...>::matches(const EntityId entity) {
    QueryTerm terms[max_term_count];
    const usz count = this->terms(terms);
    return QUERY_SCAN::matches(this->world, terms, count, entity);
}

// --- Monitors ----------------------------------------------------------------

template <typename... Ts>
ObserverId Query<Ts...>::monitor(const MonitorCallback callback, void* user_data) {
    QueryTerm terms[max_term_count];
    const usz count = this->terms(terms);
    return this->world->monitor(terms, count, callback, user_data);
}

// --- Observers ---------------------------------------------------------------

template <typename... Ts>
ObserverId Query<Ts...>::observe(const ObserverCallback callback, void* user_data) {
    QueryTerm terms[max_term_count];
    const usz count = this->terms(terms);
    return this->world->observe(terms, count, callback, user_data);
}

template <typename... Ts>
Query<Ts...> World::query(const u32 flags) {
    Query<Ts...> query;
    query.world = this;
    query.flags = flags;
    return query;
}
