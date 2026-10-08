#pragma once

#include "engine/display_window.hpp"
#include "engine/engine.h"
#include "engine/gpu/device.hpp"
#include "engine/gpu/window_presenter.hpp"

// The standalone application: the window, the GPU device, the presenter that
// turns the window into frames, and the engine that draws them. The engine
// renders straight into the swapchain. The editor build (editor/src/app.hpp)
// is the same loop with the editor UI layered on top.
struct App {
    // Returns false if anything could not start. shutdown() is still safe to
    // call afterwards.
    bool init();
    // Runs frames until the window is closed or quit is requested.
    void run();
    void shutdown();

    const char* title = "LuauEngine";
    int width = 1280;
    int height = 720;
    // Start with the demo's post-process pass (`--post`) or shadow pass
    // (`--shadow`) on. F6 and F7 toggle them, F5 reloads every shader.
    bool demo_post = false;
    bool demo_shadow = false;

    DisplayWindow window;
    GpuDevice gpu;
    WindowPresenter presenter;
    Engine engine;

    bool running = false;

private:
    void poll_events();
    void frame(f32 dt);
};
