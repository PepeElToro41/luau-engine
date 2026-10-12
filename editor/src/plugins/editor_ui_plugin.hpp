#pragma once

#include "editor.hpp"
#include "engine/plugin.hpp"
#include "engine/scheduler/scheduler.hpp"

// ImGui and the editor's panels as a plugin. build creates the ImGui
// context and its SDL3 / Vulkan backends on the window plugin's window and
// Engine::gpu (through vk_access.hpp, the one Vulkan-specific piece of the
// editor), registers the Inspector draw functions, makes the Output panel
// the renderer's log sink and takes the window close over from the window
// plugin (`quit_on_close` off: the Unsaved Scene prompt decides).
//
// Systems: "events" feeds every SDL event to ImGui and turns quit / close
// into Editor::request_quit; "post_startup" records the loaded scene as
// the saved state; "ui", a phase this plugin adds after post_render, draws
// the panels and renders ImGui into the frame's target, which the
// viewport plugin has pointed back at the backbuffer by then;
// "pre_shutdown" gives the log sink back; "shutdown" destroys the backends
// and the context. Requires WindowPlugin, CorePlugin, RenderPlugin and
// EditorCameraPlugin (the Explorer lists the camera entity).
struct EditorUiPlugin : Plugin {
    static constexpr const char* NAME = "editor_ui";
    static constexpr const char* UI_PHASE = "ui";

    bool build(Engine& engine) override;

    Editor editor;
    PhaseHandle ui_phase;

private:
    bool init_imgui(Engine& engine);

    static void handle_events(SystemContext& ctx);
    static void started(SystemContext& ctx);
    static void draw(SystemContext& ctx);
    static void detach(SystemContext& ctx);
    static void shutdown(SystemContext& ctx);

    bool context_ready = false;
    bool sdl_backend_ready = false;
    bool vulkan_backend_ready = false;
};
