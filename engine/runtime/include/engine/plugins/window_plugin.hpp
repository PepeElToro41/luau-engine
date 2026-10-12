#pragma once

#include "engine/display_window.hpp"
#include "engine/gpu/gpu_types.hpp"
#include "engine/plugin.hpp"
#include "engine/scheduler/scheduler.hpp"
#include "engine/templates/dynamic_array.hpp"

#include <SDL3/SDL.h>

// The window, the GPU and the run loop: SDL, a DisplayWindow, GPU::init
// into Engine::gpu, and the runner Engine::run() calls. Every other plugin
// that needs the GPU requires this one (RenderPlugin does).
//
// The runner is the loop both apps used to write by hand: per frame it
// polls SDL into `events`, calls Engine::update(dt), begins a GPU frame,
// calls Engine::render with the backbuffer as the target and ends the
// frame; it returns once Engine::quit() was called, with the GPU idle.
// Another plugin changes what is drawn where by editing the FrameContext
// from the render pipeline (an editor retargets it at a viewport texture
// in pre_render and draws its UI into the backbuffer after).
//
// Events are data: `events` holds this frame's SDL events, cleared and
// refilled by the runner before the update pipeline, so a system in the
// "events" phase reads them (engine.get_plugin<WindowPlugin>()->events).
// The plugin's own "events" system turns SDL_EVENT_QUIT and the window's
// close request into Engine::quit() while `quit_on_close` is set (an
// editor clears it to ask about unsaved work first) and a pixel-size
// change into GPU::mark_resized.
//
// Teardown: a "gpu_idle" phase added before pre_shutdown waits for the
// GPU, so the releases the other plugins' pre_shutdown systems cause are
// safe; the post_shutdown system destroys the GPU context, the window
// and SDL, after everything that used them.
struct WindowDesc {
    const char* title = "LuauEngine";
    int width = 1280;
    int height = 720;
    // GpuInitDesc::swapchain_format: _SRGB for linear-light output, _UNORM
    // for already-encoded colors (an ImGui editor).
    GpuFormat swapchain_format = GPU_FORMAT_BGRA8_SRGB;
};

struct WindowPlugin : Plugin {
    static constexpr const char* NAME = "window";
    static constexpr const char* GPU_IDLE_PHASE = "gpu_idle";

    WindowPlugin() = default;
    explicit WindowPlugin(const WindowDesc& desc) : desc(desc) {}

    bool build(Engine& engine) override;

    WindowDesc desc;
    DisplayWindow window;
    // This frame's SDL events, in the order they arrived.
    DynamicArray<SDL_Event> events;
    bool quit_on_close = true;
    PhaseHandle gpu_idle;

private:
    static void run(Engine& engine);
    static void handle_events(SystemContext& ctx);
    static void wait_idle(SystemContext& ctx);
    static void shutdown(SystemContext& ctx);

    void poll();
    bool sdl_ready = false;
};
