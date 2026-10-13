#include "editor.hpp"

#include "engine/asset/asset_entity.hpp"
#include "engine/asset/asset_types/scene_asset.hpp"
#include "engine/asset/text_asset.hpp"
#include "engine/gpu/vulkan/vk_access.hpp"
#include "engine/plugins/demo_plugin.hpp"
#include "engine/plugins/window_plugin.hpp"
#include "engine/project.hpp"
#include "engine/render/demo_scene.hpp"
#include "engine/render/materials.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/shader_library.hpp"
#include "engine/utils/hash.hpp"
#include "engine/utils/log.hpp"
#include "plugins/editor_camera_plugin.hpp"
#include "plugins/editor_grid_plugin.hpp"
#include "plugins/editor_viewport_plugin.hpp"

#include "ui/dock_layout.hpp"
#include "ui/panels.hpp"

#include <cstdio>
#include <string>
#include <vector>

// --- Plugins -----------------------------------------------------------------

SDL_Window* Editor::sdl_window() const {
    const WindowPlugin* window = this->engine->get_plugin<WindowPlugin>();
    return window != nullptr ? window->window.window : nullptr;
}

EditorCameraPlugin* Editor::camera() const {
    return this->engine->get_plugin<EditorCameraPlugin>();
}

EditorGridPlugin* Editor::grid() const {
    return this->engine->get_plugin<EditorGridPlugin>();
}

EditorViewportPlugin* Editor::viewport() const {
    return this->engine->get_plugin<EditorViewportPlugin>();
}

DemoPlugin* Editor::demo() const {
    return this->engine->get_plugin<DemoPlugin>();
}

void Editor::mark_scene_saved() {
    this->scene_dirty = false;
    this->scene_saved_hash = this->scene_hash();
}

// --- Panels -----------------------------------------------------------------

