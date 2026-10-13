#include "engine/engine.hpp"
#include "engine/plugins/core_plugin.hpp"
#include "engine/plugins/demo_plugin.hpp"
#include "engine/plugins/project_plugin.hpp"
#include "engine/plugins/render_plugin.hpp"
#include "engine/plugins/scene_plugin.hpp"
#include "engine/plugins/window_plugin.hpp"
#include "engine/utils/log.hpp"
#include "plugins/editor_camera_plugin.hpp"
#include "plugins/editor_grid_plugin.hpp"
#include "plugins/editor_highlight_plugin.hpp"
#include "plugins/editor_ui_plugin.hpp"
#include "plugins/editor_viewport_plugin.hpp"

#ifndef EDITOR_TEST_PROJECT_DIR
#define EDITOR_TEST_PROJECT_DIR "."
#endif

// The editor application: the same engine and plugins as the standalone,
// plus its own. The engine renders into per-slot viewport textures shown in
// an ImGui Viewport panel (EditorViewportPlugin) and only the UI is drawn
// into the swapchain (EditorUiPlugin); the Viewport is drawn from the
// editor's fly camera (EditorCameraPlugin) over a ground grid
// (EditorGridPlugin), with the selection outlined (EditorHighlightPlugin).
// The UI plugin is added before the project so its Output panel is a log
// sink by the time the project reports what it opened. There is no
// open-project page yet: the editor opens EDITOR_TEST_PROJECT_DIR
// (editor/CMakeLists.txt) and its scenes/demo.scene.
int main() {
    // The Output panel shows everything; the terminal keeps the problems,
    // so a run without the panel in view (or a crash before it is drawn)
    // still shows them.
    LOG::set_stderr_level(LOG_WARNING);
    Engine engine;
    WindowDesc window;
    window.title = "LuauEngine Editor";
    window.width = 1600;
    window.height = 900;
    window.swapchain_format = EditorViewportPlugin::UI_FORMAT;
    engine.add_plugin<WindowPlugin>(window);
    engine.add_plugin<CorePlugin>();
    engine.add_plugin<RenderPlugin>();
    engine.add_plugin<EditorCameraPlugin>();
    engine.add_plugin<EditorGridPlugin>();
    engine.add_plugin<EditorHighlightPlugin>();
    engine.add_plugin<EditorUiPlugin>();
    engine.add_plugin<EditorViewportPlugin>();
    engine.add_plugin<ProjectPlugin>(EDITOR_TEST_PROJECT_DIR);
    // The Viewport is drawn from the fly camera: the scene's own camera
    // keeps its RenderCamera tag for the standalone.
    ScenePlugin* scene = engine.add_plugin<ScenePlugin>();
    scene->tag_render_camera = false;
    engine.add_plugin<DemoPlugin>();

    const bool ok = engine.init();
    if (ok) {
        engine.run();
    }
    engine.shutdown();
    return ok ? 0 : 1;
}
