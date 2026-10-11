#pragma once

#include "engine/display_window.hpp"
#include "engine/engine.hpp"
#include "engine/gpu/gpu.hpp"

#include <filesystem>
#include <string>

// The standalone application: a window, the GPU and the engine drawing
// straight into the swapchain. It opens a project directory (`--project
// <dir>`, the editor's testing project by default), registers every asset
// file under it and loads a scene file from it (`--scene <path>`, relative
// to the project, scenes/demo.scene by default); without a loadable scene
// it spawns the built-in demo. The frame is drawn from the scene camera
// tagged RenderCamera, else the first camera under scene_root. F5 reloads
// the shaders, F6 toggles the demo post-process pass, F7 the demo shadow
// pass (also --post / --shadow). `--cooked` makes the asset provider refuse
// uncooked files, as a shipped build would.
struct App {
    bool init();
    void run();
    void shutdown();

    const char* title = "LuauEngine";
    int width = 1280;
    int height = 720;
    bool demo_post = false;
    bool demo_shadow = false;
    bool cooked_only = false;
    std::filesystem::path project_dir;
    std::filesystem::path scene_path = "scenes/demo.scene";

    DisplayWindow window;
    GpuContext* gpu = nullptr;
    Engine engine;
    bool running = false;

private:
    // Registers every .lunaasset and text asset under the project with the
    // engine and makes the project the shader include root.
    void scan_project();
    // Loads `scene_path` as the scene; false (with a message) when it is
    // missing or did not load.
    bool load_scene();
    void poll_events();
    void frame(f32 dt);
};
