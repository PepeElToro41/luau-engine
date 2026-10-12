#pragma once

#include "engine/engine.hpp"
#include "engine/render/renderer.hpp"
#include "file_dialog.hpp"
#include "selection.hpp"
#include "ui/asset_browser_panel.hpp"
#include "ui/explorer_panel.hpp"
#include "ui/import/import_panel.hpp"
#include "ui/inspector_panel.hpp"
#include "ui/output_panel.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <filesystem>
#include <string>

struct DemoPlugin;
struct EditorCameraPlugin;
struct EditorGridPlugin;
struct EditorViewportPlugin;

// The editor's state and panels, owned by EditorUiPlugin and drawn once
// per frame from its "ui" system (draw()). The window, GPU and loop are
// the engine's plugins; this struct reaches them through `engine`.
//
// The scene is a file: the scene plugin opens the project's
// scenes/demo.scene at startup, File > New / Open / Save / Save As and a
// double-clicked .scene in the Asset Browser switch it, and Save All writes
// it after the dirty materials. Whether it needs saving is a flag the
// panels raise when they change scene content (`scene_dirty`), backed by a
// comparison of the serialized text's hash with the one recorded at the
// last save, so an edit made by other code is caught at the prompts: New,
// Open and Quit ask Save / Discard / Cancel when something would be lost.
struct Editor {
    // Draws the dockspace, the menus and every open panel; called between
    // ImGui::NewFrame and ImGui::Render.
    void draw();
    // Records the current scene as the saved state (after the startup
    // scene loaded).
    void mark_scene_saved();
    // New / Open / Quit first ask what to do with unsaved changes; the
    // chosen action is kept in `pending` and performed by perform_pending()
    // once the scene is saved or discarded. An empty `file` for Open means
    // "show the file dialog".
    void request_new_scene();
    void request_open_scene(const std::filesystem::path& file = {});
    void request_quit();
    // Renderer messages land in the Output panel.
    static void render_log(RenderLogLevel level, const char* text, void* user_data);

    Engine* engine = nullptr;
    OutputPanel output;
    // What the Explorer or the Asset Browser picked last (selection.hpp);
    // `resolved_file` is the file whose entity was last looked up.
    Selection selection;
    std::filesystem::path resolved_file;
    ExplorerPanel explorer;
    InspectorPanel inspector;
    AssetBrowserPanel asset_browser;
    ImportPanel import_panel;

private:
    // The plugins the panels read and write; nullptr when not added.
    SDL_Window* sdl_window() const;
    EditorCameraPlugin* camera() const;
    EditorGridPlugin* grid() const;
    EditorViewportPlugin* viewport() const;
    DemoPlugin* demo() const;

    void open_import_dialog();
    void poll_import_dialog();
    void queue_import(const std::filesystem::path& source);
    void draw_main_menu();
    // Gives a file selection the entity it is loaded as (a .material's
    // Material entity), once per file picked.
    void resolve_selection();
    // Performs the Inspector's Save / Reload request on the selected file.
    void apply_file_action(InspectorFileAction action);
    // File > Save All (Ctrl+S): writes every file-backed entity tagged
    // AssetDirty (the materials) back to its file, then the scene when it
    // needs saving, and reports each in the Output panel.
    void save_all();
    // Performs what a component's draw function asked for: loading a picked
    // .material, or compiling a picked shader and giving it to a Material.
    void apply_inspector_request(const InspectorRequest& request);
    void draw_viewport();
    void draw_stats();

    // --- Scenes ----------------------------------------------------------------

    // Registers the file and loads it as the scene; clears the selection
    // and the dirty state. False (reported) when it did not load.
    bool open_scene(const std::filesystem::path& absolute);
    // Writes the scene to `absolute` (Engine::save_scene) and records the
    // saved state. False (reported) when it could not be written.
    bool save_scene_to(const std::filesystem::path& absolute);
    // Saves to the scene's own file, or asks for one when it has none.
    void save_scene();
    // Shows the save dialog; a pending action (see above) runs after a
    // successful save.
    void save_scene_as();
    // Empties the scene and forgets its file.
    void new_scene_now();
    void perform_pending();
    // Whether the scene differs from its file: the flag, else the hash.
    bool scene_needs_save();
    u64 scene_hash();
    // The scene's path relative to the project, or "(unsaved scene)".
    std::string scene_display_name() const;
    void draw_unsaved_scene_popup();
    void poll_scene_dialog();

    bool reset_layout = false;
    NativeFileDialog file_dialog;
    // The Open Scene... / Save Scene As... dialog, and which one is up.
    NativeFileDialog scene_dialog;
    enum struct SceneDialog : u8 { NONE, OPEN, SAVE_AS };
    SceneDialog scene_dialog_kind = SceneDialog::NONE;
    // What the Save / Discard / Cancel prompt is guarding.
    enum struct PendingAction : u8 { NONE, NEW, OPEN, OPEN_FILE, QUIT };
    PendingAction pending = PendingAction::NONE;
    std::filesystem::path pending_file;
    bool open_unsaved_popup = false;
    // Scene content changed since the last load or save, as far as the
    // panels know; `scene_saved_hash` is the serialized text at that point.
    bool scene_dirty = false;
    u64 scene_saved_hash = 0;

    bool show_explorer = true;
    bool show_inspector = true;
    bool show_viewport = true;
    bool show_output = true;
    bool show_stats = true;
    bool show_asset_browser = true;
    bool show_import = false;
    bool show_demo_window = false;
};
