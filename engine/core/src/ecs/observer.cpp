#include "engine/ecs/observer.hpp"

#include "engine/ecs/archetype_candidates.hpp"
#include "engine/ecs/archetype_listener.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/utils/sorted_ids.hpp"
#include "engine/ecs/world.hpp"
#include "engine/memory/temporal_allocator.hpp"

#include <cstdio>

namespace OBSERVER {

namespace {

// Whether `id` (a concrete id of an archetype type) matches a non-excluded
// term of the observer; `output` is set when one of those terms is an output.
bool term_matches(const Observer& observer, const Id id, bool* output) {
    bool matched = false;
    *output = false;
    for (usz i = 0; i < observer.term_count; i++) {
        const QueryTerm& term = observer.terms[i];
        if (term.is_excluded() || !ECS::ID_MATCHES(ECS::FOLD_ANY(term.id), id)) {
            continue;
        }
        matched = true;
        if (term.is_output()) {
            *output = true;
        }
    }
    return matched;
}

// Index of the first entry with an id >= `id`, or count.
usz term_lower_bound(const DynamicArray<ArchetypeObserverTerm>& terms, const Id id) {
    usz low = 0;
    usz high = terms.count;
    while (low < high) {
        const usz middle = low + (high - low) / 2;
        if (terms[middle].id < id) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

// Inserts the entry keeping the list sorted by (id, observer); merges the
// output flag if the entry is already there. Type ids are walked ascending
// and observer ids grow with time, so this almost always appends.
void insert_term(DynamicArray<ArchetypeObserverTerm>& terms, const ArchetypeObserverTerm entry) {
    usz position = terms.count;
    while (position > 0) {
        const ArchetypeObserverTerm& previous = terms[position - 1];
        if (previous.id < entry.id || (previous.id == entry.id && previous.observer < entry.observer)) {
            break;
        }
        if (previous.id == entry.id && previous.observer == entry.observer) {
            terms[position - 1].output = terms[position - 1].output || entry.output;
            return;
        }
        position--;
    }
    terms.push(entry);
    for (usz i = terms.count - 1; i > position; i--) {
        terms[i] = terms[i - 1];
    }
    terms[position] = entry;
}

// Lists the observer on `archetype`: its id, plus an entry for every id of
// the type one of its terms matches.
void tag(Archetype* archetype, const Observer& observer) {
    SORTED_IDS::insert(archetype->observers.observers, observer.id);
    const ArchetypeType& type = archetype->type;
    for (usz i = 0; i < type.id_count; i++) {
        bool output = false;
        if (term_matches(observer, type.ids[i], &output)) {
            insert_term(archetype->observers.observer_terms, ArchetypeObserverTerm { type.ids[i], observer.id, output });
        }
    }
}

void untag(Archetype* archetype, const ObserverId id) {
    SORTED_IDS::remove(archetype->observers.observers, id);
    DynamicArray<ArchetypeObserverTerm>& terms = archetype->observers.observer_terms;
    usz i = 0;
    while (i < terms.count) {
        if (terms[i].observer == id) {
            terms.remove_at(i);
        } else {
            i++;
        }
    }
}

// The observer's archetype listener: a new archetype holding the observer's
// first `with` id (or any new archetype, for an observer without one) is run
// through the full matcher and tagged if it passes. `user_data` carries the
// ObserverId, since Observer values move when the world's map rehashes.
void on_archetype(World* world, Archetype* archetype, const ArchetypeEvent event, void* user_data) {
    if (event != ARCHETYPE_CREATED || archetype == world->root_archetype) {
        return;
    }
    const ObserverId id = static_cast<ObserverId>(reinterpret_cast<usz>(user_data));
    const Observer* observer = world->observers.find(id);
    if (observer != nullptr && observer->matcher.matches(archetype)) {
        tag(archetype, *observer);
    }
}

struct Event {
    ObserverId observer;
    Id id;
};

// Runs the callbacks. A callback may create or destroy observers, which
// edits the lists the events were taken from, so the events are collected
// up front (on a TemporalAllocator the caller holds) and each observer is
// looked up again right before it fires: one destroyed by an earlier
// callback of the batch is skipped.
void fire(World* world, const EntityId entity, const ObserverEvent event, const Event* events, const usz count) {
    for (usz i = 0; i < count; i++) {
        const Observer* observer = world->observers.find(events[i].observer);
        if (observer == nullptr) {
            continue;
        }
        observer->callback(world, entity, event, events[i].id, observer->user_data);
    }
}

// Whether `terms` (sorted by id then observer) has an entry for (id, observer).
bool has_term(const DynamicArray<ArchetypeObserverTerm>& terms, const Id id, const ObserverId observer) {
    for (usz i = term_lower_bound(terms, id); i < terms.count && terms[i].id == id; i++) {
        if (terms[i].observer == observer) {
            return true;
        }
        if (terms[i].observer > observer) {
            return false;
        }
    }
    return false;
}

// The MOVED events of `entity` arriving in `destination` from `source`: for
// every observer of `destination`, `added` if a term matches it, else
// `removed` if a term matches that (the entity still matches, one of the
// query's ids left), else, if `source` lacks the observer, the entity
// entered the result set: because `removed` was an excluded id, or, when
// nothing was removed, because of `added` (a termless query, or one with
// only without() terms, picking the entity up out of the root).
usz collect(const Archetype* source, const Archetype* destination, const Id added, const Id removed, Event* out) {
    const DynamicArray<ObserverId>& here = destination->observers.observers;
    const DynamicArray<ArchetypeObserverTerm>& here_terms = destination->observers.observer_terms;
    const DynamicArray<ObserverId>* there = source != nullptr ? &source->observers.observers : nullptr;
    const DynamicArray<ArchetypeObserverTerm>* there_terms = source != nullptr ? &source->observers.observer_terms : nullptr;

    usz j = 0;
    usz written = 0;
    for (usz i = 0; i < here.count; i++) {
        const ObserverId id = here[i];
        if (added != 0 && has_term(here_terms, added, id)) {
            out[written++] = Event { id, added };
            continue;
        }
        if (removed != 0 && there_terms != nullptr && has_term(*there_terms, removed, id)) {
            out[written++] = Event { id, removed };
            continue;
        }
        while (there != nullptr && j < there->count && (*there)[j] < id) {
            j++;
        }
        const bool was_in = there != nullptr && j < there->count && (*there)[j] == id;
        if (!was_in) {
            out[written++] = Event { id, removed != 0 ? removed : added };
        }
    }
    return written;
}

} // namespace

ObserverId create(World* world, const QueryTerm* terms, const usz term_count, const ObserverCallback callback, void* user_data) {
    if (callback == nullptr) {
        fprintf(stderr, "[ecs] error: observer needs a callback\n");
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
            fprintf(stderr, "[ecs] error: observer term %llu has id 0 (did a type fail to register?)\n", i);
            return 0;
        }
        if (!term.is_on_this() || term.is_optional() || term.is_or()) {
            fprintf(stderr, "[ecs] error: observer term %llu (%llx) is not a plain term on the matched entity; observers only support with() / without() terms\n",
                i, term.id);
            return 0;
        }
        if (term.is_excluded()) {
            without[without_count++] = term.id;
        } else {
            with[with_count++] = term.id;
        }
    }

    Observer observer;
    observer.id = world->next_observer_id++;
    observer.matcher = ArchetypeMatcher::create(world->allocator, with, with_count, without, without_count);
    observer.terms = world->allocator->allocate_array<QueryTerm>(term_count + 1);
    observer.term_count = term_count;
    for (usz i = 0; i < term_count; i++) {
        observer.terms[i] = terms[i];
    }
    observer.callback = callback;
    observer.user_data = user_data;
    // Any archetype the matcher accepts holds every `with` id, so listening
    // under the first one loses nothing and skips unrelated tables.
    observer.listener = ARCHETYPE_LISTENER::add(world, ARCHETYPE_LISTENER::key_for(with, with_count), on_archetype,
        reinterpret_cast<void*>(static_cast<usz>(observer.id)));
    // Only the archetypes holding the rarest `with` id can match; the rest
    // of the world is never tested. destroy() walks the same record.
    const ArchetypeCandidates candidates = ARCHETYPE_CANDIDATES::collect(world, with, with_count, &temp);
    observer.record_id = candidates.record_id;
    const Observer& stored = world->observers.insert(observer.id, observer);

    if (candidates.narrowed) {
        for (usz i = 0; i < candidates.count; i++) {
            Archetype* archetype = ARCHETYPE_CANDIDATES::archetype_of(world, candidates.entries[i]);
            if (archetype != nullptr && stored.matcher.matches(archetype)) {
                tag(archetype, stored);
            }
        }
        return stored.id;
    }
    for (usz i = 0; i < world->archetypes.alive_count; i++) {
        Archetype* archetype = world->archetypes.get_element_any(world->archetypes.get_alive_id(i));
        if (archetype == world->root_archetype) {
            continue;
        }
        if (stored.matcher.matches(archetype)) {
            tag(archetype, stored);
        }
    }
    return stored.id;
}

bool destroy(World* world, const ObserverId id) {
    Observer* observer = world->observers.find(id);
    if (observer == nullptr) {
        return false;
    }
    observer->matcher.free();
    world->allocator->free(observer->terms);
    ARCHETYPE_LISTENER::remove(world, observer->listener);
    const Id record_id = observer->record_id;
    world->observers.remove(id);

    // Every archetype tagged with the observer holds all of its `with` ids,
    // so the record create() narrowed through still lists them all,
    // archetypes created since included. A record that is gone means no
    // archetype holds the id any more.
    TemporalAllocator temp = TemporalAllocator::create();
    const ArchetypeCandidates candidates = ARCHETYPE_CANDIDATES::collect_for(world, record_id, &temp);
    if (candidates.narrowed) {
        for (usz i = 0; i < candidates.count; i++) {
            Archetype* archetype = ARCHETYPE_CANDIDATES::archetype_of(world, candidates.entries[i]);
            if (archetype != nullptr) {
                untag(archetype, id);
            }
        }
        return true;
    }
    for (usz i = 0; i < world->archetypes.alive_count; i++) {
        untag(world->archetypes.get_element_any(world->archetypes.get_alive_id(i)), id);
    }
    return true;
}

void fire_moved(World* world, const EntityId entity, const Archetype* source, const Archetype* destination, const Id added, const Id removed) {
    if (world->observers.is_empty() || destination == nullptr || destination->observers.observers.count == 0) {
        return;
    }
    TemporalAllocator temp = TemporalAllocator::create();
    Event* events = temp.allocate_array<Event>(destination->observers.observers.count);
    const usz count = collect(source, destination, added, removed, events);
    fire(world, entity, OBSERVER_MOVED, events, count);
}

void fire_changed(World* world, const EntityId entity, const Archetype* archetype, const Id id) {
    if (world->observers.is_empty() || archetype == nullptr || archetype->observers.observer_terms.count == 0) {
        return;
    }
    const DynamicArray<ArchetypeObserverTerm>& terms = archetype->observers.observer_terms;
    const usz start = term_lower_bound(terms, id);
    usz end = start;
    while (end < terms.count && terms[end].id == id) {
        end++;
    }
    if (start == end) {
        return;
    }

    TemporalAllocator temp = TemporalAllocator::create();
    Event* events = temp.allocate_array<Event>(end - start);
    usz count = 0;
    for (usz i = start; i < end; i++) {
        if (terms[i].output) {
            events[count++] = Event { terms[i].observer, id };
        }
    }
    fire(world, entity, OBSERVER_CHANGED, events, count);
}

} // namespace OBSERVER
