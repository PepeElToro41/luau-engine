#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/ecs/world.hpp"
#include "engine/scene/scene.hpp"

#include <cstring>
#include <type_traits>

// How a component shows up in the editor's Inspector panel. The information
// is a component on the component entity itself: INSPECTOR::expose<T>() sets
// an Inspector on world.component<T>(), and the panel, walking the ids of
// the selected entity's archetype, draws every id whose entity has one.
//
// Core only names the draw function; the editor defines InspectorContext and
// the functions (they are ImGui code), and registers them at startup:
//
//     INSPECTOR::expose<Transform>(world, "Transform", &INSPECTORS::transform, 0);
//
// A draw function gets a pointer to the entity's data for the component and
// returns true if it wrote to it, which the panel turns into
// World::modified(). It must not add or remove components: the pointer is
// invalidated by any structural change on the entity's archetype.
//
// Whether the panel's "Add Component" menu offers a component is a second
// component on the component entity, Addable, set by INSPECTOR::addable<T>():
// a component without one cannot be added from the editor (Shader, Material
// and the asset components are created by code, not by hand). Its init
// function writes the default value; addable<T>() defaults it to a copy of a
// value-initialized T, so a component whose declared defaults are right
// needs nothing more:
//
//     INSPECTOR::addable<Camera>(world);
//     INSPECTOR::addable<PrimitiveRenderer>(world, &default_primitive);
//
// INSPECTOR::add() is the one way the panel adds: it builds the value in
// scratch memory, zeroed then handed to init, and set()s it on the entity,
// so the added hook already sees the final value. Tags are simply add()ed.

struct InspectorContext;

using InspectorDrawFn = bool (*)(InspectorContext& ctx, void* data);
// Writes the default value of a freshly added component into `data`, which
// holds zeroed bytes of the component's size and is not yet on the entity.
// `world` and `entity` are there to look things up (a material to use),
// not to be changed: adding or removing ids on `entity` here is an error.
using InspectorInitFn = void (*)(World& world, EntityId entity, void* data);

struct Inspector {
    // Header text in the panel.
    char name[ENTITY_NAME_CAPACITY] = {};
    InspectorDrawFn draw = nullptr;
    // Components are listed lowest order first.
    u32 order = 100;
};

// On a component entity: the component may be added from the editor.
struct Addable {
    // nullptr leaves the zeroed bytes (always the case for tags).
    InspectorInitFn init = nullptr;
};

namespace INSPECTOR {

// Sets the Inspector of component `id` (a component entity, not a pair).
// Calling it again replaces the previous one. No-op for 0, pairs, or a
// nullptr draw function.
void expose(World& world, Id id, const char* name, InspectorDrawFn draw, u32 order = 100);
template <typename T>
void expose(World& world, const char* name, const InspectorDrawFn draw, const u32 order = 100) {
    INSPECTOR::expose(world, world.component<T>(), name, draw, order);
}

// The Inspector of component `id`, or nullptr if it has none, or `id` is a
// pair, 0, or not alive. Invalidated by structural changes on the component
// entity, which never happen while an entity is being drawn.
const Inspector* of(World& world, Id id);

// The init a value-initialized T gives: `T value {}` copied into the data.
template <typename T>
void default_init(World&, EntityId, void* data) {
    const T value {};
    memcpy(data, &value, sizeof(T));
}

// Marks component `id` (a component or tag entity, not a pair) as addable
// from the editor with `init` as its default value. Calling it again
// replaces the init. No-op for 0, pairs and dead ids.
void addable(World& world, Id id, InspectorInitFn init = nullptr);
// Same for T, defaulting init to default_init<T> for a type with data and
// to none for an empty one (a tag has no bytes to fill).
template <typename T>
void addable(World& world, const InspectorInitFn init = std::is_empty_v<T> ? nullptr : &INSPECTOR::default_init<T>) {
    INSPECTOR::addable(world, world.id<T>(), init);
}
// The Addable of component `id`, or nullptr if it has none, or `id` is a
// pair, 0, or not alive.
const Addable* addable_of(World& world, Id id);

// Whether add() would succeed: `id` is addable, `entity` is alive and does
// not have it yet.
bool can_add(World& world, EntityId entity, Id id);
// Adds `id` to `entity` with its default value (see above). Returns false
// and does nothing when can_add() is false.
bool add(World& world, EntityId entity, Id id);

} // namespace INSPECTOR
