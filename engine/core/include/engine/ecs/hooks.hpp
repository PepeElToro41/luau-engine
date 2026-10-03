#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"

struct World;

// Callbacks that run when an id is added to, removed from, or written on an
// entity. `id` is the concrete id the event is about, so a hook registered on
// (R, *) still learns which (R, T) was touched.
//
// Hooks are fired by the ENTITY operations (add / remove / set / destroy), not
// by archetype storage, so moving rows around never fires anything on its
// own. Added and changed callbacks run after the operation is complete and
// may do anything, including adding to, removing from or destroying the
// entity. Removed callbacks run before the id (or the entity) goes away, with
// the data still readable, and must not add to, remove from or destroy the
// entity they are told about: the caller is in the middle of its move.
// Reading it, or changing other entities, is always fine.
using HookCallback = void (*)(World* world, EntityId entity, Id id, void* user_data);
using HookId = u64;

enum HookKind : u32 {
    // The entity gained the id. From set() the data has been written and no
    // changed event follows; from add(), which is for tags, any data has
    // been zeroed (and a warning printed).
    HOOK_ADDED = 0,
    // The entity is about to lose the id (data still readable): remove(),
    // delete_entity(), or an exclusive relation swapping its target.
    HOOK_REMOVED = 1,
    // set() overwrote data the entity already had, or modified() was called
    // after the data was written in place through get().
    HOOK_CHANGED = 2,
    HOOK_KIND_COUNT = 3,
};

struct Hook {
    HookCallback callback = nullptr;
    void* user_data = nullptr;
    HookId id = 0;
};

// One list per kind. Allocated lazily by the owner (a ComponentRecord or the
// World) so records without hooks pay a single pointer.
struct HookList {
    DynamicArray<Hook> lists[HOOK_KIND_COUNT];

    void initialize(BaseAllocator* allocator);
    void free();

    bool is_empty(const HookKind kind) const { return this->lists[kind].count == 0; }

    void add(HookKind kind, HookCallback callback, void* user_data, HookId id);
    // Removes the hook with `id` from whichever list holds it. `out_kind`
    // receives the kind it was registered under. False if not found.
    bool remove(HookId id, HookKind* out_kind);

    // Runs every hook of `kind` in registration order. Hooks may register or
    // unregister hooks of this same list while it runs (including themselves):
    // a hook removed before its turn is skipped, one added runs in this pass.
    void fire(HookKind kind, World* world, EntityId entity, Id id) const;
};

namespace HOOKS {

// Fires every hook of `kind` that covers `id`: the id's own hooks, for a pair
// (R, T) also those on (R, *), (*, T) and (*, *), and the world-wide hooks on
// WILDCARD. Returns immediately if no hook of that kind is registered anywhere.
void fire(World* world, HookKind kind, EntityId entity, Id id);

} // namespace HOOKS
