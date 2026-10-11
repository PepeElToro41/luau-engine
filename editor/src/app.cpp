#include "app.hpp"

#include "engine/asset/asset_entity.hpp"
#include "engine/asset/asset_types/scene_asset.hpp"
#include "engine/asset/text_asset.hpp"
#include "engine/render/demo_scene.hpp"
#include "engine/render/materials.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/shader_library.hpp"
#include "engine/utils/hash.hpp"

#include "ui/dock_layout.hpp"
#include "ui/inspectors/inspectors.hpp"
#include "ui/panels.hpp"
#include "ui/themes.hpp"

#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

bool App::init() {
    if (!DISPLAY_WINDOW::sdl_initialize()) {
        return false;
    }
    if (!this->window.initialize(this->title, this->width, this->height)) {
        return false;
    }
    GpuInitDesc gpu_desc;
    gpu_desc.window = this->window.window;
    gpu_desc.swapchain_format = UI_FORMAT;
    this->gpu = GPU::init(gpu_desc);
    if (this->gpu == nullptr) {
        fprintf(stderr, "[editor] GPU init failed\n");
        return false;
    }
    if (!this->engine.init(this->gpu)) {
        return false;
    }
    this->engine.get_singleton<Renderer>()->log_sink = RenderLogSink{&App::render_log, this};
    INSPECTORS::register_all(*this->engine.get_singleton<World>());
    if (!this->editor_camera.init(*this->engine.get_singleton<World>())) {
        fprintf(stderr, "[editor] editor camera could not be created\n");
        return false;
    }
    this->explorer.editor_camera = this->editor_camera.entity;
    // The Viewport is always drawn from the fly camera, whichever scene
    // camera carries the RenderCamera tag for the standalone.
    this->engine.get_singleton<Renderer>()->camera = this->editor_camera.entity;
    // Create > Shader in the Asset Browser copies one of the engine's shaders.
    this->asset_browser.shader_template_dir = this->engine.get_singleton<Renderer>()->engine_render_dir;
    if (!this->init_ui()) {
        return false;
    }
    if (!this->init_viewport(this->viewport_requested_width, this->viewport_requested_height)) {
        return false;
    }

    this->output.info("GPU: %s", this->gpu->info.device_name);
    this->output.info("Swapchain: %ux%u, %u images, %s (wanted %s)", this->gpu->width, this->gpu->height, VK_ACCESS::swapchain_image_count(this->gpu),
        GPU_FORMAT::name(this->gpu->info.swapchain_format), GPU_FORMAT::name(UI_FORMAT));
    if (this->gpu->info.swapchain_format != UI_FORMAT) {
        this->output.warning("Surface does not offer the UNORM swapchain format; UI colors will look washed out");
    }
    this->open_test_project();
    this->load_startup_scene();
    this->output.info("Editor ready");

    this->running = true;
    return true;
}

#ifndef EDITOR_TEST_PROJECT_DIR
#define EDITOR_TEST_PROJECT_DIR "."
#endif

void App::open_test_project() {
    // Placeholder until the editor has an open-project page: the path is the
    // repository root, baked in by editor/CMakeLists.txt.
    Project* project = this->engine.create_singleton<Project>();
    if (project == nullptr) {
        this->output.error("Project singleton already exists");
        return;
    }
    if (project->open(EDITOR_TEST_PROJECT_DIR)) {
        this->output.info("Project '%s' opened at %s", project->name.c_str(), project->root.string().c_str());
        // The project's render/ directory overrides the engine's shaders.
        // The whole project is shader source: its root is the include root
        // and where a name falls back to when no asset entity carries a path.
        const std::string render_dir = project->root.string();
        RENDERER::set_project_render_dir(*this->engine.get_singleton<Renderer>(), render_dir.c_str());
        this->scan_project_assets(*project);
    } else {
        this->output.error("Test project directory not found: %s", EDITOR_TEST_PROJECT_DIR);
    }
}

