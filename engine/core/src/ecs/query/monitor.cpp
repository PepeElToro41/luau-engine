#include "engine/ecs/query/monitor.hpp"

#include "engine/ecs/archetype/archetype_candidates.hpp"
#include "engine/ecs/archetype/archetype_listener.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/utils/sorted_ids.hpp"
#include "engine/ecs/world.hpp"
#include "engine/memory/temporal_allocator.hpp"

#include <cstdio>

namespace MONITOR {

namespace {

// Writes the ids `only` has and `other` lacks into `out` (both lists sorted
// ascending; either archetype may be nullptr). Returns how many.
usz diff(const Archetype* only, const Archetype* other, ObserverId* out) {
    if (only == nullptr) {
        return 0;
    }
    const DynamicArray<ObserverId>& a = only->observers.monitors;
    const usz b_count = other != nullptr ? other->observers.monitors.count : 0;
    const ObserverId* b = other != nullptr ? other->observers.monitors.data : nullptr;

    usz i = 0;
    usz j = 0;
    usz written = 0;
    while (i < a.count) {
        if (j < b_count && b[j] < a[i]) {
            j++;
            continue;
        }
        if (j < b_count && b[j] == a[i]) {
            i++;
            j++;
            continue;
        }
        out[written++] = a[i++];
    }
    return written;
}

// The monitor's archetype listener: a new archetype holding the monitor's
// first `with` id (or any new archetype, for a monitor without one) is run
// through the full matcher and tagged if it passes. `user_data` carries the
// ObserverId, since Monitor values move when the world's map rehashes.
void on_archetype(World* world, Archetype* archetype, const ArchetypeEvent event, void* user_data) {
    if (event != ARCHETYPE_CREATED || archetype == world->root_archetype) {
        return;
    }
    const ObserverId id = static_cast<ObserverId>(reinterpret_cast<usz>(user_data));
    const Monitor* monitor = world->monitors.find(id);
    if (monitor != nullptr && monitor->matcher.matches(archetype)) {
        SORTED_IDS::insert(archetype->observers.monitors, id);
    }
}

// Fires `event` for every monitor `only` has and `other` lacks.
void fire(World* world, const EntityId entity, const MonitorEvent event, const Archetype* only, const Archetype* other) {
    if (world->monitors.is_empty() || only == nullptr || only->observers.monitors.count == 0) {
        return;
    }

    // A callback may create or destroy monitors, which edits the lists being
    // compared, so the diff is taken up front and each id is looked up again
    // right before it fires: a monitor destroyed by an earlier callback of
    // this batch is skipped.
    TemporalAllocator temp = TemporalAllocator::create();
    ObserverId* ids = temp.allocate_array<ObserverId>(only->observers.monitors.count);
    const usz count = diff(only, other, ids);
    for (usz i = 0; i < count; i++) {
        const Monitor* monitor = world->monitors.find(ids[i]);
        if (monitor == nullptr) {
            continue;
        }
        monitor->callback(world, entity, event, monitor->user_data);
    }
}

} // namespace

ObserverId create(World* world, const QueryTerm* terms, const usz term_count, const MonitorCallback callback, void* user_data) {
    if (callback == nullptr) {
        fprintf(stderr, "[ecs] error: monitor needs a callback\n");
        return 0;
    }

    TemporalAllocator temp = TemporalAllocator::create();
    Id* with = temp.allocate_array<Id>(term_count + 1);
    Id* without = temp.allocate_array<Id>(term_count + 1);
    usz with_count = 0;
    usz without_count = 0;
    for (usz i = 0; i < term_count; i++) {
        const QueryTerm& term = terms[i];
        if (term.id == 0) {
            fprintf(stderr, "[ecs] error: monitor term %llu has id 0 (did a type fail to register?)\n", i);
            return 0;
        }
        if (!term.is_on_this() || term.is_optional() || term.is_or()) {
            fprintf(stderr, "[ecs] error: monitor term %llu (%llx) is not a plain term on the matched entity; monitors only support with() / without() terms\n",
                i, term.id);
            return 0;
        }
        if (term.is_excluded()) {
            without[without_count++] = term.id;
        } else {
            with[with_count++] = term.id;
        }
    }

    Monitor monitor;
    monitor.id = world->next_observer_id++;
    monitor.matcher = ArchetypeMatcher::create(world->allocator, with, with_count, without, without_count);
    monitor.callback = callback;
    monitor.user_data = user_data;
    // Any archetype the matcher accepts holds every `with` id, so listening
    // under the first one loses nothing and skips unrelated tables.
    monitor.listener = ARCHETYPE_LISTENER::add(world, ARCHETYPE_LISTENER::key_for(with, with_count), on_archetype,
        reinterpret_cast<void*>(static_cast<usz>(monitor.id)));
    // Only the archetypes holding the rarest `with` id can match; the rest
    // of the world is never tested. destroy() walks the same record.
    const ArchetypeCandidates candidates = ARCHETYPE_CANDIDATES::collect(world, with, with_count, &temp);
    monitor.record_id = candidates.record_id;
    world->monitors.insert(monitor.id, monitor);

    if (candidates.narrowed) {
        for (usz i = 0; i < candidates.count; i++) {
            Archetype* archetype = ARCHETYPE_CANDIDATES::archetype_of(world, candidates.entries[i]);
            if (archetype != nullptr && monitor.matcher.matches(archetype)) {
                SORTED_IDS::insert(archetype->observers.monitors, monitor.id);
            }
        }
        return monitor.id;
    }
    for (usz i = 0; i < world->archetypes.alive_count; i++) {
        Archetype* archetype = world->archetypes.get_element_any(world->archetypes.get_alive_id(i));
        if (archetype == world->root_archetype) {
            continue;
        }
        if (monitor.matcher.matches(archetype)) {
            SORTED_IDS::insert(archetype->observers.monitors, monitor.id);
        }
    }
    return monitor.id;
}

bool destroy(World* world, const ObserverId id) {
    Monitor* monitor = world->monitors.find(id);
    if (monitor == nullptr) {
        return false;
    }
    monitor->matcher.free();
    ARCHETYPE_LISTENER::remove(world, monitor->listener);
    const Id record_id = monitor->record_id;
    world->monitors.remove(id);

    // Every archetype tagged with the monitor holds all of its `with` ids,
    // so the record create() narrowed through still lists them all,
    // archetypes created since included. A record that is gone means no
    // archetype holds the id any more.
    TemporalAllocator temp = TemporalAllocator::create();
    const ArchetypeCandidates candidates = ARCHETYPE_CANDIDATES::collect_for(world, record_id, &temp);
    if (candidates.narrowed) {
        for (usz i = 0; i < candidates.count; i++) {
            Archetype* archetype = ARCHETYPE_CANDIDATES::archetype_of(world, candidates.entries[i]);
            if (archetype != nullptr) {
                SORTED_IDS::remove(archetype->observers.monitors, id);
            }
        }
        return true;
    }
    for (usz i = 0; i < world->archetypes.alive_count; i++) {
        Archetype* archetype = world->archetypes.get_element_any(world->archetypes.get_alive_id(i));
        SORTED_IDS::remove(archetype->observers.monitors, id);
    }
    return true;
}

void fire_leave(World* world, const EntityId entity, const Archetype* source, const Archetype* destination) {
    fire(world, entity, MONITOR_LEAVE, source, destination);
}

void fire_enter(World* world, const EntityId entity, const Archetype* source, const Archetype* destination) {
    fire(world, entity, MONITOR_ENTER, destination, source);
}

} // namespace MONITOR
