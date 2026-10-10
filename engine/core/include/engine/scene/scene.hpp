#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/templates/dynamic_array.hpp"

struct World;

// The scene hierarchy: entities named with a Name component and parented
// through the ECS::CHILD_OF relation (exclusive, so an entity has at most one
// parent; traversable, so queries can walk up it; deleting a parent deletes
// its subtree, see entity_cleanup.hpp).
//
// SCENE:: are the helpers over any World: parent / set_parent / children
// read and write (CHILD_OF, parent) pairs, name / set_name the Name
// component, spawn does both in one go. Nothing here knows about a root.
//
// Scene is the one tree the simulation draws and the editor shows: init()
// creates its root entity, "scene_root", and Scene::spawn parents under it
// by default. Entities that are not scene content (the renderer's shaders
// and materials, for instance) simply have no parent, so a walk down from
// `root` only ever meets scene content. The runtime Engine creates the
// Scene as a singleton in init(); the root goes away with the World.

// Capacity of a Name, terminator included; longer names are cut.
constexpr usz ENTITY_NAME_CAPACITY = 64;

// Display name of an entity. Plain data (the World copies it); not unique
// and never used to look entities up.
struct Name {
    char value[ENTITY_NAME_CAPACITY] = {};

    // A Name holding `text`, truncated to fit. nullptr gives an empty name.
    static Name make(const char* text);
};

namespace SCENE {

// The entity's CHILD_OF target (with generation), or 0 if it has no parent
// or is not alive.
EntityId parent(World& world, EntityId entity);
// Parents `entity` under `parent`, replacing any current parent since
// CHILD_OF is exclusive; a `parent` of 0 detaches it. No-op if either
// entity is not alive, or if `parent` is `entity` itself or one of its
// descendants, since that would close a cycle (a warning is printed).
void set_parent(World& world, EntityId entity, EntityId parent);
// Whether `ancestor` is on the parent chain of `entity` (not `entity`
// itself). False if either is 0 or not alive.
bool is_descendant_of(World& world, EntityId entity, EntityId ancestor);

// Appends the direct children of `parent` to `out`, lowest entity id first
// so the order is stable from frame to frame, and returns how many were
// added. `out` is not cleared.
usz children(World& world, EntityId parent, DynamicArray<EntityId>& out);
// Number of direct children of `parent`.
usz child_count(World& world, EntityId parent);
// Whether `parent` has at least one child.
bool has_children(World& world, EntityId parent);

// The entity's name, or nullptr if it has no Name component or is not
// alive. The pointer is invalidated by any structural change on the
// entity's archetype, so copy it if it has to outlive the next add/remove.
const char* name(World& world, EntityId entity);
// Sets (or adds) the entity's Name. No-op if the entity is not alive.
void set_name(World& world, EntityId entity, const char* name);

// A new entity with Name `name` parented under `parent`, or without a
// parent when `parent` is 0. Returns 0 if the entity could not be created
// (index exhausted). A `parent` that is not alive leaves the entity
// unparented.
EntityId spawn(World& world, const char* name, EntityId parent);

} // namespace SCENE

struct Scene {
    World* world = nullptr;
    // The "scene_root" entity every scene entity descends from.
    EntityId root = 0;

    // Creates the root entity in `world`, which must be initialized and
    // outlive the Scene. There is nothing to free: the root is an ordinary
    // entity released with the World.
    void init(World* world);

    // A new named entity under `parent`, or under the root when `parent` is
    // 0. Returns 0 if the entity could not be created.
    EntityId spawn(const char* name, EntityId parent = 0);
    // Whether `entity` is the root or descends from it.
    bool contains(EntityId entity);
};
