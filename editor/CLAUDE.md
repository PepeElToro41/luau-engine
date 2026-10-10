# standalone and editor

`LuauEngine::engine`: INTERFACE target linking all three.
`standalone/` and `editor/` link this.
Each has an `App` (`src/app.hpp`) that owns `DisplayWindow`, the `GpuContext*` from `GPU::init` and `Engine` and runs the loop.
Standalone renders the engine straight into `GpuFrame::backbuffer`;
the editor renders it into per-slot viewport textures (sRGB with a UNORM `sampled_format`, so ImGui copies the encoded bytes through to the UNORM swapchain) shown in an ImGui Viewport panel, barriers them to SAMPLED, and draws only the UI into the swapchain through `imgui_impl_vulkan`, fed by `gpu/vulkan/vk_access.hpp` (the one Vulkan-specific piece of the editor).
The editor has one selection, `editor/src/selection.hpp`: a `Selection` (kind NONE / ENTITY / FILE, the entity, and for a file its project-relative path plus the entity the file is loaded as, 0 when none) owned by the App and handed to the Explorer, the Asset Browser and the Inspector, so clicking in one panel replaces the other's highlight;
`App::resolve_selection` runs before the Inspector draws and, once per picked `.material`, registers the file (`Engine::load_asset_file`) and loads it with `MATERIAL::load` so the selection carries the Material entity, and `App::apply_file_action` performs the Inspector's Save (`MATERIAL::save` to the file's path) or Reload (`MATERIAL::reload`).
Editor panels live in `editor/src/ui/` (`output_panel`, `explorer_panel` (the scene tree: `draw(open, world, scene, selection)` lists `scene_root` and, under each expanded node, its `CHILD_OF` children through `SCENE::children`, read fresh every frame, then after a separator the editor's own unparented `editor_camera` entity (`ExplorerPanel::editor_camera`, set by the App) so its Transform can be inspected;
rows show the `Name`, the filter keeps matching subtrees open, a click calls `selection.select_entity`, a selected entity that dies clears it;
right-clicking a scene row opens a context menu whose New Entity spawns a child of it through `SCENE::spawn` with a `Name` and an identity `Transform`, selects it and opens the parent node on the next frame (`ExplorerPanel::reveal`),
right-clicking the tree's empty space does the same under `scene_root`, and editor-only rows get no menu),
`inspector_panel` (`draw(open, world, project, selection)`: for an entity an editable Name header, then one collapsing section per archetype id whose component entity has an `Inspector`, drawn by its function with an `InspectorContext` (`ui/inspectors/inspector_context.hpp`: world, entity, id, and a 128-byte per-(entity, id) scratch zeroed on first use and dropped when the selection changes) and turned into `World::modified` when it reports a change;
ids without one are listed under "Other" by name, pairs as `(CHILD_OF, parent)`;
for a file its name, kind, folder and size, and when the file is loaded as an entity the Save / Reload buttons (reported through `file_action` for the App) followed by that entity's component sections the same way),
the draw functions in `ui/inspectors/` (`INSPECTORS::register_all` exposes `Transform` (position, Euler-degree rotation cached in the scratch so the fields do not jump while editing, scale),
`Camera` (field of view in degrees, near, far),
`PrimitiveRenderer` (shape combo, material shown by name) and `Material` (`material_inspector.cpp`: the shader's name, every block member as a field by its reflected type (3- and 4-float vectors as HDR color edits, matrices one row per column, bools as checkboxes) written straight into the block, texture slots as a combo over the texture asset entities (`world.query<AssetUuid>().with<ECS::Pair<AssetType, AssetTexture>>()`, listed by `Name` sorted, plus `none`;
the GUID in the tooltip;
a GUID no entity carries is shown as such and kept),
sampler slots as their words parsed on Enter with the typed text held in the scratch) and `AssetUuid` (`asset_inspector.cpp`: type and GUID, read-only, under the "Asset" header)),
`asset_browser_panel` for the open project's files (a click calls `selection.select_file` with the project-relative path and the row highlight mirrors the selection, dropping when another panel selects;
rename and new-folder reselect the result, delete clears it;
double-clicking a file sets `activated`;
right-click opens a context menu: Import on importable files sets `activated` like a double-click, on a `.lunaasset` Reimport and Save Original... act on the kept original (`IMPORT::find_source` probed when the menu opens, both disabled when none was kept;
Reimport reports `reimport_source` + `reimport_asset` for the App to queue, Save Original... asks for a path in a modal and writes the bytes with `IMPORT::save_source`),
Delete removes the entry after a confirmation modal, Rename... (or F2 on the selection) asks for a new name in a modal, New Folder... creates a folder in the current one after asking for a name in a modal;
right-clicking the empty space of the listing offers New Folder..., Create and Refresh;
the toolbar's Create dropdown (the same entries as the Create submenu of both context menus: Material, Shader, Code > Luau / Cpp) writes a template file in the current folder through the name modal with the extension fixed (a `.material` with a fresh GUID on the `unlit` shader and `color = 1 1 1 1`, a `.slang` skeleton of the unlit shader, a `.luau` or `.cpp` stub;
`AssetBrowserPanel::create_file`) and selects it, so a new `.material` lands in the Inspector at once;
all reporting to the Output panel),
`import_panel` (a floating window: queue of source files, per-kind `MeshImportOptions` / `TextureImportOptions` including a "Keep original" checkbox, destination folder + name, runs `OBJ::import` / `IMAGE::import`;
`open_reimport(source, asset)` queues the original kept in a `.lunaasset` instead, titled Reimport (`PANELS::REIMPORT`, same `###Import` id),
header showing the asset and its original, destination defaulting to the asset itself, which keeps its GUID when written in place and gets a fresh one anywhere else),
`format.hpp` (`UI::format_size`),
`dock_layout` for the default docking, `panels.hpp` for the window titles);
the App draws Viewport and Stats itself.
The Viewport is drawn from the editor's own camera (`editor/src/editor_camera.hpp`: the editor-only `EditorCamera` tag and `EditorCameraController`, which spawns the unparented `editor_camera` entity with `Transform` + `Camera` + `EditorCamera` + `RenderCamera`, so it never appears in the scene tree, and each frame before the engine renders reads SDL's input state: right mouse held over the Viewport image enters fly mode with SDL relative mouse mode, the mouse yaws / pitches (no roll, pitch clamped),
WASD / E / Q move, Shift multiplies the speed, the wheel scales it;
releasing the button ends it and warps the cursor back to where it was;
the window focus flag is deliberately not consulted, since it flapped under a Wayland pointer lock and re-locked every frame).
File > Import... opens the OS picker through `editor/src/file_dialog.hpp` (`NativeFileDialog`, SDL3 `SDL_ShowOpenFileDialog`: async callback stores paths under a mutex, `take()` drains them on the main thread each frame);
the picked files and double-clicked `.obj` / image files in the Asset Browser both land in the Import panel, with the browser's current folder as destination.
The open project is `Project` (`editor/src/project.hpp`: root path + name),
editor-only since standalone loads assets differently;
the editor registers it as an engine singleton at startup.
There is no open-project page yet: the editor opens `EDITOR_TEST_PROJECT_DIR` (`editor/testing_project/`, set in `editor/CMakeLists.txt`;
it holds sample `meshes/*.obj` and `textures/*.png` to import, and `materials/demo.material`) and `App::scan_project_assets` registers every `.lunaasset` and `.material` under it with the provider through `Engine::load_asset_file`, so GUID references (a material's textures) resolve.
Importers live in `editor/src/import/`: `importer.hpp` (`ImportSource`: the bytes to import as a path + offset + size + original name + the asset's GUID when they sit inside a `.lunaasset`, `ImportSource::file(path)` for a plain file;
`IMPORT::read_file` / `read_source`, `find_source(asset)` probes a `.lunaasset` for its kept original, `save_source` writes it out, `fnv1a` for `content_hash`, `random_guid`, `add_editor_chunks(writer, source, bytes, size, keep_source)` writes the NAME / IMPS placeholders and the SRC chunk or its placeholder, `write_asset`),
`obj_importer.hpp` (`OBJ::parse` text -> `ObjMesh`, one interleaved 32-byte stream of position/normal/texcoord, fan-triangulated, one submesh per `usemtl`, smooth normals generated when absent, v flipped;
`OBJ::import` `ImportSource` -> `.lunaasset` through `MeshAssetWriter`) and `image_importer.hpp` (`IMAGE::decode` via vendored stb_image into a `DecodedImage` in R8/RG8/RGBA8 or the 16-bit UNORM variants, RGB promoted to RGBA;
`IMAGE::import` `ImportSource` -> `.lunaasset` through `TextureAssetWriter`).
Decoders stay in the editor because core has no external dependencies;
engine_tests links only core, so importer behaviour is not covered by `engine_tests`.
