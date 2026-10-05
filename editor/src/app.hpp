#pragma once

#include "engine/display_window.hpp"
#include "engine/engine.h"
#include "engine/gpu/device.hpp"
#include "engine/gpu/render_target.hpp"
#include "engine/gpu/window_presenter.hpp"
#include "file_dialog.hpp"
#include "project.hpp"
#include "ui/asset_browser_panel.hpp"
#include "ui/explorer_panel.hpp"
#include "ui/import/import_panel.hpp"
#include "ui/output_panel.hpp"

#include <imgui.h>

// The editor application: the same window / device / presenter / engine as
// the standalone build (standalone/src/app.hpp), plus the Dear ImGui context
// the editor is drawn with. The engine renders into an offscreen target that
// the Viewport panel shows as a texture; the swapchain image itself only ever
// receives the UI.
//
// Panels: Explorer (left), Viewport (center), Asset Browser, Output and Stats
// (bottom), laid out by DOCK_LAYOUT::build_default on first run and restorable
// from the View menu. Each panel with state of its own lives in ui/. Import
// is a floating window that shows up when File > Import... (the OS file
// picker) or a double-click in the Asset Browser queues a source file.
//
// The project being edited is the engine's Project singleton. There is no
// open-project page yet, so init() opens TEST_PROJECT_DIR (the repository
// root, from CMake) to have something for the Asset Browser to show.
struct App {
    // Returns false if anything could not start. shutdown() is still safe to
    // call afterwards.
    bool init();
    // Runs frames until the window is closed or quit is requested.
    void run();
    void shutdown();

    const char* title = "LuauEngine Editor";
    int width = 1600;
    int height = 900;

    DisplayWindow window;
    GpuDevice gpu;
    WindowPresenter presenter;
    Engine engine;

    bool running = false;

private:
    // Creates the engine's Project singleton and opens the hardcoded test
    // project into it. Replace with the open-project flow once it exists.
    void open_test_project();

    bool init_ui();
    void shutdown_ui();
    bool init_viewport(VkExtent2D extent);
    void shutdown_viewport();
    // Applies a pending viewport size change. Must run while no frame is
    // being recorded: it waits the device idle and rebuilds the images.
    void apply_viewport_resize();

    // Polls SDL, feeding every event to ImGui before acting on it.
    void poll_events();
    // Shows the OS picker for importable files, starting in the project.
    void open_import_dialog();
    // Moves files picked in the OS dialog into the Import panel.
    void poll_import_dialog();
    // Queues `source` (absolute, or relative to the project root) in the
    // Import panel with the Asset Browser's folder as destination.
    void queue_import(const std::filesystem::path& source);
    void frame(f32 dt);

    // Builds the ImGui frame: dockspace, main menu, panels.
    void draw_editor();
    void draw_main_menu();
    void draw_viewport();
    void draw_stats();

    bool ui_ready = false;
    // Set to rebuild the default dock layout at the start of the next frame.
    bool reset_layout = false;

    OutputPanel output;
    ExplorerPanel explorer;
    AssetBrowserPanel asset_browser;
    ImportPanel import_panel;
    NativeFileDialog file_dialog;

    // Formats. ImGui's colors are already display-encoded, so the swapchain
    // it draws into is _UNORM (no second encode). The engine renders linear
    // light, so the scene target is _SRGB like the standalone swapchain, and
    // the UI samples it through a _UNORM view to show the encoded bytes 1:1.
    static constexpr VkFormat UI_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;
    static constexpr VkFormat SCENE_FORMAT = VK_FORMAT_B8G8R8A8_SRGB;

    // One scene target per frame in flight, so the UI pass of frame N never
    // samples the image frame N+1 is writing.
    OffscreenTarget viewport[FRAMES_IN_FLIGHT];
    VkDescriptorSet viewport_texture[FRAMES_IN_FLIGHT] = {};
    VkExtent2D viewport_extent = {1280, 720};
    VkExtent2D viewport_requested = {1280, 720};

    // Panel visibility, toggled from the View menu.
    bool show_explorer = true;
    bool show_viewport = true;
    bool show_output = true;
    bool show_stats = true;
    bool show_asset_browser = true;
    bool show_import = false;
    bool show_demo_window = false;

    f32 frame_dt = 0.0f;
};
