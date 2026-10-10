#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/scene/scene.hpp"
#include "project.hpp"
#include "selection.hpp"
#include "ui/inspectors/inspector_context.hpp"

#include <imgui.h>

#include <filesystem>

struct World;

// The editor's Selection (selection.hpp). For an entity: the header edits
// the Name; below it, every id of the entity's archetype whose component
// entity carries an Inspector (see engine/scene/inspector.hpp) gets a
// collapsing section drawn by its draw function, lowest `order` first, and
// a change reported by the function becomes World::modified(). The ids
// without one (pairs such as (CHILD_OF, parent), tags, components nothing
// exposed) are listed at the bottom so nothing is hidden. Below them an
// "Add Component" button opens a menu of every component with an Addable
// (engine/scene/inspector.hpp), the ones the entity already holds greyed
// out; a pick calls INSPECTOR::add, which sets the component's default.
//
// For a file: its name, folder, kind and size, and when the file is loaded
// as an entity (a .material) the Save and Reload buttons followed by that
// entity's component sections, the same way. The buttons only set
// `file_action`; the App performs it after the draw and resets it.
enum struct InspectorFileAction : u8 {
    NONE,
    SAVE,
    RELOAD,
};

struct InspectorPanel {
    // Submits the window. `open` is the View-menu toggle. `project` is the
    // open project (null or closed: file selections show only their path).
    void draw(bool* open, World* world, const Project* project, const Selection& selection);

    // What the file header's button asked for during the last draw().
    InspectorFileAction file_action = InspectorFileAction::NONE;

private:
    // Scratch slots the draw functions keep edit state in, one per id of
    // the current entity; all dropped when the selection changes.
    static constexpr usz SCRATCH_SLOTS = 16;
    struct Scratch {
        Id id = 0;
        alignas(16) u8 bytes[INSPECTOR_SCRATCH_CAPACITY] = {};
    };

    void draw_entity(World& world, EntityId entity);
    void draw_header(World& world, EntityId entity);
    void draw_components(World& world, EntityId entity);
    // The "Add Component" button and its menu; adds after the menu closed,
    // so no section drawn this frame still points into the old archetype.
    void draw_add_component(World& world, EntityId entity);
    void draw_file(World* world, const Project* project, const Selection& selection);
    // Finds or claims the scratch slot for `id`, zeroing it when new.
    // nullptr once every slot is taken (the draw function then gets no
    // persistent state and must cope with a fresh one each frame).
    void* scratch_for(Id id, bool* fresh);
    // The text for an id without an Inspector: "(CHILD_OF, cube)" for pairs,
    // the entity's Name or "component <id>" otherwise.
    static void label_of_id(World& world, Id id, char* out, usz capacity);

    EntityId scratch_entity = 0;
    Scratch scratch[SCRATCH_SLOTS];
    usz scratch_count = 0;
    // Per-frame fallback for draw functions when the slots are exhausted.
    alignas(16) u8 scratch_spill[INSPECTOR_SCRATCH_CAPACITY] = {};

    // The Name field's text and the entity it was copied from.
    char name_buffer[ENTITY_NAME_CAPACITY] = {};
    EntityId name_entity = 0;

    // The file whose facts are cached below, read when the selection changes.
    std::filesystem::path file_shown;
    bool file_is_directory = false;
    bool file_exists = false;
    u64 file_size = 0;
};