// Registers every asset file under the project with the AssetResourceProvider
// (.lunaasset containers and .material text assets), so references by GUID
// resolve: a material names its textures by GUID and the GPU asset cache
// asks the provider for them. Stands in for the path-to-GUID index
// docs/asset_format.md describes. The shader files the shader library can
// load (the project's render/ and the engine's) become shader asset
// entities too, so the Inspector can offer them; they are not provider
// assets, SHADER_LIBRARY reads them itself through the AssetPath the
// entity carries, which is why a project shader may sit in any folder.
void App::scan_project_assets(const Project& project) {
    std::error_code error;
    u32 registered = 0;
    for (std::filesystem::recursive_directory_iterator it(project.root, error), end; it != end; it.increment(error)) {
        if (error || !it->is_regular_file(error)) {
            continue;
        }
        std::string extension = it->path().extension().string();
        for (char& c : extension) {
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }
        if (extension != ASSET_FILE::EXTENSION && TEXT_ASSET::type_of_path(extension.c_str()) == 0) {
            continue;
        }
        const std::string path = it->path().string();
        if (!this->engine.load_asset_file(path.c_str()).is_null()) {
            registered += 1;
        } else {
            this->output.warning("Asset %s could not be registered", path.c_str());
        }
    }
    this->output.info("%u asset files registered", registered);

    // Engine shaders first: a project file of the same name has the same
    // GUID and points the entity at its own path, which is the override.
    u32 shaders = 0;
    const Renderer* renderer = this->engine.get_singleton<Renderer>();
    if (renderer != nullptr && renderer->engine_render_dir[0] != '\0') {
        shaders += this->register_shader_assets(renderer->engine_render_dir, false);
    }
    shaders += this->register_shader_assets(project.root, true);
    this->output.info("%u shader files registered", shaders);
}

u32 App::register_shader_assets(const std::filesystem::path& directory, const bool recursive) {
    AssetEntities* assets = this->engine.get_singleton<AssetEntities>();
    if (assets == nullptr) {
        return 0;
    }
    std::error_code error;
    u32 registered = 0;
    // Stem -> path of the files this walk registered, to report a second
    // file of the same name: the name is the GUID, so only one can win.
    std::unordered_map<std::string, std::string> seen;
    const auto visit = [&](const std::filesystem::path& file) {
        std::string extension = file.extension().string();
        for (char& c : extension) {
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }
        if (extension != ".slang" && extension != ".glsl") {
            return;
        }
        const std::string stem = file.stem().string();
        const std::string path = file.string();
        const auto [where, inserted] = seen.emplace(stem, path);
        if (!inserted) {
            this->output.warning("Shader %s is ignored: %s already registered the name '%s'", path.c_str(), where->second.c_str(), stem.c_str());
            return;
        }
        if (assets->add(SHADER_LIBRARY::asset_guid(stem.c_str()), ASSET_TYPE::SHADER, stem.c_str(), path.c_str()) != 0) {
            registered += 1;
        } else {
            this->output.warning("Shader %s could not be registered", path.c_str());
        }
    };
    if (recursive) {
        for (std::filesystem::recursive_directory_iterator it(directory, error), end; it != end; it.increment(error)) {
            if (!error && it->is_regular_file(error)) {
                visit(it->path());
            }
        }
    } else {
        for (std::filesystem::directory_iterator it(directory, error), end; it != end; it.increment(error)) {
            if (!error && it->is_regular_file(error)) {
                visit(it->path());
            }
        }
    }
    return registered;
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
    if (this->gpu != nullptr) {
        GPU::wait_idle(this->gpu);
    }
    this->shutdown_viewport();
    this->shutdown_ui();
    this->engine.shutdown();
    if (this->gpu != nullptr) {
        GPU::shutdown(this->gpu);
        this->gpu = nullptr;
    }
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

    // ImGui draws into the swapchain through a pass compatible with every
    // pass that targets the swapchain format (the one the engine's pipelines
    // are built against too).
    GpuTargetFormats ui_formats;
    ui_formats.color[0] = this->gpu->info.swapchain_format;
    ui_formats.color_count = 1;
    const u32 min_images = VK_ACCESS::swapchain_min_image_count(this->gpu);

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_1;
    info.Instance = VK_ACCESS::instance(this->gpu);
    info.PhysicalDevice = VK_ACCESS::physical_device(this->gpu);
    info.Device = VK_ACCESS::device(this->gpu);
    info.QueueFamily = VK_ACCESS::graphics_queue_family(this->gpu);
    info.Queue = VK_ACCESS::graphics_queue(this->gpu);
    info.DescriptorPoolSize = 16; // backend-managed pool for the font + viewport textures
    info.MinImageCount = min_images < 2 ? 2 : min_images;
    info.ImageCount = VK_ACCESS::swapchain_image_count(this->gpu);
    info.PipelineInfoMain.RenderPass = VK_ACCESS::render_pass(this->gpu, ui_formats);
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

bool App::init_viewport(const u32 width, const u32 height) {
    GpuSamplerDesc sampler_desc;
    sampler_desc.mip_filter = GPU_FILTER_NEAREST;
    sampler_desc.address_u = sampler_desc.address_v = sampler_desc.address_w = GPU_ADDRESS_CLAMP;
    this->viewport_sampler = GPU::sampler(this->gpu, sampler_desc);

    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        GpuTextureDesc desc;
        desc.format = SCENE_FORMAT;
        desc.sampled_format = UI_FORMAT;
        desc.width = width;
        desc.height = height;
        desc.usage = GPU_TEXTURE_USAGE_COLOR_ATTACHMENT | GPU_TEXTURE_USAGE_SAMPLED;
        this->viewport[i] = GPU::create_texture(this->gpu, desc);
        if (!this->viewport[i].is_valid()) {
            return false;
        }
        this->viewport_state[i] = GPU_STATE_UNDEFINED;
        this->viewport_texture[i] = ImGui_ImplVulkan_AddTexture(VK_ACCESS::sampler(this->gpu, this->viewport_sampler), VK_ACCESS::image_view(this->gpu, this->viewport[i]),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    this->viewport_width = width;
    this->viewport_height = height;
    this->viewport_requested_width = width;
    this->viewport_requested_height = height;
    return true;
}

void App::shutdown_viewport() {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        if (this->viewport_texture[i] != VK_NULL_HANDLE && this->ui_ready) {
            ImGui_ImplVulkan_RemoveTexture(this->viewport_texture[i]);
        }
        this->viewport_texture[i] = VK_NULL_HANDLE;
        if (this->viewport[i].is_valid()) {
            GPU::destroy_texture(this->gpu, this->viewport[i]);
            this->viewport[i] = GpuTexture{};
        }
    }
}

void App::apply_viewport_resize() {
    const u32 wanted_width = this->viewport_requested_width;
    const u32 wanted_height = this->viewport_requested_height;
    if ((wanted_width == this->viewport_width && wanted_height == this->viewport_height) || wanted_width == 0 || wanted_height == 0) {
        return;
    }
    // Rare (the user is dragging a panel edge), so a full idle is acceptable.
    GPU::wait_idle(this->gpu);
    this->shutdown_viewport();
    if (!this->init_viewport(wanted_width, wanted_height)) {
        fprintf(stderr, "[editor] viewport resize to %ux%u failed\n", wanted_width, wanted_height);
    }
}

// --- Frame ------------------------------------------------------------------

void App::poll_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            this->request_quit();
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            GPU::mark_resized(this->gpu);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            this->editor_camera.add_wheel(event.wheel.y);
            break;
        default:
            break;
        }
    }
}

