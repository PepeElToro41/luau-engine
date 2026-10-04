#include "app.hpp"

#include "ui/dock_layout.hpp"
#include "ui/panels.hpp"
#include "ui/themes.hpp"

#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <cstdio>

bool App::init() {
    if (!DISPLAY_WINDOW::sdl_initialize()) {
        return false;
    }
    if (!this->window.initialize(this->title, this->width, this->height)) {
        return false;
    }
    if (!this->gpu.init(this->window.window)) {
        return false;
    }
    if (!this->presenter.init(&this->gpu, this->window.window, UI_FORMAT)) {
        fprintf(stderr, "[editor] presenter init failed\n");
        return false;
    }
    if (!this->engine.init(&this->gpu)) {
        return false;
    }
    if (!this->init_ui()) {
        return false;
    }
    if (!this->init_viewport(this->viewport_extent)) {
        return false;
    }

    this->output.info("GPU: %s", this->gpu.properties.deviceName);
    this->output.info("Swapchain: %ux%u, %u images, format %d (wanted %d)",
        this->presenter.swapchain.extent.width,
        this->presenter.swapchain.extent.height,
        this->presenter.swapchain.image_count,
        static_cast<int>(this->presenter.swapchain.format),
        static_cast<int>(UI_FORMAT));
    if (this->presenter.swapchain.format != UI_FORMAT) {
        this->output.warning("Surface does not offer the UNORM swapchain format; UI colors will look washed out");
    }
    this->output.info("Editor ready");

    this->running = true;
    return true;
}

void App::run() {
    u64 last = SDL_GetTicksNS();
    while (this->running) {
        const u64 now = SDL_GetTicksNS();
        const f32 dt = static_cast<f32>(now - last) / 1e9f;
        last = now;

        this->poll_events();
        this->frame(dt);
    }
}

void App::shutdown() {
    this->running = false;
    this->gpu.wait_idle();
    this->shutdown_viewport();
    this->shutdown_ui();
    this->engine.shutdown();
    this->presenter.shutdown();
    this->gpu.shutdown();
    this->window.shutdown();
    DISPLAY_WINDOW::sdl_shutdown();
}

// --- UI lifecycle -------------------------------------------------------------

static void imgui_check_vk_result(VkResult result) {
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[editor][imgui] VkResult %d\n", static_cast<int>(result));
    }
}

bool App::init_ui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    THEMES::catppuccin_mocha();

    if (!ImGui_ImplSDL3_InitForVulkan(this->window.window)) {
        fprintf(stderr, "[editor] ImGui SDL3 backend init failed\n");
        return false;
    }

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_1;
    info.Instance = this->gpu.instance;
    info.PhysicalDevice = this->gpu.physical_device;
    info.Device = this->gpu.device;
    info.QueueFamily = this->gpu.graphics_queue_family;
    info.Queue = this->gpu.graphics_queue;
    info.DescriptorPoolSize = 16; // backend-managed pool for the font + viewport textures
    info.MinImageCount = this->presenter.swapchain.min_image_count < 2 ? 2 : this->presenter.swapchain.min_image_count;
    info.ImageCount = this->presenter.swapchain.image_count;
    info.PipelineInfoMain.RenderPass = this->presenter.targets.render_pass;
    info.PipelineInfoMain.Subpass = 0;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = imgui_check_vk_result;
    if (!ImGui_ImplVulkan_Init(&info)) {
        fprintf(stderr, "[editor] ImGui Vulkan backend init failed\n");
        return false;
    }

    this->ui_ready = true;
    return true;
}

void App::shutdown_ui() {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    if (this->ui_ready) {
        ImGui_ImplVulkan_Shutdown();
        this->ui_ready = false;
    }
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

bool App::init_viewport(const VkExtent2D extent) {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        OffscreenTarget& target = this->viewport[i];
        if (!target.init(&this->gpu, SCENE_FORMAT, extent, UI_FORMAT)) {
            return false;
        }
        this->viewport_texture[i] = ImGui_ImplVulkan_AddTexture(target.sampler, target.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    this->viewport_extent = extent;
    this->viewport_requested = extent;
    return true;
}

void App::shutdown_viewport() {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        if (this->viewport_texture[i] != VK_NULL_HANDLE && this->ui_ready) {
            ImGui_ImplVulkan_RemoveTexture(this->viewport_texture[i]);
        }
        this->viewport_texture[i] = VK_NULL_HANDLE;
        this->viewport[i].shutdown();
    }
}

void App::apply_viewport_resize() {
    const VkExtent2D wanted = this->viewport_requested;
    if (wanted.width == this->viewport_extent.width && wanted.height == this->viewport_extent.height) {
        return;
    }
    if (wanted.width == 0 || wanted.height == 0) {
        return;
    }

    // Rare (the user is dragging a panel edge), so a full idle is acceptable.
    this->gpu.wait_idle();
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        OffscreenTarget& target = this->viewport[i];
        ImGui_ImplVulkan_RemoveTexture(this->viewport_texture[i]);
        if (!target.resize(wanted)) {
            fprintf(stderr, "[editor] viewport resize to %ux%u failed\n", wanted.width, wanted.height);
        }
        this->viewport_texture[i] = ImGui_ImplVulkan_AddTexture(target.sampler, target.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    this->viewport_extent = wanted;
}

// --- Frame ------------------------------------------------------------------

void App::poll_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            this->running = false;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            this->presenter.mark_resized();
            break;
        default:
            break;
        }
    }
}

