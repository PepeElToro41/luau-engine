#include "plugins/editor_ui_plugin.hpp"

#include "engine/engine.hpp"
#include "engine/gpu/gpu.hpp"
#include "engine/gpu/vulkan/vk_access.hpp"
#include "engine/plugins/core_plugin.hpp"
#include "engine/plugins/render_plugin.hpp"
#include "engine/plugins/window_plugin.hpp"
#include "engine/render/renderer.hpp"
#include "plugins/editor_camera_plugin.hpp"
#include "plugins/editor_viewport_plugin.hpp"
#include "ui/inspectors/inspectors.hpp"
#include "ui/themes.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <cstdio>

static void imgui_check_vk_result(VkResult result) {
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[editor][imgui] VkResult %d\n", static_cast<int>(result));
    }
}

bool EditorUiPlugin::build(Engine& engine) {
    if (engine.require<WindowPlugin>() == nullptr || engine.require<CorePlugin>() == nullptr || engine.require<RenderPlugin>() == nullptr
        || engine.require<EditorCameraPlugin>() == nullptr) {
        return false;
    }
    WindowPlugin* window = engine.get_plugin<WindowPlugin>();
    Renderer* renderer = engine.get_singleton<Renderer>();
    this->editor.engine = &engine;

    // Renderer messages land in the Output panel from here on.
    renderer->log_sink = RenderLogSink{&Editor::render_log, &this->editor};
    INSPECTORS::register_all(*engine.get_singleton<World>());
    this->editor.explorer.editor_camera = engine.get_plugin<EditorCameraPlugin>()->controller.entity;
    // Create > Shader in the Asset Browser copies one of the engine's shaders.
    this->editor.asset_browser.shader_template_dir = renderer->engine_render_dir;
    // The close request goes through the Unsaved Scene prompt.
    window->quit_on_close = false;

    if (!this->init_imgui(engine)) {
        return false;
    }
    this->editor.output.info("GPU: %s", engine.gpu->info.device_name);
    this->editor.output.info("Swapchain: %ux%u, %u images, %s (wanted %s)", engine.gpu->width, engine.gpu->height, VK_ACCESS::swapchain_image_count(engine.gpu),
        GPU_FORMAT::name(engine.gpu->info.swapchain_format), GPU_FORMAT::name(EditorViewportPlugin::UI_FORMAT));
    if (engine.gpu->info.swapchain_format != EditorViewportPlugin::UI_FORMAT) {
        this->editor.output.warning("Surface does not offer the UNORM swapchain format; UI colors will look washed out");
    }

    this->ui_phase = engine.scheduler.add_phase(engine.pipelines.render, EditorUiPlugin::UI_PHASE);
    engine.scheduler.phase_after(this->ui_phase, engine.phases.post_render);
    engine.add_system(engine.phases.events, EditorUiPlugin::NAME, &EditorUiPlugin::handle_events, this);
    engine.add_system(engine.phases.post_startup, EditorUiPlugin::NAME, &EditorUiPlugin::started, this);
    engine.add_system(this->ui_phase, EditorUiPlugin::NAME, &EditorUiPlugin::draw, this);
    engine.add_system(engine.phases.pre_shutdown, EditorUiPlugin::NAME, &EditorUiPlugin::detach, this);
    engine.add_system(engine.phases.shutdown, EditorUiPlugin::NAME, &EditorUiPlugin::shutdown, this);
    return true;
}

