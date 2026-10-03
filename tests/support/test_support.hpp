#pragma once

// Shared helpers for engine tests. Keep this small: sample component types,
// an arena-leak check and a hook recorder. World setup stays explicit in each
// test (World world; world.init(); ... world.free();) to match the engine's
// explicit-cleanup style, so there are no RAII fixtures here.

#include "engine/defines.hpp"
#include "engine/ecs/hooks.hpp"
#include "engine/ecs/world.hpp"
#include "engine/memory/temporal_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"

#include <doctest.h>

// --- Sample types ------------------------------------------------------------

struct Position {
    f32 x = 0;
    f32 y = 0;
};

struct Velocity {
    f32 dx = 0;
    f32 dy = 0;
};

struct Health {
    i32 value = 0;
};

// Empty types register as tags.
struct TagA {};
struct TagB {};
struct Likes {};
struct Eats {};

// --- Arena leak check --------------------------------------------------------

// Every TemporalAllocator the engine creates must have rewound MAIN_ARENA by
// the time a test finishes. Call after world.free().
#define CHECK_ARENA_CLEAN() CHECK(MAIN_ARENA.offset == 0)

// --- Hook recording ----------------------------------------------------------

struct HookEvent {
    HookKind kind;
    EntityId entity;
    Id id;
};

// Pass `&log` as user_data and `HookLog::record_added` etc. as the callback.
struct HookLog {
    DynamicArray<HookEvent> events;

    static void record_added(World* world, EntityId entity, Id id, void* user_data) {
        (void)world;
        static_cast<HookLog*>(user_data)->events.push({HOOK_ADDED, entity, id});
    }
    static void record_removed(World* world, EntityId entity, Id id, void* user_data) {
        (void)world;
        static_cast<HookLog*>(user_data)->events.push({HOOK_REMOVED, entity, id});
    }
    static void record_changed(World* world, EntityId entity, Id id, void* user_data) {
        (void)world;
        static_cast<HookLog*>(user_data)->events.push({HOOK_CHANGED, entity, id});
    }

    usz count_of(HookKind kind) const {
        usz n = 0;
        for (const HookEvent& e : this->events) {
            if (e.kind == kind) {
                n += 1;
            }
        }
        return n;
    }

    void free() { this->events.free(); }
};
