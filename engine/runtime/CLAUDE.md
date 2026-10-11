# engine/runtime

`engine/runtime` -> `LuauEngine::runtime`: the `Engine` object (simulation + renderer, later physics etc).
It owns no window or swapchain: the app owns the `GpuContext` and its frames, calls `update(dt)` and `render(FrameContext&)` with the `GpuFrame` from `GPU::begin_frame` plus the target texture (the swapchain image or an editor viewport texture) and its state, and reads the state the target was left in from `target_state`.
Engine-wide globals are singletons keyed by type (`engine.create_singleton<T>(...)` once, `engine.get_singleton<T>()` after, nullptr if missing);
apps may register their own types too.
`init(gpu)` creates the `AssetResourceProvider`, the ECS `World`, the `Scene` (its `scene_root`;
spawn scene entities with `scene->spawn(name, parent)`),
the `AssetEntities` index (every asset `load_asset_file` registers gets, or updates, its unparented entity with `AssetUuid`, `(AssetType, tag)` and `Name`;
`get_singleton<AssetEntities>()->find(guid)` is it;
freed first in `shutdown`) and the `Renderer` (driven through `RENDERER::`, `SHADER_LIBRARY::` and `MATERIAL::`;
its `Shader` / `Material` entities are named but unparented, so they never appear in the scene tree),
the `FrameAllocator` (core `memory/frame_allocator.hpp`: one `ENGINE_FRAME_ARENA_BYTES` arena for whatever a frame builds and drops, `Renderer::frame_allocator`;
`update` resets it, so it is one `update` per frame and nothing from it survives into the next;
a full arena returns nullptr and logs once per frame, growing it is a TODO) and registers the render components;
`load_asset_file(path)` registers a `.lunaasset` or a text asset (`.material`, `.scene`) and returns its GUID.
Scenes are files (core `asset/asset_types/scene_asset.hpp`): `load_scene(guid)` unloads the content under `scene_root` once the registered `.scene`'s text is in hand and instantiates the file there, setting `Scene::current` / `current_path`;
`instantiate_scene(guid, parent)` instantiates it under any alive entity without unloading, which is a prefab;
`save_scene(path)` writes the content under `scene_root` with `Scene::current` as guid (a fresh `GUID::random()` for a new scene), registers the file and makes it current;
`write_scene(out)` is the text a save would write (the editor hashes it to know whether the scene changed);
`new_scene()` unloads and forgets current;
`serialize_context(ctx)` fills a `SerializeContext` with the Engine's resolver (a material GUID loads the material through `MATERIAL::load`, a shader GUID the shader through `SHADER_LIBRARY::load`, any other asset is its entity) and the renderer log as the warning sink, so line-level problems reach the editor's Output panel and only a structurally broken file fails.
`shutdown` frees the World first (its removed hooks release shader programs, material blocks and pipelines),
then the renderer, so the app must have waited for the GPU to go idle.
Links core and graphics.
`render/demo_scene.hpp` is the stand-in scene (`camera` plus `cube`, `sphere` and `cylinder` `PrimitiveRenderer` entities under `scene_root`, one unlit material) both apps fall back to when the project's `scenes/demo.scene` (the same content, drawn with `materials/demo.material`) is missing or does not load, with toggles for a post-process pass, a depth-only shadow pass, a textured material (`set_texture`) and a `.material` asset (`set_material(engine, guid)`) (editor: Render menu;
standalone: F5 reload, F6 post, F7 shadow, `--post` / `--shadow`).