bool EditorUiPlugin::init_imgui(Engine& engine) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    this->context_ready = true;

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    THEMES::catppuccin_mocha();

    if (!ImGui_ImplSDL3_InitForVulkan(engine.get_plugin<WindowPlugin>()->window.window)) {
        engine.log(RENDER_LOG_ERROR, "ImGui SDL3 backend init failed");
        return false;
    }
    this->sdl_backend_ready = true;

    // ImGui draws into the swapchain through a pass compatible with every
    // pass that targets the swapchain format (the one the engine's pipelines
    // are built against too).
    GpuContext* gpu = engine.gpu;
    GpuTargetFormats ui_formats;
    ui_formats.color[0] = gpu->info.swapchain_format;
    ui_formats.color_count = 1;
    const u32 min_images = VK_ACCESS::swapchain_min_image_count(gpu);

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_1;
    info.Instance = VK_ACCESS::instance(gpu);
    info.PhysicalDevice = VK_ACCESS::physical_device(gpu);
    info.Device = VK_ACCESS::device(gpu);
    info.QueueFamily = VK_ACCESS::graphics_queue_family(gpu);
    info.Queue = VK_ACCESS::graphics_queue(gpu);
    info.DescriptorPoolSize = 16; // backend-managed pool for the font + viewport textures
    info.MinImageCount = min_images < 2 ? 2 : min_images;
    info.ImageCount = VK_ACCESS::swapchain_image_count(gpu);
    info.PipelineInfoMain.RenderPass = VK_ACCESS::render_pass(gpu, ui_formats);
    info.PipelineInfoMain.Subpass = 0;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = imgui_check_vk_result;
    if (!ImGui_ImplVulkan_Init(&info)) {
        engine.log(RENDER_LOG_ERROR, "ImGui Vulkan backend init failed");
        return false;
    }
    this->vulkan_backend_ready = true;
    return true;
}

// --- Systems -----------------------------------------------------------------------

void EditorUiPlugin::handle_events(SystemContext& ctx) {
    EditorUiPlugin* self = static_cast<EditorUiPlugin*>(ctx.user_data);
    const WindowPlugin* window = ENGINE::of(ctx).get_plugin<WindowPlugin>();
    for (const SDL_Event& event : window->events) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            self->editor.request_quit();
        }
    }
}

void EditorUiPlugin::started(SystemContext& ctx) {
    EditorUiPlugin* self = static_cast<EditorUiPlugin*>(ctx.user_data);
    // Whatever the scene plugin loaded (or spawned) is the saved state.
    self->editor.mark_scene_saved();
    self->editor.output.info("Editor ready");
}

void EditorUiPlugin::draw(SystemContext& ctx) {
    EditorUiPlugin* self = static_cast<EditorUiPlugin*>(ctx.user_data);
    FrameContext& frame = ENGINE::frame(ctx);

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    self->editor.draw();
    ImGui::Render();

    // The UI into the frame's target: the backbuffer, which the viewport
    // plugin pointed the context back at.
    GPU::cmd_barrier(frame.frame.cmd, frame.target, frame.target_state, GPU_STATE_COLOR_ATTACHMENT);
    GpuRenderPassDesc ui_pass;
    ui_pass.color[0].texture = frame.target;
    ui_pass.color[0].clear[0] = 0.1f;
    ui_pass.color[0].clear[1] = 0.1f;
    ui_pass.color[0].clear[2] = 0.1f;
    ui_pass.color_count = 1;
    ui_pass.name = "editor ui";
    GPU::cmd_begin_render_pass(frame.frame.cmd, ui_pass);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), VK_ACCESS::command_buffer(frame.frame.cmd));
    GPU::cmd_end_render_pass(frame.frame.cmd);
    frame.target_state = GPU_STATE_COLOR_ATTACHMENT;
}

void EditorUiPlugin::detach(SystemContext& ctx) {
    if (Renderer* renderer = ENGINE::of(ctx).get_singleton<Renderer>()) {
        renderer->log_sink = RenderLogSink{};
    }
}

void EditorUiPlugin::shutdown(SystemContext& ctx) {
    EditorUiPlugin* self = static_cast<EditorUiPlugin*>(ctx.user_data);
    if (self->vulkan_backend_ready) {
        ImGui_ImplVulkan_Shutdown();
        self->vulkan_backend_ready = false;
    }
    if (self->sdl_backend_ready) {
        ImGui_ImplSDL3_Shutdown();
        self->sdl_backend_ready = false;
    }
    if (self->context_ready) {
        ImGui::DestroyContext();
        self->context_ready = false;
    }
}