void App::frame(const f32 dt) {
    this->frame_dt = dt;
    this->engine.update(dt);
    this->editor_camera.update(*this->engine.get_singleton<World>(), this->window.window, this->viewport_hovered, dt);
    this->apply_viewport_resize();

    GpuFrame frame;
    if (!GPU::begin_frame(this->gpu, frame)) {
        return;
    }
    const u32 slot = frame.slot;

    // Scene into this slot's viewport texture, then over to SAMPLED for the
    // Viewport panel.
    FrameContext scene;
    scene.frame = frame;
    scene.target = this->viewport[slot];
    scene.target_state = this->viewport_state[slot];
    this->engine.render(scene);
    GPU::cmd_barrier(frame.cmd, this->viewport[slot], scene.target_state, GPU_STATE_SAMPLED);
    this->viewport_state[slot] = GPU_STATE_SAMPLED;

    // UI into the swapchain image.
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    this->draw_editor();
    ImGui::Render();

    GPU::cmd_barrier(frame.cmd, frame.backbuffer, GPU_STATE_UNDEFINED, GPU_STATE_COLOR_ATTACHMENT);
    GpuRenderPassDesc ui_pass;
    ui_pass.color[0].texture = frame.backbuffer;
    ui_pass.color[0].clear[0] = 0.1f;
    ui_pass.color[0].clear[1] = 0.1f;
    ui_pass.color[0].clear[2] = 0.1f;
    ui_pass.color_count = 1;
    ui_pass.name = "editor ui";
    GPU::cmd_begin_render_pass(frame.cmd, ui_pass);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), VK_ACCESS::command_buffer(frame.cmd));
    GPU::cmd_end_render_pass(frame.cmd);

    GPU::end_frame(this->gpu, frame);
}

// --- Panels -----------------------------------------------------------------

