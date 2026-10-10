#pragma once

#include "engine/display_window.hpp"
#include "engine/engine.hpp"
#include "engine/gpu/gpu.hpp"

// The standalone application: a window, the GPU and the engine drawing
// straight into the swapchain. F5 reloads the shaders, F6 toggles the demo
// post-process pass, F7 the demo shadow pass (also --post / --shadow).
struct App {
    bool init();
    void run();
    void shutdown();

    const char* title = "LuauEngine";
    int width = 1280;
    int height = 720;
    bool demo_post = false;
    bool demo_shadow = false;

    DisplayWindow window;
    GpuContext* gpu = nullptr;
    Engine engine;
    bool running = false;

private:
    void poll_events();
    void frame(f32 dt);
};
