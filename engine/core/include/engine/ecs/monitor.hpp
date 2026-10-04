#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/archetype.hpp"
#include "engine/ecs/archetype_listener.hpp"
#include "engine/ecs/archetype_matcher.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/ecs/query_term.hpp"

struct World;

// Monitors: a callback for entities entering and leaving a query's result set.
//
//     static void on_mover(World* world, EntityId entity, MonitorEvent event, void* user_data);
//
//     ObserverId movers = world.query<Position, Velocity>().without<Frozen>().monitor(on_mover, &state);
//     ...
//     world.unmonitor(movers);
//
// A monitor is a matcher kept over the archetype graph. When it is created
// every archetype it matches gets the monitor's id in its sorted
// Archetype::observers.monitors list, and an archetype listener (see
// archetype_listener.hpp) registered under the first `with` id tests each
// archetype created afterwards, so tables that cannot match never reach the
// matcher. Whether an entity is in the result set is then a
// property of the archetype it sits in, and an entity moving between two
// archetypes fires exactly the monitors whose membership changed: one merge
// walk over the two sorted lists finds the ids only the destination has
// (MONITOR_ENTER) and the ids only the source has (MONITOR_LEAVE). The root
// archetype stores no rows and is never part of a result set, so an entity
// with no ids is in no monitor: creating an entity fires nothing, clearing
// one leaves everything it was in, and deleting one leaves everything.
//
// Timing follows the hooks. LEAVE fires before the entity moves, with its
// data still readable, and the callback must not add to, remove from or
// destroy the entity it is told about (changing other entities is fine).
// ENTER fires after the move and, for set(), after the data was written, but
// before the added hook; the callback may do anything. A callback may create
// or destroy monitors, including its own.
//
// Only the trivial query shape is accepted: every term on THIS, no optionals,
// or-terms or traversal. Creating a monitor costs one matcher test per
// existing archetype; afterwards each new archetype is tested once per
// monitor whose first `with` id it holds, and moves pay a walk over two
// short id lists. Destroyed archetypes need no bookkeeping: the id list
// goes with the table.

enum MonitorEvent : u32 {
    // The entity now matches the query (after the move that made it match).
    MONITOR_ENTER = 0,
    // The entity is about to stop matching (before the move; data readable).
    MONITOR_LEAVE = 1,
};

using MonitorCallback = void (*)(World* world, EntityId entity, MonitorEvent event, void* user_data);

struct Monitor {
    ObserverId id = 0;
    // Id lists on the world's allocator.
    ArchetypeMatcher matcher;
    MonitorCallback callback = nullptr;
    void* user_data = nullptr;
    // Tags the archetypes created after the monitor; removed with it.
    ArchetypeListenerId listener = 0;
    // The with id whose ComponentRecord create() walked to find the matching
    // archetypes (see ARCHETYPE_CANDIDATES); destroy() walks it again to
    // untag them. 0 when no with id narrows the walk.
    Id record_id = 0;
};

namespace MONITOR {

// Registers a monitor over `terms` and tags every matching archetype with
// its id. Entities already in the result set do not fire ENTER. Returns the
// id, or 0 with an error printed for a null callback, a 0 id, or a term the
// trivial matcher cannot express.
ObserverId create(World* world, const QueryTerm* terms, usz term_count, MonitorCallback callback, void* user_data);
// Removes the monitor's id from every archetype and releases it. False if
// no monitor has that id.
bool destroy(World* world, ObserverId id);

// Fire LEAVE (before a move) or ENTER (after it) on `entity` for the monitors
// whose membership differs between `source` and `destination`. Either may be
// nullptr for "no archetype": the entity is being deleted (fire_leave with a
// null destination) or has none yet. Both return at once when the world has
// no monitors.
void fire_leave(World* world, EntityId entity, const Archetype* source, const Archetype* destination);
void fire_enter(World* world, EntityId entity, const Archetype* source, const Archetype* destination);

} // namespace MONITOR
