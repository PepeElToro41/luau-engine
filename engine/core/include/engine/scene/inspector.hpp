#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/ecs/world.hpp"
#include "engine/scene/scene.hpp"

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

struct InspectorContext;

using InspectorDrawFn = bool (*)(InspectorContext& ctx, void* data);

struct Inspector {
    // Header text in the panel.
    char name[ENTITY_NAME_CAPACITY] = {};
    InspectorDrawFn draw = nullptr;
    // Components are listed lowest order first.
    u32 order = 100;
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

} // namespace INSPECTOR
