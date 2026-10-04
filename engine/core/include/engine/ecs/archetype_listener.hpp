#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/templates/dynamic_array.hpp"

struct Archetype;
struct World;

// Archetype lifecycle listeners: a callback for archetypes being created and
// destroyed, for anything that keeps per-archetype state over the graph
// (monitors, cached queries, observers).
//
//     static void on_archetype(World* world, Archetype* archetype, ArchetypeEvent event, void* user_data);
//
//     ArchetypeListenerId id = ARCHETYPE_LISTENER::add(world, world->id<Position>(), on_archetype, &state);
//     ...
//     ARCHETYPE_LISTENER::remove(world, id);
//
// Listeners are registered under an id and only fire for archetypes whose
// type covers that id, so a listener that only cares about tables holding
// Position never hears about tables that do not. When an archetype is
// created (or destroyed) its ids are walked once and the listeners under
// each are fired, plus those under the patterns the id falls in: a pair
// (R, T) also fires (R, *), (*, T) and (*, *), and every archetype fires
// WILDCARD (ANY is folded to WILDCARD on registration). Each listener fires
// at most once per archetype, however many of its ids reach the same key.
//
// Anything with an ArchetypeMatcher can listen under the first of its `with`
// ids: an archetype the matcher accepts holds all of them, so it certainly
// holds the first. ARCHETYPE_LISTENER::key_for picks that id, or WILDCARD
// when there is no `with` side. The callback still has to run the full
// matcher; the key is only a filter that keeps unrelated tables cheap.
//
// ARCHETYPE_CREATED fires from Archetype::create_archetype once the
// archetype is complete (columns, signature, world index) and before any
// entity sits in it. ARCHETYPE_DESTROYED fires from Archetype::destroy
// before anything is unlinked, with the archetype still intact and empty.
// World::free tears everything down at once and fires nothing. A callback
// may add or remove listeners, including its own; one removed before its
// turn is skipped.

enum ArchetypeEvent : u32 {
    ARCHETYPE_CREATED = 0,
    ARCHETYPE_DESTROYED = 1,
};

using ArchetypeListenerCallback = void (*)(World* world, Archetype* archetype, ArchetypeEvent event, void* user_data);
// 0 is never issued.
using ArchetypeListenerId = u64;

struct ArchetypeListener {
    ArchetypeListenerId id = 0;
    ArchetypeListenerCallback callback = nullptr;
    void* user_data = nullptr;
};

// Every listener registered under one key, in registration order. Allocated
// on the world's allocator by ARCHETYPE_LISTENER::add and released when its
// last listener goes.
struct ArchetypeListenerList {
    DynamicArray<ArchetypeListener> listeners;

    explicit ArchetypeListenerList(BaseAllocator* allocator) : listeners(allocator) {}
};

namespace ARCHETYPE_LISTENER {

// Registers `callback` under `key` (an id, a pair, a pattern or WILDCARD; ANY
// sides are folded to WILDCARD). Returns the listener's id, or 0 with an
// error printed for a null callback or a 0 key.
ArchetypeListenerId add(World* world, Id key, ArchetypeListenerCallback callback, void* user_data);
// Unregisters the listener. False if no listener has that id.
bool remove(World* world, ArchetypeListenerId id);

// The key a matcher built from `with` should listen under: the first
// non-zero id of `with` folded to its WILDCARD spelling, or WILDCARD itself
// when there is none.
Id key_for(const Id* with, usz with_count);

// Fires `event` on every listener whose key `archetype` covers. Called by
// Archetype::create_archetype and Archetype::destroy; returns at once when
// the world has no listeners.
void fire(World* world, Archetype* archetype, ArchetypeEvent event);

// Releases every list; World::free calls it.
void free_all(World* world);

} // namespace ARCHETYPE_LISTENER
