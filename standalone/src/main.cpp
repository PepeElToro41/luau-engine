#include "engine/engine.hpp"
#include "engine/plugins/core_plugin.hpp"
#include "engine/plugins/demo_plugin.hpp"
#include "engine/plugins/project_plugin.hpp"
#include "engine/plugins/render_plugin.hpp"
#include "engine/plugins/scene_plugin.hpp"
#include "engine/plugins/window_plugin.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>

#ifndef STANDALONE_DEFAULT_PROJECT_DIR
#define STANDALONE_DEFAULT_PROJECT_DIR "."
#endif

// The standalone application: the engine with the window plugin drawing
// straight into the swapchain. It opens a project directory (`--project
// <dir>`, the editor's testing project by default), registers every asset
// file under it and loads a scene file from it (`--scene <path>`, relative
// to the project, scenes/demo.scene by default); without a loadable scene
// it spawns the built-in demo. The frame is drawn from the scene camera
// tagged RenderCamera, else the first camera under scene_root. F5 reloads
// the shaders, F6 toggles the demo post-process pass, F7 the demo shadow
// pass (also --post / --shadow). `--cooked` makes the asset provider refuse
// uncooked files, as a shipped build would.
int main(const int argc, char** argv) {
    bool demo_post = false;
    bool demo_shadow = false;
    bool cooked_only = false;
    std::filesystem::path project_dir = STANDALONE_DEFAULT_PROJECT_DIR;
    std::filesystem::path scene_path = "scenes/demo.scene";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--post") == 0) {
            demo_post = true;
        } else if (strcmp(argv[i], "--shadow") == 0) {
            demo_shadow = true;
        } else if (strcmp(argv[i], "--cooked") == 0) {
            cooked_only = true;
        } else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
            project_dir = argv[++i];
        } else if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
            scene_path = argv[++i];
        } else {
            fprintf(stderr, "usage: standalone [--project <dir>] [--scene <path>] [--post] [--shadow] [--cooked]\n");
            return 1;
        }
    }

    Engine engine;
    WindowDesc window;
    window.title = "LuauEngine";
    engine.add_plugin<WindowPlugin>(window);
    // A shipped build only ever loads cooked assets (see docs/asset_format.md);
    // a development project's imports are not cooked yet, so that is opt-in.
    engine.add_plugin<CorePlugin>(cooked_only);
    engine.add_plugin<RenderPlugin>();
    engine.add_plugin<ProjectPlugin>(project_dir);
    engine.add_plugin<ScenePlugin>(scene_path);
    engine.add_plugin<DemoPlugin>(demo_post, demo_shadow);

    const bool ok = engine.init();
    if (ok) {
        engine.run();
    }
    engine.shutdown();
    return ok ? 0 : 1;
}
