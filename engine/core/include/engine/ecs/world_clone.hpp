#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/ecs/world.hpp"

// Cloning a World id for id.
//
// WORLD_CLONE::clone(source, destination) copies every alive entity of
// `source` into `destination`, a freshly init()ed world, under the exact
// same id and generation (World::make_alive), with the same component ids
// (the two tiers of component_registry.hpp are carried over) and the same
// archetype per entity, column by column. Afterwards an EntityId means the
// same object in both worlds, so everything keyed by entity id outside the
// world (a pipeline cache keyed by shader entity, an asset index, a
// selection) stays valid without being re-pointed, and both worlds can be
// read through the same component metadata. It is what Play in the editor
// does: the game runs in the clone, Stop frees it, and the authoring world
// was only ever read.
//
// Components are copied by memcpy, which is right for plain data and wrong
// for the few components that own memory through a pointer: a memcpy
// aliases it, and two worlds would free it. Such a component declares how
// it is copied with a Cloner on its component entity, the same shape as
// Serializer, Inspector and Addable:
//
//     static void material_clone(const void* source, void* destination, usz size, void*) {
//         memcpy(destination, source, size);                  // then deep-copy the owned block
//         ...
//     }
//     CLONE::expose<Material>(world, &material_clone);
//
// A Cloner that only does the memcpy is how a component says "share": the
// pointer is copied and whoever owns it must know two worlds hold it (the
// shader program the play world borrows and never frees). The lookup is on
// the source world, so expose the Cloner where the component is registered.
//
// What the clone does not carry: hooks, observers, monitors, cached
// queries and archetype listeners (they are per-world registrations, and
// the destination's owner installs its own), the world's clock, and dead
// ids (a stale handle is dead in both worlds).
//
// Order of work, since component records snapshot the traits and sizes of
// the ids they are created for: every entity is made alive first, then the
// (COMPONENT, TypeInfo) of every component entity is set, then the trait
// ids of every relation (EXCLUSIVE, TRAVERSABLE, the deletion policies), and
// only then is each archetype of the source rebuilt in the destination and
// its rows copied in bulk, each placement told to the hierarchy caches the
// way ENTITY::move tells them.
//
// With `fence` set, the destination is left issuing new ids above every id
// it was given (World::set_range(0, 0)), so an id at or above
// WorldCloneResult::fence was created after the clone: a cheap "runtime
// entity" test, and the reason a cloned id deleted in the destination is
// never reissued (it falls in the range below the active one, which stays
// inactive).

// Copies one value of `size` bytes from a source world's column into the
// destination's. `destination` holds uninitialized bytes.
using CloneFn = void (*)(const void* source, void* destination, usz size, void* user_data);

// On a component entity: how the component is cloned instead of by memcpy.
struct Cloner {
    CloneFn clone = nullptr;
    void* user_data = nullptr;
};

namespace CLONE {

// Sets the Cloner of component `id` (a component entity, not a pair).
// Calling it again replaces the previous one. No-op for 0, pairs, dead ids
// and a nullptr function.
void expose(World& world, Id id, CloneFn clone, void* user_data = nullptr);
template <typename T>
void expose(World& world, const CloneFn clone, void* user_data = nullptr) {
    CLONE::expose(world, world.component<T>(), clone, user_data);
}

// The Cloner of component `id`, or nullptr if it has none, or `id` is a
// pair, 0 or not alive. Never registers anything on `world`.
const Cloner* of(World& world, Id id);

} // namespace CLONE

struct WorldCloneDesc {
    // Leave the destination issuing ids above every cloned one.
    bool fence = true;
};

struct WorldCloneResult {
    bool ok = false;
    // Alive entities copied (the built-ins among them) and archetypes with
    // rows rebuilt.
    usz entities = 0;
    usz archetypes = 0;
    // The first id the destination issues after the clone; 0 without a fence.
    EntityIdLow fence = 0;
    // Why `ok` is false; a static string.
    const char* error = nullptr;
};

namespace WORLD_CLONE {

// Clones `source` into `destination`, which must be another World, init()ed
// and untouched since (no entity created, no component registered).
// Nothing is written to `source`. On failure the result says why, with an
// error logged, and the destination may hold a partial copy: free() it.
WorldCloneResult clone(World& source, World& destination, const WorldCloneDesc& desc = WorldCloneDesc {});

} // namespace WORLD_CLONE
