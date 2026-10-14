#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/utils/type_id.hpp"

#include <type_traits>

// Process-wide component ids: the preregistered tier.
//
// A World hands component ids out of the band [1, ECS::MAX_COMPONENT_ID] in
// two tiers. A type claimed here, through preregister<T>(), gets an id from
// a library-level counter that every World in the process agrees on, so a
// world cloned from another (WORLD_CLONE) and anything keyed by a component
// id outside a world keep meaning the same thing. A type nobody
// preregistered is claimed by each World on its own, from a counter that
// grows down from the top of the band (World::runtime_next_id), so the two
// tiers can never meet until the band is genuinely full:
//
//     [1 ---------> count()  ...  World::runtime_next_id <--------- MAX_COMPONENT_ID]
//      preregistered, process-wide           runtime, world-local, grows down
//
// Preregister at engine startup, from the code that owns the type
// (RENDER_COMPONENTS::register_all, ASSET_ENTITY::register_components, ...):
//
//     COMPONENT_REGISTRY::preregister<Transform>("Transform");
//
// Later is still correct (a world that already handed out runtime ids
// never collides with a new preregistration: they grow from opposite ends,
// and World::bind_component checks every binding against the id's current
// owner rather than trusting it), only pointless, since worlds that already
// claimed the type as a runtime component keep their own id. The decision
// whether T carries data (component<T>) or none (tag<T>) is made once here,
// by the first claim, and every world follows it.
//
// A fixed table, no allocation, no destructor, usable before main.
// Single-threaded like the rest of core: registering a component from a job
// would need the counter made atomic.

struct ComponentRegistryEntry {
    TypeId type_id = 0;
    Id id = 0;
    // Zero length for tags and for empty types.
    TypeInfo info {};
    // Claimed as a tag: no (COMPONENT, info) is stored on the entity at all.
    bool is_tag = false;
    // Only set by preregister<T>(name); a string literal, never copied.
    const char* name = nullptr;
};

namespace COMPONENT_REGISTRY {

// Claims the next id for `type_id`, or returns the id it already has (the
// first claim decides `info`, `is_tag` and `name`; later ones are
// ignored). 0 and a LOG_CATEGORY::ECS error once ECS::MAX_COMPONENT_ID
// types are registered.
Id claim(TypeId type_id, TypeInfo info, bool is_tag, const char* name = nullptr);

// Claims T the way World::id<T>() would register it: a component with
// sizeof / alignof for a type with data, a tag for an empty type.
template <typename T>
Id preregister(const char* name) {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>, "preregister<T>: pass the plain type, without const / volatile / reference");
    TypeInfo info {};
    if constexpr (!std::is_empty_v<T>) {
        info.length = sizeof(T);
        info.alignment = alignof(T);
    }
    return COMPONENT_REGISTRY::claim(TYPE_ID::get<T>(), info, std::is_empty_v<T>, name);
}

// Claims T as a tag whatever its size, as World::tag<T>() would.
template <typename T>
Id preregister_tag(const char* name) {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>, "preregister_tag<T>: pass the plain type, without const / volatile / reference");
    return COMPONENT_REGISTRY::claim(TYPE_ID::get<T>(), TypeInfo {}, true, name);
}

// The entry for a type / an id / a preregistered name, or nullptr. Pointers
// stay valid until reset().
const ComponentRegistryEntry* find(TypeId type_id);
const ComponentRegistryEntry* find_id(Id id);
const ComponentRegistryEntry* find_name(const char* name);
template <typename T>
const ComponentRegistryEntry* find() {
    return COMPONENT_REGISTRY::find(TYPE_ID::get<T>());
}

// Registered types; entry(i).id == i + 1.
usz count();
const ComponentRegistryEntry& entry(usz index);

// Forgets every registration. For tests only, and only while no World is
// alive: a world's bindings would keep pointing at ids the registry no
// longer knows.
void reset();

} // namespace COMPONENT_REGISTRY
