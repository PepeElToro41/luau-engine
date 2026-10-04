#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/archetype.hpp"
#include "engine/ecs/archetype_listener.hpp"
#include "engine/ecs/archetype_matcher.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/ecs/query_term.hpp"

struct World;

// Observers: a callback for "something this query sees about the entity
// changed", meant for marking results dirty and recomputing them. One
// callback hears about both kinds of event: an archetype move that involves
// one of the query's ids and leaves the entity matching, and a write to the
// data of an output term of a matching entity. Observers never report an
// entity leaving the result set; that is what monitors are for.
//
//     static void on_event(World* world, EntityId entity, ObserverEvent event, Id id, void* user_data);
//
//     ObserverId id = world.query<Position>().with<Alive, Health>().observe(on_event, &state);
//     ...
//     world.unobserve(id);
//
// Where a term sits in the query decides what it reports. Every term reports
// the archetype moves that involve it; only the output terms (the template
// list of World::query<Ts...>(), TERM_OUTPUT) also report writes:
//
//     world.set(e, Position { });   // nothing: e does not match yet
//     world.add<Alive>(e);          // nothing: still no Health
//     world.set(e, Health { 50 });  // MOVED, Health: e now matches
//     world.set(e, Health { 100 }); // nothing: Health is a with() term, no move
//     world.set(e, Position { });   // CHANGED, Position: an output was written
//     world.set(e, Position { });   // CHANGED again
//     world.remove<Alive>(e);       // nothing: e stops matching
//     world.set(e, Position { });   // nothing: e no longer matches
//
// Unlike a monitor, an observer also fires while the entity keeps matching,
// if the id that moved is one of the query's: with the term (Likes, *), an
// entity holding (Likes, a) that gains (Likes, b) reports MOVED (Likes, b),
// and losing (Likes, a) afterwards reports MOVED (Likes, a) too, since what
// the query sees of the entity changed. An exclusive relation swapping its
// target reports the new pair. A move that touches no term of the query (an
// unrelated id) is silent, however many of its ids the entity has, and so
// is any move the entity does not match after.
//
// Terms follow the matcher's patterns: (R, *), (*, T), (*, *), WILDCARD; the
// id reported is always the concrete one. An entity that starts matching
// because a without() id was removed reports MOVED with that id.
//
// Timing: MOVED fires after the move and, for set(), after the data was
// written, before the monitors' ENTER and the added hooks; CHANGED fires
// right after the write, before the changed hooks. Either callback may do
// anything, including moving or destroying the entity, and may create or
// destroy observers, including its own.
//
// An observer is a matcher kept over the archetype graph like a monitor:
// every matching archetype lists the observer's id, plus one entry per
// (id of its type, matching term) so an event on an id finds the observers
// that care in one lower bound (see ArchetypeObservers). Only the trivial
// query shape is accepted: every term on THIS, no optionals, or-terms or
// traversal.

enum ObserverEvent : u32 {
    // The entity moved archetype because of `id`, an id of the query, and
    // matches the query now (it may or may not have before).
    OBSERVER_MOVED = 0,
    // set() overwrote, or modified() reported, the data of output term `id`
    // on a matching entity.
    OBSERVER_CHANGED = 1,
};

using ObserverCallback = void (*)(World* world, EntityId entity, ObserverEvent event, Id id, void* user_data);

struct Observer {
    ObserverId id = 0;
    // Id lists on the world's allocator.
    ArchetypeMatcher matcher;
    // The query, copied onto the world's allocator; tagging an archetype
    // walks it to find which of the archetype's ids each term matches.
    QueryTerm* terms = nullptr;
    usz term_count = 0;
    ObserverCallback callback = nullptr;
    void* user_data = nullptr;
    // Tags the archetypes created after the observer; removed with it.
    ArchetypeListenerId listener = 0;
    // The with id whose ComponentRecord create() walked to find the matching
    // archetypes (see ARCHETYPE_CANDIDATES); destroy() walks it again to
    // untag them. 0 when no with id narrows the walk.
    Id record_id = 0;
};

namespace OBSERVER {

// Registers an observer over `terms` and tags every matching archetype with
// it. Entities already in the result set fire nothing. Returns the id, or 0
// with an error printed for a null callback, a 0 id, or a term the trivial
// matcher cannot express.
ObserverId create(World* world, const QueryTerm* terms, usz term_count, ObserverCallback callback, void* user_data);
// Removes the observer from every archetype and releases it. False if no
// observer has that id.
bool destroy(World* world, ObserverId id);

// Fires MOVED after `entity` moved from `source` to `destination` because
// `added` was added and / or `removed` removed (0 for none; both set for an
// exclusive swap), for the observers of `destination` that either id
// concerns, and for those `source` lacks (the entity entered). Either
// archetype may be the root (in no observer) or nullptr. Returns at once
// when the world has no observers.
void fire_moved(World* world, EntityId entity, const Archetype* source, const Archetype* destination, Id added, Id removed);
// CHANGED for the observers of `archetype` with `id` as an output term,
// after its data was written.
void fire_changed(World* world, EntityId entity, const Archetype* archetype, Id id);

} // namespace OBSERVER
