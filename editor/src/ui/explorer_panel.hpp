#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "selection.hpp"

#include <imgui.h>

struct Scene;
struct World;

// The scene tree: the Scene's "scene_root" entity at the top, and under
// every node its ECS::CHILD_OF children (see engine/scene/scene.hpp),
// listed lowest id first and read fresh from the World each frame. A node
// shows its Name component, or its id when it has none; hovering shows the
// id. The filter box keeps the nodes whose name matches and every ancestor
// of those, opened so the matches are visible. Below the scene tree, after a
// separator, come the editor's own entities (`editor_camera`): they have no
// parent, so they are not scene content, but their Transform is still worth
// reaching from the Inspector. Clicking a row makes it the editor's
// Selection (selection.hpp); a selected entity that dies clears it.
struct ExplorerPanel {
    // Submits the window. `open` is the View-menu toggle. Without a world or
    // scene the panel says so instead of listing anything.
    void draw(bool* open, World* world, Scene* scene, Selection& selection);

    ImGuiTextFilter filter;
    // The editor's camera entity (see editor_camera.hpp), listed as its own
    // top-level node under the scene tree; 0 or dead lists nothing.
    EntityId editor_camera = 0;

private:
    // Draws `entity`'s row and, if open, its children. `stack` is the
    // scratch the children lists are appended to; every level truncates it
    // back on the way out, so the slice it uses stays valid while it recurses.
    void draw_entity(World& world, EntityId entity, bool is_root, DynamicArray<EntityId>& stack, Selection& selection);
    // Whether `entity` or any descendant passes the filter.
    bool subtree_matches(World& world, EntityId entity, DynamicArray<EntityId>& stack);
    // The row's text: the Name, or "entity <id>" without one.
    static void label_of(World& world, EntityId entity, char* out, usz capacity);
};
