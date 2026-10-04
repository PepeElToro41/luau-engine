#include "engine/ecs/archetype_listener.hpp"

#include "engine/ecs/archetype.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/world.hpp"
#include "engine/memory/temporal_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"

#include <cstdio>
#include <new>

namespace ARCHETYPE_LISTENER {

namespace {

// Appends `key` unless it is already in the list. The lists are a few
// entries long, so a linear scan beats hashing.
void push_unique(DynamicArray<Id>& keys, const Id key) {
    for (const Id existing : keys) {
        if (existing == key) {
            return;
        }
    }
    keys.push(key);
}

// Every key an archetype with `type` covers: each id, the wildcard patterns
// of each pair, (*, *) if it holds any pair, and WILDCARD. No duplicates.
void collect_keys(const ArchetypeType& type, DynamicArray<Id>& keys) {
    keys.reserve(type.id_count * 3 + 2);
    bool any_pair = false;
    for (usz i = 0; i < type.id_count; i++) {
        const Id id = type.ids[i];
        push_unique(keys, id);
        if (ECS::IS_PAIR(id)) {
            any_pair = true;
            push_unique(keys, ECS::PAIR(ECS::PAIR_FIRST(id), ECS::WILDCARD));
            push_unique(keys, ECS::PAIR(ECS::WILDCARD, ECS::PAIR_SECOND(id)));
        }
    }
    if (any_pair) {
        push_unique(keys, ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD));
    }
    push_unique(keys, ECS::WILDCARD);
}

} // namespace

ArchetypeListenerId add(World* world, const Id key, const ArchetypeListenerCallback callback, void* user_data) {
    if (callback == nullptr) {
        fprintf(stderr, "[ecs] error: archetype listener needs a callback\n");
        return 0;
    }
    if (key == 0) {
        fprintf(stderr, "[ecs] error: archetype listener registered under id 0\n");
        return 0;
    }
    const Id folded = ECS::FOLD_ANY(key);

    ArchetypeListenerList** slot = world->archetype_listeners.find(folded);
    ArchetypeListenerList* list = nullptr;
    if (slot != nullptr) {
        list = *slot;
    } else {
        list = world->allocator->allocate_array<ArchetypeListenerList>(1);
        new (list) ArchetypeListenerList(world->allocator);
        world->archetype_listeners.insert(folded, list);
    }

    ArchetypeListener listener;
    listener.id = world->next_archetype_listener_id++;
    listener.callback = callback;
    listener.user_data = user_data;
    list->listeners.push(listener);
    world->archetype_listener_keys.insert(listener.id, folded);
    return listener.id;
}

bool remove(World* world, const ArchetypeListenerId id) {
    const Id* key = world->archetype_listener_keys.find(id);
    if (key == nullptr) {
        return false;
    }
    ArchetypeListenerList** slot = world->archetype_listeners.find(*key);
    if (slot == nullptr) {
        // The two maps are kept in step; this is a bug if it happens.
        world->archetype_listener_keys.remove(id);
        return false;
    }
    ArchetypeListenerList* list = *slot;
    for (usz i = 0; i < list->listeners.count; i++) {
        if (list->listeners[i].id == id) {
            list->listeners.remove_at(i);
            break;
        }
    }
    if (list->listeners.is_empty()) {
        world->archetype_listeners.remove(*key);
        list->listeners.free();
        world->allocator->free(list);
    }
    world->archetype_listener_keys.remove(id);
    return true;
}

Id key_for(const Id* with, const usz with_count) {
    for (usz i = 0; i < with_count; i++) {
        if (with[i] != 0) {
            return ECS::FOLD_ANY(with[i]);
        }
    }
    return ECS::WILDCARD;
}

void fire(World* world, Archetype* archetype, const ArchetypeEvent event) {
    if (world->archetype_listeners.is_empty()) {
        return;
    }

    // A callback may add or remove listeners, which edits (or frees) the
    // lists being walked, so the listeners to run are copied out first and
    // each is checked to still exist right before it fires. A listener added
    // during the walk does not run for this archetype.
    TemporalAllocator temp = TemporalAllocator::create();
    DynamicArray<Id> keys(&temp);
    collect_keys(archetype->type, keys);

    // Resolve each key once; the lists are then sized before anything is
    // copied, since growing an array on the arena cannot reuse its old block.
    DynamicArray<const ArchetypeListenerList*> lists(&temp);
    lists.reserve(keys.count);
    
    usz total = 0;
    for (const Id key : keys) {
        ArchetypeListenerList** slot = world->archetype_listeners.find(key);
        if (slot == nullptr) {
            continue;
        }
        lists.push(*slot);
        total += (*slot)->listeners.count;
    }
    if (total == 0) {
        return;
    }

    DynamicArray<ArchetypeListener> batch(&temp);
    batch.reserve(total);
    for (const ArchetypeListenerList* list : lists) {
        for (const ArchetypeListener& listener : list->listeners) {
            batch.push(listener);
        }
    }

    for (const ArchetypeListener& listener : batch) {
        if (!world->archetype_listener_keys.contains(listener.id)) {
            continue;
        }
        listener.callback(world, archetype, event, listener.user_data);
    }
}

void free_all(World* world) {
    for (auto& entry : world->archetype_listeners) {
        entry.value->listeners.free();
        world->allocator->free(entry.value);
    }
    world->archetype_listeners.free();
    world->archetype_listener_keys.free();
    world->next_archetype_listener_id = 1;
}

} // namespace ARCHETYPE_LISTENER
