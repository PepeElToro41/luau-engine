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
its `Shader` / `Material` entities are named but unparented, so they never appear in the scene tree) and registers the render components;
`load_asset_file(path)` registers a `.lunaasset` and returns its GUID.
`shutdown` frees the World first (its removed hooks release shader programs, material blocks and pipelines),
then the renderer, so the app must have waited for the GPU to go idle.
Links core and graphics.
`render/demo_scene.hpp` is the stand-in scene both apps spawn (`camera` plus `cube`, `sphere` and `cylinder` `PrimitiveRenderer` entities under `scene_root`, one unlit material) with toggles for a post-process pass, a depth-only shadow pass, a textured material (`set_texture`) and a `.material` asset (`set_material(engine, guid)`;
the editor applies the test project's `materials/demo.material` at startup) (editor: Render menu;
standalone: F5 reload, F6 post, F7 shadow, `--post` / `--shadow`).