void App::frame(const f32 dt) {
    this->frame_dt = dt;
    this->engine.update(dt);
    this->apply_viewport_resize();

    FrameContext frame;
    if (!this->presenter.begin(frame)) {
        return;
    }

    // Scene into this slot's offscreen target. Its render pass ends in
    // SHADER_READ_ONLY_OPTIMAL and carries the dependency to the UI pass below.
    FrameContext scene = frame;
    scene.target = this->viewport[frame.slot].target();
    this->engine.render(scene);

    // UI into the swapchain image, sampling the scene through the Viewport panel.
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    this->draw_editor();
    ImGui::Render();

    VkClearValue clear{};
    clear.color = {{0.1f, 0.1f, 0.1f, 1.0f}};
    VkRenderPassBeginInfo pass_info{};
    pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass_info.renderPass = frame.target.render_pass;
    pass_info.framebuffer = frame.target.framebuffer;
    pass_info.renderArea.extent = frame.target.extent;
    pass_info.clearValueCount = 1;
    pass_info.pClearValues = &clear;
    vkCmdBeginRenderPass(frame.cmd, &pass_info, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.cmd);
    vkCmdEndRenderPass(frame.cmd);

    this->presenter.end();
}

// --- Panels -----------------------------------------------------------------

void App::draw_editor() {
    const ImGuiID dockspace = DOCK_LAYOUT::submit_dockspace();
    if (this->reset_layout || DOCK_LAYOUT::is_unset(dockspace)) {
        this->reset_layout = false;
        this->show_explorer = true;
        this->show_viewport = true;
        this->show_output = true;
        this->show_stats = true;
        DOCK_LAYOUT::build_default(dockspace);
    }

    this->draw_main_menu();
    if (this->show_explorer) {
        this->explorer.draw(&this->show_explorer);
    }
    if (this->show_viewport) {
        this->draw_viewport();
    }
    if (this->show_output) {
        this->output.draw(&this->show_output);
    }
    if (this->show_stats) {
        this->draw_stats();
    }
    if (this->show_demo_window) {
        ImGui::ShowDemoWindow(&this->show_demo_window);
    }
}

void App::draw_main_menu() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Quit", "Alt+F4")) {
            this->running = false;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem(PANELS::EXPLORER, nullptr, &this->show_explorer);
        ImGui::MenuItem(PANELS::VIEWPORT, nullptr, &this->show_viewport);
        ImGui::MenuItem(PANELS::OUTPUT, nullptr, &this->show_output);
        ImGui::MenuItem(PANELS::STATS, nullptr, &this->show_stats);
        ImGui::Separator();
        ImGui::MenuItem("ImGui Demo", nullptr, &this->show_demo_window);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) {
            this->reset_layout = true;
        }
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

void App::draw_viewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool open = ImGui::Begin(PANELS::VIEWPORT, &this->show_viewport);
    ImGui::PopStyleVar();
    if (open) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const u32 want_w = avail.x > 1.0f ? static_cast<u32>(avail.x) : 1;
        const u32 want_h = avail.y > 1.0f ? static_cast<u32>(avail.y) : 1;
        // Picked up by apply_viewport_resize() before the next frame begins;
        // this frame still shows the current image stretched to fit.
        this->viewport_requested = {want_w, want_h};

        const u32 slot = this->presenter.scheduler.current;
        ImGui::Image(reinterpret_cast<ImTextureID>(this->viewport_texture[slot]), avail);
    }
    ImGui::End();
}

void App::draw_stats() {
    if (ImGui::Begin(PANELS::STATS, &this->show_stats)) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("Frame %llu", static_cast<unsigned long long>(this->presenter.frame_index));
        ImGui::Text("%.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
        ImGui::Text("dt %.3f ms", this->frame_dt * 1000.0f);
        ImGui::Text("Swapchain %ux%u, %u images",
            this->presenter.swapchain.extent.width,
            this->presenter.swapchain.extent.height,
            this->presenter.swapchain.image_count);
        ImGui::Text("Viewport %ux%u", this->viewport_extent.width, this->viewport_extent.height);
        ImGui::Text("GPU %s", this->gpu.properties.deviceName);
    }
    ImGui::End();
}
