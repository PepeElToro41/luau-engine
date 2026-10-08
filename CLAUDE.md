# LuauEngine

C++20 game engine built with CMake and Vulkan.

## Layout

The engine lives in `engine/` as three static libraries plus an umbrella target:

- `engine/core` -> `LuauEngine::core`: `defines.hpp`, `memory/`, `templates/`, `utils/`, `math/`, `ecs/`, `asset/`, `platform/`. No external dependencies. Keep it that way: tests and benchmarks link only this. `math/` is header-only (`math/math.hpp` includes it all): `Vector2` (scalar, 8 bytes), `Vector3` (SIMD, 16 bytes with a padding lane `w` that nothing reads; `load`/`store` move 12-byte vertex data), `Vector4`, `Quaternion` (a `Vector4` x, y, z, w with w scalar; `a * b` applies b first), `Matrix4x4` (four `Vector4` columns, column-major GLSL layout, column vectors). `math/simd.hpp` is the register layer, `SIMD::f32x4` with SSE2 on x86-64 and a scalar fallback elsewhere or with `ENGINE_MATH_FORCE_SCALAR` defined (verify both: `cmake -S . -B build-scalar -DCMAKE_CXX_FLAGS=-DENGINE_MATH_FORCE_SCALAR`). Binary helpers are free functions in `MATH::` (`dot`, `cross`, `lerp`, `min`, `max`, `approx_equal`, `slerp`, ...). Conventions: right-handed, +Y up, forward -Z, radians; `Matrix4x4::perspective` / `orthographic` produce Vulkan clip space (depth 0..1, Y negated) for a LESS depth test with depth cleared to 1.0; `look_at` is the world-to-view matrix. `platform/` is the OS layer: one public header per area (`platform/file.hpp`: `File` + `PLATFORM::file_open/close/size/read/write`, positional and 64-bit) implemented once per OS in `src/platform/linux/` and `src/platform/windows/`; every source there is wrapped in its platform macro since sources are globbed. Nothing else in core calls the OS directly; the asset reader and writer go through it. `asset/asset_view.hpp` is the `.lunaasset` byte layout (header, dependency table, chunk table, aligned payloads; `CHUNK_TYPE::*` tags) plus `AssetView`, a view over the prelude only: `AssetView::parse(data, size)` returns a view whose `is_ok()` / `parse_error` say how it went. `asset/asset_writer.hpp` is `AssetWriter` (collects dependencies and chunk payloads, `write()` emits the file) and `ASSET_FILE::write_file`. Neither knows about chunk contents. Nothing loads a whole asset file: `asset/asset_reader.hpp` (`AssetReader`) opens the file, keeps its header, and reads one chunk at a time by seeking with the view's chunk table (`read_chunk(view, tag, allocator)` returns a `ReadChunk`: bytes + entry + `read_error`; `read_chunk(entry, out)` reads into caller memory), `matches(view)` catches a stale view, `read_prelude` (on the reader, or `ASSET_FILE::read_prelude(path, allocator)`) returns a parsed view over a fresh buffer released with `ASSET_FILE::free_prelude`. `asset/asset_types/texture_asset.hpp` and `asset/asset_types/mesh_asset.hpp` define the texture and mesh payload layouts (plain on-disk structs, format enums, a validating `TextureAssetView` / `MeshAssetView` that takes the prelude view plus the descriptor chunk's bytes or `ReadChunk` and hands back the entries of the big chunks to read, and a `TextureAssetWriter` / `MeshAssetWriter` that builds the payloads and `add_chunks` them to an `AssetWriter`). `asset/asset_resource_provider.hpp` is the `AssetResourceProvider`: assets are registered by prelude view plus path (`add(view, path)`, keyed by the header GUID, prelude copied), `get(guid)` / `get(view)` lazily reads every runtime chunk into provider-owned 64-byte aligned buffers through `AssetReader` and returns the same `AssetResource` afterwards (`find_payload(tag, &entry)` gives the bytes to hand a per-type view), `get_chunk` reads one chunk, `unload` streams payloads out; a file re-imported since the view was taken is detected with `matches` and its prelude refreshed. The runtime `Engine` creates it as a singleton in `init()` and frees it in `shutdown()`. `docs/asset_format.md` is the specification and the place to record layout decisions. `utils/hash.hpp` is `HASH::fnv1a` / `fnv1a_str` (constexpr) / `fnv1a_append`, the one hash for names and cache keys. `VERTEX_LAYOUT` in `mesh_asset.hpp` fixes the shader input location of every vertex semantic (position 0, normal 1, tangent 2, color 3, texcoord0..3 4..7, joints 8, weights 9) and hashes a mesh's vertex layout for pipeline caching. `render_graph/render_graph.hpp` is the live `RenderGraph`: transient resources (`add_resource`, format defaulting to the backbuffer's, size relative or absolute) and passes (`add_pass` of kind DRAW_SCENE / FULLSCREEN / CLEAR / BLIT / CUSTOM) with color/depth attachments, sampled inputs, order (`move_pass`) and `enabled`; the two built-in resources `backbuffer` / `backbuffer_depth` are the FrameContext target, written by exactly one pass. Structural edits set `dirty`, live ones (clear values, constants, draw tag, fullscreen shader, custom callback) do not. Handles carry generations, names are looked up by hash. `render_graph/render_graph_plan.hpp` is the pure compiler: `RENDER_GRAPH::compile(graph, backbuffer, plan, diagnostics)` validates and produces a `RenderGraphPlan` (resolved resources with usage and frame start/end states, passes with per-attachment load/store/initial/final/previous states, the states inputs must be put in before each pass, and a render-pass compatibility key per raster pass that covers formats plus the dependency-defining states). No Vulkan types in core: formats are opaque `u32`s. One image per transient resource is shared by all frames in flight (one queue, one command buffer, render pass dependencies order the frames).
- `engine/graphics` -> `LuauEngine::graphics`: `display_window`, `gpu/`. Owns SDL3, volk, Slang, shaderc and imgui. `gpu/` is layered: `GpuDevice` (instance, device, queues, surface) -> `Swapchain` (images, acquire/present) -> `SwapchainTargets` / `OffscreenTarget` (render pass + framebuffer + a `DepthAttachment`, both expose a `RenderTarget`; every target has one color attachment and one depth attachment, format from `find_depth_format`, cleared each frame and never stored, so `vkCmdBeginRenderPass` takes `RENDER_TARGET_ATTACHMENT_COUNT` clear values filled by `render_target_clear_values`) -> `FrameScheduler` (frames in flight, fences, command buffers) -> `WindowPresenter` (bundles the previous three; `begin(FrameContext&)` / `end()`). Beside that chain, `gpu/resource_manager.hpp` is `GpuResourceManager`: creates `GpuBuffer` (device-local via staging, or host-visible and persistently mapped) and `GpuTexture` (2D, mipmapped, view over every level), uploads synchronously on the graphics queue (`upload_buffer`, `upload_texture` with per-mip `GpuTextureUpload`s, leaving the image in `SHADER_READ_ONLY_OPTIMAL`), and frees either immediately (`destroy_*`, caller guarantees idle) or deferred (`release_*` queues on the current frame slot; `begin_frame(slot)` frees that slot's queue once its fence has been waited on). The runtime `Engine` creates it as a singleton in `init()`, calls `begin_frame(frame.slot)` at the top of `render()`, and shuts it down in `shutdown()`; the apps already `wait_idle()` before that. `gpu/shaders/` is the shader path, one header per step. `shaders/shader.hpp` is the vocabulary they share (`ShaderStage`, `ShaderStageMask`, `ShaderLanguage` with a SLANG value reserved, `SHADER::stage_name`) and pulls in no Vulkan. `shaders/preprocessing.hpp` is everything done to the text before a compiler sees it: `ShaderDefine`, the engine's `#pragma pass <tag> [vertex] [fragment]` directives (`SHADER_PREPROCESSING::parse_directives` / `strip_directives`, `ShaderDirectives`), the `STAGE_*` / `PASS_<TAG>` define names (`stage_define` / `pass_define`), the Slang entry point names (`stage_entry_point`: `vertex` / `fragment` / `compute`), `enable_includes` (GLSL only: inserts the include-directive extension after `#version`, keeping line numbers) and `resolve_include` (the including file's directory, then the roots); its source is the one graphics file compiled into `engine_tests`. `shaders/compilation.hpp`: `ShaderSource` (text + stage + language + entry point) -> `SHADER_COMPILER::compile` / `compile_file` -> `CompiledShader` (allocator-owned SPIR-V words, entry point, and a `log` with the compiler's errors or warnings; nothing is printed, the caller decides; `CompiledShader::from_spirv` wraps precompiled words). One backend per language behind the private `src/gpu/shaders/compilation_backends.hpp`, both targeting Vulkan 1.1 / SPIR-V 1.3: `compilation_slang.cpp` (libslang from the SDK, the only file that includes it; loads the module from the text with the defines as macros and `include_dirs` as search paths, links the `[shader("...")]` function named by `entry_point`, checks its stage, returns its SPIR-V with entry point `main`; the session uses column-major matrix layout so `float4x4` reads `Matrix4x4` as-is and `mul(m, v)` is GLSL's `m * v`; `on_include` gets the module's dependency files) and `compilation.cpp` (shaderc for GLSL, confined there; `#include` through an includer that calls `resolve_include`). `shaders/module.hpp`: `SHADER::create_module` / `destroy_module` / `stage_info` / `to_vk_stage`; shader modules are only needed during pipeline creation, so they have no deferred-destroy path. `gpu/pipeline.hpp` is `GraphicsPipeline`: `init(gpu, target, GraphicsPipelineDesc)` builds the layout (push constant ranges, descriptor set layouts) and pipeline against the target's render pass from `CompiledShader`s (vertex required, fragment optional), with vertex bindings/attributes added through `add_binding` / `add_attribute`, fixed state defaults of triangle list, back-face cull, counter-clockwise front face (correct together with the Y-negating projections), depth test LESS with write, `PipelineBlend` opaque/alpha/additive, and dynamic viewport + scissor. `bind`, `push_constants`; `shutdown()` needs an idle GPU, `release(resources)` defers destruction through `GpuResourceManager::release_pipeline` for pipelines replaced while frames that bound them are in flight (shader reloads). `init(gpu, VkRenderPass, desc)` builds against any render pass with `desc.color_attachment_count` blend states (0 for depth-only). `gpu/render_graph_backend.hpp` realizes a `RenderGraphPlan`: `RenderGraphBackend::realize(plan)` owns one `GpuTexture` per used transient and a `VkRenderPass` + `VkFramebuffer` per raster pass (reused when unchanged, otherwise released through `GpuResourceManager::release_render_pass` / `release_framebuffer` / `release_texture`); the pass that writes the backbuffer borrows the FrameContext target's render pass. `execute(frame, plan, graph, executor)` records the passes in order, emitting the barriers the plan asks for against per-image tracked layouts, and hands each raster pass to a `RenderPassExecutor` (`draw_scene` / `fullscreen` / `custom`) with a `RenderPassContext` (command buffer, slot, live pass desc, render pass, compat key, input views + sampler). Pipelines are built against `render_pass_for_key(key)`. `gpu/descriptor.hpp` is the descriptor infrastructure: `DescriptorLayoutCache` (bindings hash -> layout, `empty_layout()`), `DescriptorAllocator` (growing pool list; FREE flag for long-lived sets, `reset()` per frame slot for transient ones), `DescriptorWriter` (batched `vkUpdateDescriptorSets`), `SamplerCache`. `shaders/reflection.hpp` wraps the vendored spirv-reflect (confined to `reflection.cpp`): `SHADER_REFLECT::merge(CompiledShader, ShaderReflection&)` folds a stage's bindings (with uniform block members), push constants and vertex inputs into one fixed-array `ShaderReflection`. `shaders/program.hpp` is `SHADER_PROGRAM::load`: a shader file -> `ShaderProgram` (`language` from the extension, `.slang` or `.glsl`; per `#pragma pass`: compiled vertex/fragment stages, reflection, set layouts, push range; plus the material interface shared by its passes). Slang passes compile the `vertex` / `fragment` functions of the one file (no stage ifdefs), GLSL passes compile `main` per `#ifdef STAGE_*` section. The set convention it enforces: set 0 = the renderer's frame block (`render/engine/frame.slang`), set 1 = pass inputs (binding i = input i), set 2 = material (one uniform block + samplers, identical in every pass that declares it), set 3 reserved; per-object data is the push constant block (`render/engine/object.slang`); set layouts give every binding both stages so vertex-only passes share them. `gpu/vertex_input.hpp` builds a pipeline's vertex input from a vertex stage's reflected inputs and a `GpuMeshLayout` under the `VERTEX_LAYOUT` convention; `gpu/asset_formats.hpp` maps `VertexFormat` / `TextureFormat` to `VkFormat`; `gpu/format_utils.hpp` has the depth/stencil/aspect helpers.
- `engine/runtime` -> `LuauEngine::runtime`: the `Engine` object (simulation + renderer, later physics etc). It owns no window or swapchain: the app calls `update(dt)` and `render(const FrameContext&)`, and the `FrameContext` says which command buffer and `RenderTarget` to draw into. Engine-wide globals are singletons keyed by type (`engine.create_singleton<T>(...)` once, `engine.get_singleton<T>()` after, nullptr if missing); apps may register their own types too. `init()` creates the `AssetResourceProvider`, `GpuResourceManager`, the ECS `World` and the `Renderer`; `load_asset_file(path)` registers a `.lunaasset` and returns its GUID. Links core and graphics. `render/renderer.hpp` is the `Renderer` singleton, the moddable frame: it owns the `RenderGraph` (`renderer->graph`, edited by apps and later scripts; `build_default_graph()` is one DRAW_SCENE pass tagged `forward` into the backbuffer), recompiles and realizes it when dirty or when the target changes (a failed compile keeps the last good plan and logs the diagnostics once; with none it just clears the target), and is the graph's executor. Shaders and materials are ECS entities (`render/components.hpp`): `load_shader("unlit")` finds `<project>/render/unlit.slang` (or `.glsl`) then the same under `<ENGINE_RENDER_DIR>` (repo `render/`, both are include roots) and stores the `ShaderProgram` in a `Shader` component (`reload_shader` / `reload_all_shaders` swap it in place, bump `generation`, release its pipelines); `create_material(shader)` makes a `Material` entity whose set-2 values are set by name (`set_float` / `set_vec4` / `set_texture`, ...) and flushed into the frame slot's uniform region and descriptor set at the top of `render()`; `MeshRenderer{mesh guid, material entity}` + `Transform` is what `draw_scene` draws (first `Camera` + `Transform` entity is the view). Components are plain data (the World copies them); removed hooks hand their GPU state back to the renderer, so `Engine::shutdown` frees the World before the Renderer. `render/gpu_asset_cache.hpp` uploads mesh / texture assets on first use by GUID (`add_mesh` for procedural geometry). `pipeline_for(shader, tag, compat_key, mesh layout)` is the pipeline cache; `register_custom_pass(name, fn)` serves CUSTOM passes; `log_sink` routes messages (the editor sends them to its Output panel). `render/demo_scene.hpp` is the stand-in scene both apps spawn (cube, camera, unlit material) with toggles for a post-process pass, a depth-only shadow pass and a textured material (editor: Render menu; standalone: F5 reload, F6 post, F7 shadow, `--post` / `--shadow`).
- `LuauEngine::engine`: INTERFACE target linking all three. `standalone/` and `editor/` link this. Each has an `App` (`src/app.hpp`) that owns `DisplayWindow`, `GpuDevice`, `WindowPresenter` and `Engine` and runs the loop. Standalone renders the engine straight into the swapchain; the editor renders it into per-slot `OffscreenTarget`s shown in an ImGui Viewport panel and draws only the UI into the swapchain. Editor panels live in `editor/src/ui/` (`output_panel`, `explorer_panel`, `asset_browser_panel` for the open project's files (double-clicking a file sets `activated`; right-click opens a context menu: Import on importable files sets `activated` like a double-click, Delete removes the entry after a confirmation modal, reporting to the Output panel), `import_panel` (a floating window: queue of source files, per-kind `MeshImportOptions` / `TextureImportOptions`, destination folder + name, runs `OBJ::import` / `IMAGE::import`), `format.hpp` (`UI::format_size`), `dock_layout` for the default docking, `panels.hpp` for the window titles); the App draws Viewport and Stats itself. File > Import... opens the OS picker through `editor/src/file_dialog.hpp` (`NativeFileDialog`, SDL3 `SDL_ShowOpenFileDialog`: async callback stores paths under a mutex, `take()` drains them on the main thread each frame); the picked files and double-clicked `.obj` / image files in the Asset Browser both land in the Import panel, with the browser's current folder as destination. The open project is `Project` (`editor/src/project.hpp`: root path + name), editor-only since standalone loads assets differently; the editor registers it as an engine singleton at startup. There is no open-project page yet: the editor opens `EDITOR_TEST_PROJECT_DIR` (`editor/testing_project/`, set in `editor/CMakeLists.txt`; it holds sample `meshes/*.obj` and `textures/*.png` to import). Importers live in `editor/src/import/`: `importer.hpp` (`IMPORT::read_file`, `fnv1a` for `content_hash`, `random_guid`, `add_editor_chunks` placeholders, `write_asset`), `obj_importer.hpp` (`OBJ::parse` text -> `ObjMesh`, one interleaved 32-byte stream of position/normal/texcoord, fan-triangulated, one submesh per `usemtl`, smooth normals generated when absent, v flipped; `OBJ::import` file -> `.lunaasset` through `MeshAssetWriter`) and `image_importer.hpp` (`IMAGE::decode` via vendored stb_image into a `DecodedImage` in R8/RG8/RGBA8 or the 16-bit UNORM variants, RGB promoted to RGBA; `IMAGE::import` through `TextureAssetWriter`). Decoders stay in the editor because core has no external dependencies; engine_tests links only core, so importer behaviour is not covered by `engine_tests`.

Each module has `include/engine/...` (public) and `src/...`; sources are globbed, so new files need no CMake edit. Includes always use the `engine/` prefix regardless of module.

Vendored third-party code is copied under `vendor/<lib>/` with a hand-written `CMakeLists.txt` (imgui, doctest, nanobench, stb: `stb_image.h` compiled once in `stb_image.cpp` as `stb::image`, PNG/JPEG/BMP/TGA only, no stdio; spirv-reflect: `spirv_reflect.c/.h` from the Vulkan SDK as `spirv_reflect::spirv_reflect`, linked only by graphics).

Shaders live in the repo's `render/` directory (`ENGINE_RENDER_DIR`), written in Slang, one `.slang` file per shader: `#pragma pass <tag> [vertex] [fragment]` lines name the render passes it draws in, and the stages are the `[shader("vertex")] ... vertex(...)` and `[shader("fragment")] ... fragment(...)` functions of that same file, with uniforms declared once above them and the varyings as the struct passed from one to the other (`SV_Position` for the clip position). Each pass compiles those functions with `PASS_<TAG>` and `STAGE_*` defined, so `#ifdef PASS_SHADOW` specializes code per pass and a vertex-only pass simply never asks for `fragment`. `render/engine/*.slang` is the include library: `frame.slang` (set 0 `frame`), `object.slang` (push constants `object`), `bindings.slang` (`MATERIAL(b)` = set 2 binding b, `PASS_INPUT(i)` = set 1 binding i), `vertex.slang` (`VERTEX_POSITION` ... `VERTEX_WEIGHTS`: the `[[vk::location(N)]]` of each `VERTEX_LAYOUT` semantic, required because Slang numbers inputs in declaration order; declare only what the shader reads), `fullscreen.slang` (the fullscreen-triangle `vertex` function and its `FullscreenVertex`). Matrices in blocks are column-major `Matrix4x4`; `mul(m, v)` is GLSL's `m * v`. A project's own `render/` directory overrides files by name. `.glsl` shaders (one `main` per `#ifdef STAGE_*` section, `layout(set, binding)` by hand) still load through the shaderc backend. Slang's `SV_VertexID` is relative to the base vertex, which is why the device enables `shaderDrawParameters`.

## Build

```sh
cmake -S . -B build
cmake --build build
```

Binaries land in `build/bin`. `compile_commands.json` is exported to the build directory.

CMake options: `LUAU_ENGINE_BUILD_TESTS` (ON), `LUAU_ENGINE_BUILD_BENCHES` (ON), `LUAU_ENGINE_SANITIZE` (OFF, adds ASan + UBSan to every target; use a separate build dir).

## Tests

doctest, one binary `engine_tests` built from every `.cpp` under `tests/`.

```sh
cmake --build build --target engine_tests && ./build/bin/engine_tests   # all tests
./build/bin/engine_tests -tc="ecs/world*"                               # filter by test-case name (wildcards)
./build/bin/engine_tests -sf="*entity_index*"                           # filter by source file
./build/bin/engine_tests -s                                             # also print passing assertions
ctest --test-dir build --output-on-failure                              # same, via CTest (one entry per TEST_CASE)

cmake -S . -B build-asan -DLUAU_ENGINE_SANITIZE=ON
cmake --build build-asan --target engine_tests && ./build-asan/bin/engine_tests
```

Conventions:

- `tests/core/<dir>/<unit>_tests.cpp` mirrors `engine/core/{include,src}/engine/<dir>/<unit>`; `tests/graphics/gpu/shaders/preprocessing_tests.cpp` is the one test outside core, covering the graphics source `engine_tests` compiles in directly (see `tests/CMakeLists.txt`). Nothing else from graphics goes in: the binary must build without SDL3 / Vulkan.
- Start every file with `#include "support/test_support.hpp"` (doctest, sample component types `Position`/`Velocity`/`Health`/`TagA`/`TagB`/`Likes`/`Eats`, `CHECK_ARENA_CLEAN()`, `HookLog`).
- `TEST_CASE` names are `"<area>/<unit>: <behaviour>"`, e.g. `"ecs/world: delete_entity bumps the generation"`, so `-tc="ecs/world*"` selects a unit. Use `SUBCASE` for variants that share setup.
- No RAII fixtures (the no-destructor rule applies to tests). World tests are explicit: `World world; world.init(); ... world.free(); CHECK_ARENA_CLEAN();`. Free every container you create.
- `ENGINE_ASSERT` aborts, so assertion paths are not tested; test documented return values instead. Never assert exact `TypeId` values.
- A test that exposes a suspected engine bug keeps the documented behaviour, is decorated `* doctest::may_fail()` and carries an `// ENGINE BUG?:` comment, rather than being bent to pass.

## Benchmarks

nanobench, one binary `engine_bench` built from every `.cpp` under `bench/`. Build it in Release; a Debug build runs but warns, since `ENGINE_ASSERT` and `-O0` skew the numbers.

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target engine_bench && ./build-release/bin/engine_bench   # all
./build-release/bin/engine_bench ecs/component hash_map                                 # names containing any argument
./build-release/bin/engine_bench --list                                                 # names only
```

Adding a benchmark is one `BENCH_CASE("<area>/<unit>: <what>") { ... }` block in a `bench/core/<dir>/<unit>_bench.cpp` file (see `bench/support/bench.hpp`); it self-registers, nothing else changes. `bench` is a configured `ankerl::nanobench::Bench`; call `bench.run("label", [&] { ... })` one or more times. Use `set<T>` (not `add<T>`) for data components, wrap results in `ankerl::nanobench::doNotOptimizeAway`, and use `bench.batch(n)` when a run does `n` operations so the table shows per-operation cost. When every iteration needs a fresh index into a pre-built pool (an entity that has not had the id yet), use `BENCH::run_indexed(bench, "label", count, [&](usz i) { ... })`: it calls the lambda exactly `count` times with `i` from 0 to `count - 1` (see `bench/core/ecs/fresh_entity_bench.cpp`).

## C++ conventions

### Use `struct`, never `class`

Every type is declared with `struct`. Do not use the `class` keyword, even for types with private members. Use explicit `public:` / `private:` sections when access control is needed.

```cpp
// Good
struct Engine {
    void init();

private:
    VkInstance instance = VK_NULL_HANDLE;
};

// Bad
class Engine { ... };
```

### No `m_` prefix; access members through `this->`

Member variables are plain names with no `m_` (or `_`, or trailing `_`) prefix. Inside member functions, always access members explicitly through `this->` so they are distinguishable from locals and parameters.

```cpp
// Good
struct Engine {
    void init();

private:
    VkInstance instance = VK_NULL_HANDLE;
};

void Engine::init() {
    VkResult result = vkCreateInstance(&createInfo, nullptr, &this->instance);
    if (result != VK_SUCCESS) {
        this->instance = VK_NULL_HANDLE;
    }
}

// Bad
class Engine {
    VkInstance m_instance = VK_NULL_HANDLE;
};

void Engine::init() {
    vkCreateInstance(&createInfo, nullptr, &m_instance);
}
```

### snake_case for functions and variables

Functions, methods, member variables, locals, and parameters use `snake_case`. Type names stay `PascalCase`.

```cpp
// Good
struct VulkanBackend {
    void begin_frame();

private:
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
};

// Bad
struct VulkanBackend {
    void beginFrame();

private:
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
};
```

### No destructors

Do not declare destructors, not even defaulted or virtual ones. Cleanup is explicit: types that own resources expose a `shutdown()` (or similar) method that the owner calls. Because base types have no virtual destructor, never `delete` an object through a base pointer; destroy it as its concrete type.

The single exception is `TemporalAllocator` (`engine/include/engine/memory/temporal_allocator.hpp`): its destructor rewinds the thread's `MAIN_ARENA` to the mark taken at creation, which is the entire purpose of the type. Do not add others.

```cpp
// Good
struct VulkanBackend : RendererBackend {
    void init() override;
    void shutdown() override;
};

// Bad
struct VulkanBackend : RendererBackend {
    ~VulkanBackend() override { this->shutdown(); }
};
```

## Vulkan target: 1.1

The engine targets Vulkan 1.1. Do not use features from later core versions:

- No dynamic rendering (`VK_KHR_dynamic_rendering` / `vkCmdBeginRendering`). Use `VkRenderPass` and `VkFramebuffer`.
- No timeline semaphores (`VK_KHR_timeline_semaphore`). Use binary semaphores and fences.
- No `synchronization2`; use the original `vkCmdPipelineBarrier` / `VkSubmitInfo` APIs.

Request `VK_API_VERSION_1_1` when creating the instance and require it when picking a physical device. The one feature enabled beyond core 1.0 is `shaderDrawParameters` (core in 1.1, via `VkPhysicalDeviceVulkan11Features`), which Slang's `SV_VertexID` needs.

### Namespaces use SCREAMING_CASE

Namespaces group free functions and are named in `SCREAMING_CASE`. Functions inside keep `snake_case`.

```cpp
// Good
namespace DISPLAY_WINDOW {
bool sdl_initialize();
void sdl_shutdown();
}

// Bad
namespace display_window { ... }
namespace DisplayWindow { ... }
```
