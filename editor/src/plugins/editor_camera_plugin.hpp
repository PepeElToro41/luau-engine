#pragma once

#include "editor_camera.hpp"
#include "engine/plugin.hpp"
#include "engine/scheduler/scheduler.hpp"

// The Viewport's fly camera as a plugin (editor_camera.hpp): build spawns
// the unparented editor_camera entity and points Renderer::camera at it,
// so a scene file's RenderCamera tag is left for the standalone; the
// "events" system feeds the wheel from the window plugin's events and the
// "update" system reads SDL's input state and moves the camera. Fly mode
// only begins while the mouse is over the Viewport image, which the UI
// sets in `viewport_hovered` when it draws the panel. Requires
// WindowPlugin, CorePlugin and RenderPlugin.
struct EditorCameraPlugin : Plugin {
    static constexpr const char* NAME = "editor_camera";

    bool build(Engine& engine) override;

    EditorCameraController controller;
    // Whether the mouse was over the Viewport image when the UI was last
    // drawn; where right-click starts fly mode.
    bool viewport_hovered = false;

private:
    static void handle_events(SystemContext& ctx);
    static void update(SystemContext& ctx);
};