void Editor::draw() {
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
    Scene* scene = this->engine->get_singleton<Scene>();
    if (this->show_explorer) {
        this->explorer.scene_title = this->scene_display_name() + (this->scene_dirty ? "*" : "");
        this->explorer.draw(&this->show_explorer, this->engine->get_singleton<World>(), scene, this->selection);
        if (this->explorer.changed) {
            this->scene_dirty = true;
        }
    }
    if (this->show_inspector) {
        this->resolve_selection();
        this->inspector.draw(&this->show_inspector, this->engine->get_singleton<World>(), this->engine->get_singleton<Project>(), this->selection);
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
    } else if (EditorCameraPlugin* camera = this->camera()) {
        camera->viewport_hovered = false;
    }
    if (this->show_asset_browser) {
        this->asset_browser.draw(&this->show_asset_browser, this->engine->get_singleton<Project>(), this->selection);
        if (!this->asset_browser.activated.empty()) {
            const std::filesystem::path& file = this->asset_browser.activated;
            const Project* project = this->engine->get_singleton<Project>();
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
        if (this->import_panel.draw(&this->show_import, this->engine->get_singleton<Project>())) {
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

void Editor::resolve_selection() {
    if (!this->selection.is_file()) {
        this->resolved_file.clear();
        return;
    }
    if (this->selection.file == this->resolved_file) {
        return;
    }
    this->resolved_file = this->selection.file;
    this->selection.entity = 0;
    const Project* project = this->engine->get_singleton<Project>();
    if (project == nullptr || !project->is_open()) {
        return;
    }
    const std::filesystem::path absolute = project->root / this->selection.file;
    if (TEXT_ASSET::type_of_path(absolute.string().c_str()) != ASSET_TYPE::MATERIAL) {
        return;
    }
    // Registers (or refreshes) the file with the provider and loads it as a
    // Material entity, or finds the one loaded before.
    const AssetGuid guid = this->engine->load_asset_file(absolute.string().c_str());
    if (guid.is_null()) {
        LOG::error(LOG_CATEGORY::EDITOR, "Material %s could not be registered", this->selection.file.generic_string().c_str());
        return;
    }
    this->selection.entity = MATERIAL::load(*this->engine->get_singleton<Renderer>(), guid);
}

void Editor::apply_file_action(const InspectorFileAction action) {
    const Project* project = this->engine->get_singleton<Project>();
    Renderer* renderer = this->engine->get_singleton<Renderer>();
    if (!this->selection.is_file() || this->selection.entity == 0 || project == nullptr || !project->is_open() || renderer == nullptr) {
        return;
    }
    const std::string shown = this->selection.file.generic_string();
    const std::string absolute = (project->root / this->selection.file).string();
    switch (action) {
    case InspectorFileAction::SAVE:
        if (MATERIAL::save(*renderer, this->selection.entity, absolute.c_str())) {
            LOG::info(LOG_CATEGORY::EDITOR, "Saved %s", shown.c_str());
            this->asset_browser.refresh();
        }
        break;
    case InspectorFileAction::RELOAD:
        if (MATERIAL::reload(*renderer, this->selection.entity)) {
            LOG::info(LOG_CATEGORY::EDITOR, "Reloaded %s", shown.c_str());
        }
        break;
    case InspectorFileAction::NONE:
        break;
    }
}

void Editor::save_all() {
    Renderer* renderer = this->engine->get_singleton<Renderer>();
    World* world = this->engine->get_singleton<World>();
    const Project* project = this->engine->get_singleton<Project>();
    if (renderer == nullptr || world == nullptr) {
        return;
    }
    // Collect first: saving moves the entity out of the archetype being
    // iterated when it drops the tag.
    std::vector<EntityId> dirty;
    world->query<Material>().with<AssetDirty>().each([&](const EntityId entity, Material&) { dirty.push_back(entity); });
    const bool scene_pending = this->scene_needs_save();
    if (dirty.empty() && !scene_pending) {
        LOG::info(LOG_CATEGORY::EDITOR, "Nothing to save");
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
            LOG::info(LOG_CATEGORY::EDITOR, "Saved %s", shown.c_str());
            saved += 1;
        } else {
            LOG::error(LOG_CATEGORY::EDITOR, "Could not save %s", shown.c_str());
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

void Editor::apply_inspector_request(const InspectorRequest& request) {
    Renderer* renderer = this->engine->get_singleton<Renderer>();
    World* world = this->engine->get_singleton<World>();
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
            LOG::error(LOG_CATEGORY::EDITOR, "%s is not an asset entity", asset_name);
        } else if (MATERIAL::load(*renderer, asset->guid) == 0) {
            LOG::error(LOG_CATEGORY::EDITOR, "Material %s did not load", asset_name);
        } else {
            LOG::info(LOG_CATEGORY::EDITOR, "Loaded material %s", asset_name);
        }
        break;
    }
    case InspectorRequestKind::SET_MATERIAL_SHADER: {
        // A shader asset is named after its file's stem, which is the name
        // the library loads it by; loading an already compiled one is a lookup.
        const EntityId shader = SHADER_LIBRARY::load(*renderer, asset_name);
        if (shader == 0) {
            LOG::error(LOG_CATEGORY::EDITOR, "Shader %s did not load", asset_name);
        } else if (MATERIAL::set_shader(*renderer, request.target, shader)) {
            LOG::info(LOG_CATEGORY::EDITOR, "Material now uses shader %s", asset_name);
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

void Editor::open_import_dialog() {
    const Project* project = this->engine->get_singleton<Project>();
    std::string start;
    if (project != nullptr && project->is_open()) {
        start = project->root.string();
    }
    const int filter_count = static_cast<int>(sizeof(IMPORT_FILTERS) / sizeof(IMPORT_FILTERS[0]));
    this->file_dialog.open_files(this->sdl_window(), IMPORT_FILTERS, filter_count, start.empty() ? nullptr : start.c_str(), true);
}

void Editor::poll_import_dialog() {
    std::vector<std::filesystem::path> picked;
    std::string error;
    if (!this->file_dialog.take(&picked, &error)) {
        return;
    }
    if (!error.empty()) {
        LOG::error(LOG_CATEGORY::EDITOR, "Import dialog: %s", error.c_str());
        return;
    }
    for (const std::filesystem::path& path : picked) {
        this->queue_import(path);
    }
}

void Editor::queue_import(const std::filesystem::path& source) {
    const Project* project = this->engine->get_singleton<Project>();
    std::filesystem::path absolute = source;
    if (absolute.is_relative() && project != nullptr && project->is_open()) {
        absolute = project->root / source;
    }
    this->import_panel.open(absolute, this->asset_browser.current, &this->show_import);
}

void Editor::draw_main_menu() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        const Project* project = this->engine->get_singleton<Project>();
        const bool has_project = project != nullptr && project->is_open();
        const Scene* scene = this->engine->get_singleton<Scene>();
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
            PROJECT::scan(*this->engine);
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
        DemoPlugin* demo = this->demo();
        EditorGridPlugin* grid = this->grid();
        Renderer* renderer = this->engine->get_singleton<Renderer>();
        if (ImGui::MenuItem("Reload Shaders", "F5") && renderer != nullptr) {
            SHADER_LIBRARY::reload_all(*renderer);
        }
        ImGui::BeginDisabled(demo == nullptr);
        bool post = demo != nullptr && demo->post;
        if (ImGui::MenuItem("Post-process Pass", nullptr, &post) && demo != nullptr) {
            demo->set_post(*this->engine, post);
        }
        bool shadow = demo != nullptr && demo->shadow;
        if (ImGui::MenuItem("Shadow Pass", nullptr, &shadow) && demo != nullptr) {
            demo->set_shadow(*this->engine, shadow);
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::BeginDisabled(grid == nullptr || renderer == nullptr);
        bool grid_enabled = grid != nullptr && grid->grid.enabled;
        if (ImGui::MenuItem("Grid", nullptr, &grid_enabled) && grid != nullptr && renderer != nullptr) {
            grid->grid.set_enabled(*renderer, grid_enabled);
        }
        ImGui::EndDisabled();
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
}

void Editor::draw_viewport() {
    EditorViewportPlugin* viewport = this->viewport();
    EditorCameraPlugin* camera = this->camera();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool open = ImGui::Begin(PANELS::VIEWPORT, &this->show_viewport);
    ImGui::PopStyleVar();
    bool hovered = false;
    if (open && viewport != nullptr) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const u32 want_w = avail.x > 1.0f ? static_cast<u32>(avail.x) : 1;
        const u32 want_h = avail.y > 1.0f ? static_cast<u32>(avail.y) : 1;
        // Picked up by the viewport plugin before the next frame begins;
        // this frame still shows the current image stretched to fit.
        viewport->requested_width = want_w;
        viewport->requested_height = want_h;

        ImGui::Image(reinterpret_cast<ImTextureID>(viewport->viewport_texture[this->engine->gpu->slot]), avail);
        hovered = ImGui::IsItemHovered();
    }
    ImGui::End();
    if (camera != nullptr) {
        camera->viewport_hovered = hovered;
    }
}

void Editor::draw_stats() {
    if (ImGui::Begin(PANELS::STATS, &this->show_stats)) {
        const ImGuiIO& io = ImGui::GetIO();
        GpuContext* gpu = this->engine->gpu;
        const EditorViewportPlugin* viewport = this->viewport();
        const EditorCameraPlugin* camera = this->camera();
        ImGui::Text("Frame %llu", static_cast<unsigned long long>(gpu->frame_index));
        ImGui::Text("%.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
        ImGui::Text("dt %.3f ms", this->engine->last_dt * 1000.0f);
        ImGui::Text("Swapchain %ux%u, %u images", gpu->width, gpu->height, VK_ACCESS::swapchain_image_count(gpu));
        if (viewport != nullptr) {
            ImGui::Text("Viewport %ux%u", viewport->width, viewport->height);
        }
        if (camera != nullptr) {
            ImGui::Text("Camera speed %.2f u/s%s", camera->controller.speed, camera->controller.flying ? " (flying)" : "");
        }
        ImGui::Text("GPU %s", gpu->info.device_name);
    }
    ImGui::End();
}

// --- Scenes -----------------------------------------------------------------

// What Open Scene... and Save Scene As... offer; static for SDL (see
// IMPORT_FILTERS).
static constexpr SDL_DialogFileFilter SCENE_FILTERS[] = {
    {"Scenes (scene)", "scene"},
};

bool Editor::open_scene(const std::filesystem::path& absolute) {
    const std::string path = absolute.string();
    const AssetGuid guid = this->engine->load_asset_file(path.c_str());
    if (guid.is_null()) {
        LOG::error(LOG_CATEGORY::EDITOR, "Scene %s could not be registered", path.c_str());
        return false;
    }
    // Whatever was selected is about to be deleted with the old content.
    this->selection.clear();
    const bool ok = this->engine->load_scene(guid);
    if (!ok) {
        LOG::error(LOG_CATEGORY::EDITOR, "Scene %s did not load", path.c_str());
    }
    this->mark_scene_saved();
    return ok;
}

bool Editor::save_scene_to(const std::filesystem::path& absolute) {
    const std::string path = absolute.string();
    if (!this->engine->save_scene(path.c_str())) {
        LOG::error(LOG_CATEGORY::EDITOR, "Could not save scene %s", path.c_str());
        return false;
    }
    this->mark_scene_saved();
    LOG::info(LOG_CATEGORY::EDITOR, "Saved %s", this->scene_display_name().c_str());
    this->asset_browser.refresh();
    return true;
}

void Editor::save_scene() {
    const Scene* scene = this->engine->get_singleton<Scene>();
    if (scene != nullptr && scene->current_path[0] != '\0') {
        this->save_scene_to(scene->current_path);
    } else {
        this->save_scene_as();
    }
}

void Editor::save_scene_as() {
    const Project* project = this->engine->get_singleton<Project>();
    std::string start;
    if (project != nullptr && project->is_open()) {
        std::error_code error;
        const std::filesystem::path scenes = project->root / "scenes";
        start = (std::filesystem::is_directory(scenes, error) && !error ? scenes : project->root).string();
    }
    this->scene_dialog_kind = SceneDialog::SAVE_AS;
    if (!this->scene_dialog.save_file(this->sdl_window(), SCENE_FILTERS, 1, start.empty() ? nullptr : start.c_str())) {
        this->scene_dialog_kind = SceneDialog::NONE;
    }
}

void Editor::new_scene_now() {
    this->selection.clear();
    this->engine->new_scene();
    this->mark_scene_saved();
    LOG::info(LOG_CATEGORY::EDITOR, "New scene");
}

void Editor::request_new_scene() {
    this->pending = PendingAction::NEW;
    if (this->scene_needs_save()) {
        this->open_unsaved_popup = true;
    } else {
        this->perform_pending();
    }
}

void Editor::request_open_scene(const std::filesystem::path& file) {
    this->pending = file.empty() ? PendingAction::OPEN : PendingAction::OPEN_FILE;
    this->pending_file = file;
    if (this->scene_needs_save()) {
        this->open_unsaved_popup = true;
    } else {
        this->perform_pending();
    }
}

void Editor::request_quit() {
    this->pending = PendingAction::QUIT;
    if (this->scene_needs_save()) {
        this->open_unsaved_popup = true;
    } else {
        this->perform_pending();
    }
}

void Editor::perform_pending() {
    const PendingAction action = this->pending;
    this->pending = PendingAction::NONE;
    switch (action) {
    case PendingAction::NEW:
        this->new_scene_now();
        break;
    case PendingAction::OPEN: {
        const Project* project = this->engine->get_singleton<Project>();
        std::string start;
        if (project != nullptr && project->is_open()) {
            start = project->root.string();
        }
        this->scene_dialog_kind = SceneDialog::OPEN;
        if (!this->scene_dialog.open_files(this->sdl_window(), SCENE_FILTERS, 1, start.empty() ? nullptr : start.c_str(), false)) {
            this->scene_dialog_kind = SceneDialog::NONE;
        }
        break;
    }
    case PendingAction::OPEN_FILE:
        this->open_scene(this->pending_file);
        this->pending_file.clear();
        break;
    case PendingAction::QUIT:
        this->engine->quit();
        break;
    case PendingAction::NONE:
        break;
    }
}

u64 Editor::scene_hash() {
    DynamicArray<char> text;
    this->engine->write_scene(text);
    const u64 hash = HASH::fnv1a(text.data, text.count);
    text.free();
    return hash;
}

bool Editor::scene_needs_save() {
    if (this->scene_dirty) {
        return true;
    }
    // The safety net: a change the panels did not see (code, a script).
    return this->scene_hash() != this->scene_saved_hash;
}

std::string Editor::scene_display_name() const {
    const Scene* scene = this->engine->get_singleton<Scene>();
    if (scene == nullptr || scene->current_path[0] == '\0') {
        return "(unsaved scene)";
    }
    const Project* project = this->engine->get_singleton<Project>();
    if (project != nullptr && project->is_open()) {
        const std::filesystem::path relative = std::filesystem::path(scene->current_path).lexically_relative(project->root);
        if (!relative.empty() && relative.native()[0] != '.') {
            return relative.generic_string();
        }
    }
    return scene->current_path;
}

void Editor::draw_unsaved_scene_popup() {
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
    const Scene* scene = this->engine->get_singleton<Scene>();
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

void Editor::poll_scene_dialog() {
    std::vector<std::filesystem::path> picked;
    std::string error;
    if (!this->scene_dialog.take(&picked, &error)) {
        return;
    }
    const SceneDialog kind = this->scene_dialog_kind;
    this->scene_dialog_kind = SceneDialog::NONE;
    if (!error.empty()) {
        LOG::error(LOG_CATEGORY::EDITOR, "Scene dialog: %s", error.c_str());
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
            LOG::error(LOG_CATEGORY::EDITOR, "%s is not a .scene file", path.string().c_str());
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
