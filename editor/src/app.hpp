#pragma once

#include "engine/display_window.hpp"
#include "engine/engine.hpp"
#include "engine/gpu/gpu.hpp"
#include "engine/gpu/vulkan/vk_access.hpp"
#include "engine/render/renderer.hpp"
#include "editor_camera.hpp"
#include "file_dialog.hpp"
#include "project.hpp"
#include "selection.hpp"
#include "ui/asset_browser_panel.hpp"
#include "ui/explorer_panel.hpp"
#include "ui/import/import_panel.hpp"
#include "ui/inspector_panel.hpp"
#include "ui/output_panel.hpp"

#include <imgui.h>

#include <filesystem>

// The editor application: the same window / GPU / engine as the standalone
// app, but the engine renders into per-slot viewport textures shown in an
// ImGui Viewport panel, and only the UI is drawn into the swapchain. ImGui's
// Vulkan backend is the one piece of the editor that sees Vulkan, fed
// through vk_access.hpp. The Viewport is drawn from the editor's own fly
// camera (editor_camera.hpp), driven from SDL's input state each frame
// before the engine renders.
struct App {
    bool init();
    void run();
    void shutdown();

    const char* title = "LuauEngine Editor";
    int width = 1600;
    int height = 900;

    DisplayWindow window;
    GpuContext* gpu = nullptr;
    Engine engine;
    bool running = false;

private:
    void open_test_project();
    void scan_project_assets(const Project& project);
    bool init_ui();
    void shutdown_ui();
    bool init_viewport(u32 width, u32 height);
    void shutdown_viewport();
    void apply_viewport_resize();
    void poll_events();
    void open_import_dialog();
    void poll_import_dialog();
    void queue_import(const std::filesystem::path& source);
    void frame(f32 dt);
    void draw_editor();
    void draw_main_menu();
    // Gives a file selection the entity it is loaded as (a .material's
    // Material entity), once per file picked.
    void resolve_selection();
    // Performs the Inspector's Save / Reload request on the selected file.
    void apply_file_action(InspectorFileAction action);
    // Renderer messages land in the Output panel.
    static void render_log(RenderLogLevel level, const char* text, void* user_data);
    void draw_viewport();
    void draw_stats();

    bool ui_ready = false;
    bool reset_layout = false;
    OutputPanel output;
    // What the Explorer or the Asset Browser picked last (selection.hpp);
    // `resolved_file` is the file whose entity was last looked up.
    Selection selection;
    std::filesystem::path resolved_file;
    ExplorerPanel explorer;
    InspectorPanel inspector;
    AssetBrowserPanel asset_browser;
    ImportPanel import_panel;
    NativeFileDialog file_dialog;
    EditorCameraController editor_camera;
    // Whether the mouse was over the Viewport image when the UI was last
    // drawn; where right-click starts fly mode.
    bool viewport_hovered = false;

    // ImGui writes already-encoded colors, so the swapchain is UNORM; the
    // scene is drawn into an sRGB texture that ImGui samples through a UNORM
    // view, copying the encoded bytes through untouched.
    static constexpr GpuFormat UI_FORMAT = GPU_FORMAT_BGRA8_UNORM;
    static constexpr GpuFormat SCENE_FORMAT = GPU_FORMAT_BGRA8_SRGB;
    GpuTexture viewport[FRAMES_IN_FLIGHT] = {};
    GpuResourceState viewport_state[FRAMES_IN_FLIGHT] = {};
    VkDescriptorSet viewport_texture[FRAMES_IN_FLIGHT] = {};
    GpuSampler viewport_sampler;
    u32 viewport_width = 1280;
    u32 viewport_height = 720;
    u32 viewport_requested_width = 1280;
    u32 viewport_requested_height = 720;

    bool show_explorer = true;
    bool show_inspector = true;
    bool show_viewport = true;
    bool show_output = true;
    bool show_stats = true;
    bool show_asset_browser = true;
    bool show_import = false;
    bool show_demo_window = false;
    bool demo_post = false;
    bool demo_shadow = false;
    f32 frame_dt = 0.0f;
};