void App::draw_editor() {
    const ImGuiID dockspace = DOCK_LAYOUT::submit_dockspace();
    if (this->reset_layout || DOCK_LAYOUT::is_unset(dockspace)) {
        this->reset_layout = false;
        this->show_explorer = true;
        this->show_inspector = true;
        this->show_viewport = true;
        this->show_output = true;
        this->show_stats = true;
        this->show_asset_browser = true;
        DOCK_LAYOUT::build_default(dockspace);
    }

    this->poll_import_dialog();
    this->poll_scene_dialog();
    this->draw_main_menu();
    this->draw_unsaved_scene_popup();
    // Global, so it fires whichever panel has the focus; a text field being
    // typed in keeps the route (ImGui gives active items precedence).
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) {
        this->save_all();
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal)) {
        this->request_open_scene();
    }
    Scene* scene = this->engine.get_singleton<Scene>();
    if (this->show_explorer) {
        this->explorer.scene_title = this->scene_display_name() + (this->scene_dirty ? "*" : "");
        this->explorer.draw(&this->show_explorer, this->engine.get_singleton<World>(), scene, this->selection);
        if (this->explorer.changed) {
            this->scene_dirty = true;
        }
    }
    if (this->show_inspector) {
        this->resolve_selection();
        this->inspector.draw(&this->show_inspector, this->engine.get_singleton<World>(), this->engine.get_singleton<Project>(), this->selection);
        if (this->inspector.changed && this->selection.is_entity() && scene != nullptr && scene->contains(this->selection.entity)) {
            this->scene_dirty = true;
        }
        if (this->inspector.file_action != InspectorFileAction::NONE) {
            this->apply_file_action(this->inspector.file_action);
        }
        if (this->inspector.request.kind != InspectorRequestKind::NONE) {
            this->apply_inspector_request(this->inspector.request);
        }
    }
    if (this->show_viewport) {
        this->draw_viewport();
    } else {
        this->viewport_hovered = false;
    }
    if (this->show_asset_browser) {
        this->asset_browser.draw(&this->show_asset_browser, this->engine.get_singleton<Project>(), this->output, this->selection);
        if (!this->asset_browser.activated.empty()) {
            const std::filesystem::path& file = this->asset_browser.activated;
            const Project* project = this->engine.get_singleton<Project>();
            if (ImportPanel::kind_of(file.extension().string()) != ImportPanel::Kind::UNSUPPORTED) {
                this->queue_import(file);
            } else if (TEXT_ASSET::type_of_path(file.string().c_str()) == ASSET_TYPE::SCENE && project != nullptr && project->is_open()) {
                this->request_open_scene(project->root / file);
            }
        }
        if (!this->asset_browser.reimport_asset.empty()) {
            this->import_panel.open_reimport(this->asset_browser.reimport_source, this->asset_browser.reimport_asset, &this->show_import);
        }
    }
    if (this->show_import) {
        if (this->import_panel.draw(&this->show_import, this->engine.get_singleton<Project>(), this->output)) {
            this->asset_browser.refresh();
        }
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

// --- Selection --------------------------------------------------------------

void App::resolve_selection() {
    if (!this->selection.is_file()) {
        this->resolved_file.clear();
        return;
    }
    if (this->selection.file == this->resolved_file) {
        return;
    }
    this->resolved_file = this->selection.file;
    this->selection.entity = 0;
    const Project* project = this->engine.get_singleton<Project>();
    if (project == nullptr || !project->is_open()) {
        return;
    }
    const std::filesystem::path absolute = project->root / this->selection.file;
    if (TEXT_ASSET::type_of_path(absolute.string().c_str()) != ASSET_TYPE::MATERIAL) {
        return;
    }
    // Registers (or refreshes) the file with the provider and loads it as a
    // Material entity, or finds the one loaded before.
    const AssetGuid guid = this->engine.load_asset_file(absolute.string().c_str());
    if (guid.is_null()) {
        this->output.error("Material %s could not be registered", this->selection.file.generic_string().c_str());
        return;
    }
    this->selection.entity = MATERIAL::load(*this->engine.get_singleton<Renderer>(), guid);
}

void App::apply_file_action(const InspectorFileAction action) {
    const Project* project = this->engine.get_singleton<Project>();
    Renderer* renderer = this->engine.get_singleton<Renderer>();
    if (!this->selection.is_file() || this->selection.entity == 0 || project == nullptr || !project->is_open() || renderer == nullptr) {
        return;
    }
    const std::string shown = this->selection.file.generic_string();
    const std::string absolute = (project->root / this->selection.file).string();
    switch (action) {
    case InspectorFileAction::SAVE:
        if (MATERIAL::save(*renderer, this->selection.entity, absolute.c_str())) {
            this->output.info("Saved %s", shown.c_str());
            this->asset_browser.refresh();
        }
        break;
    case InspectorFileAction::RELOAD:
        if (MATERIAL::reload(*renderer, this->selection.entity)) {
            this->output.info("Reloaded %s", shown.c_str());
        }
        break;
    case InspectorFileAction::NONE:
        break;
    }
}

void App::save_all() {
    Renderer* renderer = this->engine.get_singleton<Renderer>();
    World* world = this->engine.get_singleton<World>();
    const Project* project = this->engine.get_singleton<Project>();
    if (renderer == nullptr || world == nullptr) {
        return;
    }
    // Collect first: saving moves the entity out of the archetype being
    // iterated when it drops the tag.
    std::vector<EntityId> dirty;
    world->query<Material>().with<AssetDirty>().each([&](const EntityId entity, Material&) { dirty.push_back(entity); });
    const bool scene_pending = this->scene_needs_save();
    if (dirty.empty() && !scene_pending) {
        this->output.info("Nothing to save");
        return;
    }
    u32 saved = 0;
    for (const EntityId entity : dirty) {
        const AssetPath* asset_path = world->get<AssetPath>(entity);
        const char* entity_name = SCENE::name(*world, entity);
        std::string shown = asset_path != nullptr ? asset_path->value : entity_name != nullptr ? entity_name : "(unnamed)";
        if (asset_path != nullptr && project != nullptr && project->is_open()) {
            const std::filesystem::path relative = std::filesystem::path(asset_path->value).lexically_relative(project->root);
            if (!relative.empty() && relative.native()[0] != '.') {
                shown = relative.generic_string();
            }
        }
        if (MATERIAL::save(*renderer, entity)) {
            this->output.info("Saved %s", shown.c_str());
            saved += 1;
        } else {
            this->output.error("Could not save %s", shown.c_str());
        }
    }
    if (saved > 0) {
        this->asset_browser.refresh();
    }
    // The scene last: its materials are on disk by now, so what it
    // references is current.
    if (scene_pending) {
        this->save_scene();
    }
}

void App::apply_inspector_request(const InspectorRequest& request) {
    Renderer* renderer = this->engine.get_singleton<Renderer>();
    World* world = this->engine.get_singleton<World>();
    if (renderer == nullptr || world == nullptr || !world->alive(request.asset)) {
        return;
    }
    const char* asset_name = SCENE::name(*world, request.asset);
    if (asset_name == nullptr || asset_name[0] == '\0') {
        asset_name = "(unnamed)";
    }
    switch (request.kind) {
    case InspectorRequestKind::LOAD_MATERIAL: {
        const AssetUuid* asset = world->get<AssetUuid>(request.asset);
        if (asset == nullptr) {
            this->output.error("%s is not an asset entity", asset_name);
        } else if (MATERIAL::load(*renderer, asset->guid) == 0) {
            this->output.error("Material %s did not load", asset_name);
        } else {
            this->output.info("Loaded material %s", asset_name);
        }
        break;
    }
    case InspectorRequestKind::SET_MATERIAL_SHADER: {
        // A shader asset is named after its file's stem, which is the name
        // the library loads it by; loading an already compiled one is a lookup.
        const EntityId shader = SHADER_LIBRARY::load(*renderer, asset_name);
        if (shader == 0) {
            this->output.error("Shader %s did not load", asset_name);
        } else if (MATERIAL::set_shader(*renderer, request.target, shader)) {
            this->output.info("Material now uses shader %s", asset_name);
        }
        break;
    }
    case InspectorRequestKind::NONE:
        break;
    }
}

// --- Import -----------------------------------------------------------------

// What File > Import... offers. SDL keeps the pointer until the dialog
// closes, hence static. Patterns are extensions without the dot, ';' separated.
static constexpr SDL_DialogFileFilter IMPORT_FILTERS[] = {
    {"All importable (obj, png, jpg, bmp, tga)", "obj;png;jpg;jpeg;bmp;tga"},
    {"Meshes (obj)", "obj"},
    {"Images (png, jpg, bmp, tga)", "png;jpg;jpeg;bmp;tga"},
};

void App::open_import_dialog() {
    const Project* project = this->engine.get_singleton<Project>();
    std::string start;
    if (project != nullptr && project->is_open()) {
        start = project->root.string();
    }
    const int filter_count = static_cast<int>(sizeof(IMPORT_FILTERS) / sizeof(IMPORT_FILTERS[0]));
    this->file_dialog.open_files(this->window.window, IMPORT_FILTERS, filter_count, start.empty() ? nullptr : start.c_str(), true);
}

void App::poll_import_dialog() {
    std::vector<std::filesystem::path> picked;
    std::string error;
    if (!this->file_dialog.take(&picked, &error)) {
        return;
    }
    if (!error.empty()) {
        this->output.error("Import dialog: %s", error.c_str());
        return;
    }
    for (const std::filesystem::path& path : picked) {
        this->queue_import(path);
    }
}

void App::queue_import(const std::filesystem::path& source) {
    const Project* project = this->engine.get_singleton<Project>();
    std::filesystem::path absolute = source;
    if (absolute.is_relative() && project != nullptr && project->is_open()) {
        absolute = project->root / source;
    }
    this->import_panel.open(absolute, this->asset_browser.current, &this->show_import);
}

void App::draw_main_menu() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        const Project* project = this->engine.get_singleton<Project>();
        const bool has_project = project != nullptr && project->is_open();
        const Scene* scene = this->engine.get_singleton<Scene>();
        const bool scene_has_file = scene != nullptr && scene->current_path[0] != '\0';
        if (ImGui::MenuItem("New Scene")) {
            this->request_new_scene();
        }
        ImGui::BeginDisabled(this->scene_dialog.is_pending());
        if (ImGui::MenuItem("Open Scene...", "Ctrl+O")) {
            this->request_open_scene();
        }
        if (ImGui::MenuItem(scene_has_file ? "Save Scene" : "Save Scene...")) {
            this->save_scene();
        }
        if (ImGui::MenuItem("Save Scene As...")) {
            this->save_scene_as();
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::BeginDisabled(!has_project || this->file_dialog.is_pending());
        if (ImGui::MenuItem("Import...")) {
            this->open_import_dialog();
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!has_project);
        // Registers files added since startup (a .material or .slang
        // created outside the editor, or a shader file the Asset Browser
        // wrote), so the Inspector's dropdowns offer them.
        if (ImGui::MenuItem("Refresh Assets")) {
            this->scan_project_assets(*project);
            this->asset_browser.refresh();
        }
        ImGui::Separator();
        ImGui::EndDisabled();
        // Every file-backed entity with unsaved edits (AssetDirty) and the
        // scene; the shortcut itself is polled in draw_editor so it works
        // from any panel.
        if (ImGui::MenuItem("Save All", "Ctrl+S")) {
            this->save_all();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Alt+F4")) {
            this->request_quit();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Render")) {
        if (ImGui::MenuItem("Reload Shaders", "F5")) {
            SHADER_LIBRARY::reload_all(*this->engine.get_singleton<Renderer>());
        }
        if (ImGui::MenuItem("Post-process Pass", nullptr, &this->demo_post)) {
            if (!RENDER_DEMO::set_post_enabled(this->engine, this->demo_post)) {
                this->demo_post = false;
            }
        }
        if (ImGui::MenuItem("Shadow Pass", nullptr, &this->demo_shadow)) {
            if (!RENDER_DEMO::set_shadow_enabled(this->engine, this->demo_shadow)) {
                this->demo_shadow = false;
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem(PANELS::EXPLORER, nullptr, &this->show_explorer);
        ImGui::MenuItem(PANELS::INSPECTOR, nullptr, &this->show_inspector);
        ImGui::MenuItem(PANELS::VIEWPORT, nullptr, &this->show_viewport);
        ImGui::MenuItem(PANELS::ASSET_BROWSER, nullptr, &this->show_asset_browser);
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

    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        SHADER_LIBRARY::reload_all(*this->engine.get_singleton<Renderer>());
    }
}

void App::render_log(const RenderLogLevel level, const char* text, void* user_data) {
    App* app = static_cast<App*>(user_data);
    // Problems also go to the terminal, so a run without the panel in view
    // (or a crash before it is drawn) still shows them.
    if (level != RENDER_LOG_INFO) {
        fprintf(stderr, "[editor][%s] %s\n", level == RENDER_LOG_ERROR ? "error" : "warning", text);
    }
    switch (level) {
    case RENDER_LOG_ERROR:
        app->output.error("%s", text);
        break;
    case RENDER_LOG_WARNING:
        app->output.warning("%s", text);
        break;
    default:
        app->output.info("%s", text);
        break;
    }
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
        this->viewport_requested_width = want_w;
        this->viewport_requested_height = want_h;

        ImGui::Image(reinterpret_cast<ImTextureID>(this->viewport_texture[this->gpu->slot]), avail);
        this->viewport_hovered = ImGui::IsItemHovered();
    } else {
        this->viewport_hovered = false;
    }
    ImGui::End();
}

void App::draw_stats() {
    if (ImGui::Begin(PANELS::STATS, &this->show_stats)) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("Frame %llu", static_cast<unsigned long long>(this->gpu->frame_index));
        ImGui::Text("%.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
        ImGui::Text("dt %.3f ms", this->frame_dt * 1000.0f);
        ImGui::Text("Swapchain %ux%u, %u images", this->gpu->width, this->gpu->height, VK_ACCESS::swapchain_image_count(this->gpu));
        ImGui::Text("Viewport %ux%u", this->viewport_width, this->viewport_height);
        ImGui::Text("Camera speed %.2f u/s%s", this->editor_camera.speed, this->editor_camera.flying ? " (flying)" : "");
        ImGui::Text("GPU %s", this->gpu->info.device_name);
    }
    ImGui::End();
}

// --- Scenes -----------------------------------------------------------------

// What Open Scene... and Save Scene As... offer; static for SDL (see
// IMPORT_FILTERS).
static constexpr SDL_DialogFileFilter SCENE_FILTERS[] = {
    {"Scenes (scene)", "scene"},
};

void App::load_startup_scene() {
    const Project* project = this->engine.get_singleton<Project>();
    if (project != nullptr && project->is_open()) {
        const std::filesystem::path scene = project->root / "scenes" / "demo.scene";
        std::error_code error;
        if (std::filesystem::exists(scene, error) && !error) {
            if (this->open_scene(scene)) {
                return;
            }
        } else {
            this->output.warning("No startup scene at %s; spawning the built-in demo", scene.string().c_str());
        }
    }
    if (!RENDER_DEMO::spawn(this->engine)) {
        this->output.error("Demo scene could not be created");
    }
    this->scene_dirty = false;
    this->scene_saved_hash = this->scene_hash();
}

bool App::open_scene(const std::filesystem::path& absolute) {
    const std::string path = absolute.string();
    const AssetGuid guid = this->engine.load_asset_file(path.c_str());
    if (guid.is_null()) {
        this->output.error("Scene %s could not be registered", path.c_str());
        return false;
    }
    // Whatever was selected is about to be deleted with the old content.
    this->selection.clear();
    const bool ok = this->engine.load_scene(guid);
    if (!ok) {
        this->output.error("Scene %s did not load", path.c_str());
    }
    this->scene_dirty = false;
    this->scene_saved_hash = this->scene_hash();
    return ok;
}

bool App::save_scene_to(const std::filesystem::path& absolute) {
    const std::string path = absolute.string();
    if (!this->engine.save_scene(path.c_str())) {
        this->output.error("Could not save scene %s", path.c_str());
        return false;
    }
    this->scene_dirty = false;
    this->scene_saved_hash = this->scene_hash();
    this->output.info("Saved %s", this->scene_display_name().c_str());
    this->asset_browser.refresh();
    return true;
}

void App::save_scene() {
    const Scene* scene = this->engine.get_singleton<Scene>();
    if (scene != nullptr && scene->current_path[0] != '\0') {
        this->save_scene_to(scene->current_path);
    } else {
        this->save_scene_as();
    }
}

void App::save_scene_as() {
    const Project* project = this->engine.get_singleton<Project>();
    std::string start;
    if (project != nullptr && project->is_open()) {
        std::error_code error;
        const std::filesystem::path scenes = project->root / "scenes";
        start = (std::filesystem::is_directory(scenes, error) && !error ? scenes : project->root).string();
    }
    this->scene_dialog_kind = SceneDialog::SAVE_AS;
    if (!this->scene_dialog.save_file(this->window.window, SCENE_FILTERS, 1, start.empty() ? nullptr : start.c_str())) {
        this->scene_dialog_kind = SceneDialog::NONE;
    }
}

void App::new_scene_now() {
    this->selection.clear();
    this->engine.new_scene();
    this->scene_dirty = false;
    this->scene_saved_hash = this->scene_hash();
    this->output.info("New scene");
}

void App::request_new_scene() {
    this->pending = PendingAction::NEW;
    if (this->scene_needs_save()) {
        this->open_unsaved_popup = true;
    } else {
        this->perform_pending();
    }
}

void App::request_open_scene(const std::filesystem::path& file) {
    this->pending = file.empty() ? PendingAction::OPEN : PendingAction::OPEN_FILE;
    this->pending_file = file;
    if (this->scene_needs_save()) {
        this->open_unsaved_popup = true;
    } else {
        this->perform_pending();
    }
}

void App::request_quit() {
    this->pending = PendingAction::QUIT;
    if (this->scene_needs_save()) {
        this->open_unsaved_popup = true;
    } else {
        this->perform_pending();
    }
}

void App::perform_pending() {
    const PendingAction action = this->pending;
    this->pending = PendingAction::NONE;
    switch (action) {
    case PendingAction::NEW:
        this->new_scene_now();
        break;
    case PendingAction::OPEN: {
        const Project* project = this->engine.get_singleton<Project>();
        std::string start;
        if (project != nullptr && project->is_open()) {
            start = project->root.string();
        }
        this->scene_dialog_kind = SceneDialog::OPEN;
        if (!this->scene_dialog.open_files(this->window.window, SCENE_FILTERS, 1, start.empty() ? nullptr : start.c_str(), false)) {
            this->scene_dialog_kind = SceneDialog::NONE;
        }
        break;
    }
    case PendingAction::OPEN_FILE:
        this->open_scene(this->pending_file);
        this->pending_file.clear();
        break;
    case PendingAction::QUIT:
        this->running = false;
        break;
    case PendingAction::NONE:
        break;
    }
}

u64 App::scene_hash() {
    DynamicArray<char> text;
    this->engine.write_scene(text);
    const u64 hash = HASH::fnv1a(text.data, text.count);
    text.free();
    return hash;
}

bool App::scene_needs_save() {
    if (this->scene_dirty) {
        return true;
    }
    // The safety net: a change the panels did not see (code, a script).
    return this->scene_hash() != this->scene_saved_hash;
}

std::string App::scene_display_name() const {
    const Scene* scene = this->engine.get_singleton<Scene>();
    if (scene == nullptr || scene->current_path[0] == '\0') {
        return "(unsaved scene)";
    }
    const Project* project = this->engine.get_singleton<Project>();
    if (project != nullptr && project->is_open()) {
        const std::filesystem::path relative = std::filesystem::path(scene->current_path).lexically_relative(project->root);
        if (!relative.empty() && relative.native()[0] != '.') {
            return relative.generic_string();
        }
    }
    return scene->current_path;
}

void App::draw_unsaved_scene_popup() {
    static const char* POPUP = "Unsaved Scene";
    if (this->open_unsaved_popup) {
        this->open_unsaved_popup = false;
        ImGui::OpenPopup(POPUP);
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    if (this->pending == PendingAction::NONE) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const Scene* scene = this->engine.get_singleton<Scene>();
    const bool has_file = scene != nullptr && scene->current_path[0] != '\0';
    ImGui::Text("Save changes to %s?", this->scene_display_name().c_str());
    ImGui::TextDisabled("%s", has_file ? "They are lost otherwise." : "The scene has not been saved to a file yet.");
    ImGui::Separator();
    if (ImGui::Button(has_file ? "Save" : "Save As...", ImVec2(100.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
        if (has_file) {
            if (this->save_scene_to(scene->current_path)) {
                this->perform_pending();
            } else {
                this->pending = PendingAction::NONE;
            }
        } else {
            // The pending action runs once the dialog's save went through.
            this->save_scene_as();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(100.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
        this->perform_pending();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
        this->pending = PendingAction::NONE;
        this->pending_file.clear();
    }
    ImGui::EndPopup();
}

void App::poll_scene_dialog() {
    std::vector<std::filesystem::path> picked;
    std::string error;
    if (!this->scene_dialog.take(&picked, &error)) {
        return;
    }
    const SceneDialog kind = this->scene_dialog_kind;
    this->scene_dialog_kind = SceneDialog::NONE;
    if (!error.empty()) {
        this->output.error("Scene dialog: %s", error.c_str());
        this->pending = PendingAction::NONE;
        return;
    }
    if (picked.empty()) {
        // Cancelled: whatever the save was guarding is off too.
        this->pending = PendingAction::NONE;
        this->pending_file.clear();
        return;
    }
    std::filesystem::path path = picked.front();
    switch (kind) {
    case SceneDialog::OPEN:
        if (TEXT_ASSET::type_of_path(path.string().c_str()) != ASSET_TYPE::SCENE) {
            this->output.error("%s is not a .scene file", path.string().c_str());
        } else {
            this->open_scene(path);
        }
        break;
    case SceneDialog::SAVE_AS:
        if (TEXT_ASSET::type_of_path(path.string().c_str()) != ASSET_TYPE::SCENE) {
            path += SCENE_ASSET::EXTENSION;
        }
        if (this->save_scene_to(path)) {
            this->perform_pending();
        } else {
            this->pending = PendingAction::NONE;
        }
        break;
    case SceneDialog::NONE:
        break;
    }
}
