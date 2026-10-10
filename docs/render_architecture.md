# Render architecture: frontend / backend split

Status: done, 2026-10-09. Every step of the migration below is in: the
backend API (`gpu/gpu_types.hpp`, `gpu/gpu.hpp`), the Vulkan backend
(`src/gpu/vulkan/`), the shader stack (`engine/shaders/`), the frontend
(`engine/render/` in graphics), the Engine and both apps on it, and
`gpu-old` deleted. Verified under the validation layers: standalone with
the default graph and with the post + shadow passes, the editor with its
viewport (an sRGB texture sampled through a UNORM view) and a textured
cube, screenshots checked for the cube's pixels. CLAUDE.md describes what
exists; this document keeps the reasoning.

Follow-ups, in no order: the D3D12 backend (`src/gpu/d3d12/`, a second
`GpuBackend` table); reflection from Slang's own API so DXIL can be
reflected; the HLSL semantics in `vertex.slang` for DXIL; dropping the
`combined_sampler` flag once no shader uses `Sampler2D`; tests over the
Vulkan-free pieces (pipeline keys, draw-list sorting, the executor's
reuse decisions with a recording stub backend); per-material bind group
caching across frames if material counts ever make the per-frame
transient groups measurable.

## Goals

- One backend API in `gpu/` that Vulkan implements now and Direct3D 12 (and
  maybe Metal) implement later. No OpenGL, so the API can assume the modern
  explicit model: command lists, immutable pipelines, bind groups, explicit
  resource states, render target formats baked into pipelines.
- Nothing above `gpu/` names a Vulkan type. That is the test of the split:
  `volk.h` is included only under `gpu/vulkan/` and the editor's ImGui glue.
- Data-oriented frontend: plain structs holding state, free functions in
  `SCREAMING_CASE` namespaces acting on them, flat arrays built once per frame
  and then consumed. No virtual executors, no per-entity GPU bookkeeping.
- Setting globals and material values is a struct write or a by-name setter
  into a CPU blob. Descriptor sets, layouts, pools and their per-slot
  retirement stop being something the frontend knows about.

What stays as it is:

- The shader path: preprocess -> compile -> reflect -> `ShaderProgram`, stored
  in a `Shader` component on a World entity. It only changes the vocabulary
  of its outputs (no Vulkan enums) and no longer creates descriptor layouts.
- The resource manager's logic (staging uploads, per-slot deferred release)
  becomes the Vulkan backend's resource implementation behind the new API.
- `GpuDevice` and `Swapchain` move into the Vulkan backend as they are.
- The core `RenderGraph` and planner. They are already Vulkan-free.

## Layers

```
engine/graphics/include/engine/
  gpu/              backend API, API-neutral. Vulkan-free headers.
    gpu_types.hpp   formats, enums, handles, descs (no functions)
    gpu.hpp         GPU:: device, frames, resources, pipelines, bind groups, commands
    vulkan/         escape hatch for code that must see Vulkan (editor ImGui):
      vk_access.hpp VK::instance/device/queue/command_buffer/image_view/render_pass(...)
  shaders/          compile stack, moved out of gpu/ (preprocessing, compilation,
                    reflection, program). Depends on gpu/gpu_types.hpp only.
  render/           frontend. Depends on gpu/, shaders/, core (ECS, assets, graph).
engine/graphics/src/gpu/vulkan/   the Vulkan backend: everything from gpu-old,
                                  reshaped behind GPU::
engine/runtime/                   Engine, singletons, demo scene. No render code.
```

The frontend moves from `runtime/render/` into `graphics/render/` (the empty
`engine/renderer.hpp` already points that way). Runtime keeps `Engine`, the
singleton registry, `load_asset_file` and the demo scene. Alternative: leave
the frontend in runtime; nothing technical forces either, but `graphics` is
where the shader stack and the gpu already are, and the editor includes
render headers directly either way.

Backend selection is at runtime and immutable: `GPU::init(desc)` takes
`desc.backend`, fills a private table of function pointers
(`src/gpu/backend.hpp`, one `GpuBackend` struct per backend returned by
`VULKAN_BACKEND::table()` / `D3D12_BACKEND::table()`), and every `GPU::`
function forwards through it. Nothing refills the table and there is one
`GpuContext` per process; switching backends is a restart.
`GPU::available(kind)` says which backends this build contains, so the
apps can pick the platform default and fall back.

## Backend API (`gpu/`)

### Handles

A resource is a small struct the frontend copies freely: a `SparseId` into
a `SparseList` pool the backend owns, plus the API-neutral facts about the
object so nothing has to call the backend to read them.

```cpp
struct GpuBuffer      { SparseId id; u64 size; u32 usage; GpuMemoryKind memory; };
struct GpuTexture     { SparseId id; GpuFormat format; u32 width, height, mip_levels; u32 usage; };
struct GpuSampler     { SparseId id; };
struct GpuPipeline    { SparseId id; };
struct GpuBindLayout  { SparseId id; };   // one descriptor set layout / root table layout
struct GpuBindGroup   { SparseId id; };   // one descriptor set / descriptor table
struct GpuCommandList { SparseId id; };   // VkCommandBuffer / ID3D12GraphicsCommandList
```

The id is the only thing the backend resolves (`SparseList::get_element_alive`,
one page lookup, generation checked, so a stale handle is caught rather
than dereferenced). `GPU_NULL_ID` (0) is "no object"; backends reserve the
zero slot of every pool. ECS components can hold these as plain data.

### Vocabulary (`gpu_types.hpp`)

- `GpuFormat`: one enum covering vertex, texture and depth formats; the
  mapping to `VkFormat` / `DXGI_FORMAT` lives in each backend. The asset
  `VertexFormat` / `TextureFormat` enums map onto it (replaces
  `asset_formats.hpp`). The render graph's opaque `RenderFormat` becomes this
  enum's value.
- `GpuResourceState`: UNDEFINED, COLOR_ATTACHMENT, DEPTH_ATTACHMENT, SAMPLED,
  TRANSFER_SRC, TRANSFER_DST, STORAGE, PRESENT. The planner already uses
  this set; it moves here and the planner includes it.
- `GpuBindingType`: UNIFORM_BUFFER, STORAGE_BUFFER, TEXTURE, SAMPLER,
  STORAGE_TEXTURE. No combined image sampler (D3D12 and Metal have none).
- `ShaderStageMask` (already in `shaders/shader.hpp`), `GpuShaderBytecode`
  (SPIRV, DXIL, METALLIB).
- Descs: `GpuBufferDesc`, `GpuTextureDesc`, `GpuSamplerDesc`,
  `GpuBindLayoutDesc` (bindings: slot, type, stages, count),
  `GpuBindGroupDesc` (entries: slot, buffer+offset+range | texture | sampler),
  `GpuPipelineDesc`, `GpuRenderPassDesc` (begin-time: color attachments
  with texture + load/store/clear, optional depth).

### Functions (`gpu.hpp`, namespace `GPU`)

```cpp
// Device and frames
bool  init(GpuInitDesc{ SDL_Window*, bool validation }, GpuContext& out);   // instance, device, queues, swapchain
void  shutdown(GpuContext&);
void  wait_idle(GpuContext&);
bool  begin_frame(GpuContext&, GpuFrame& out);   // waits the slot's fence, retires deferred releases,
                                                  // resets transient pools, acquires the swapchain image,
                                                  // begins the slot's command list
bool  end_frame(GpuContext&, GpuFrame&);          // ends the list, submits, presents
void  mark_resized(GpuContext&);
GpuTexture swapchain_texture(const GpuFrame&);    // this frame's backbuffer as an ordinary texture handle

// Resources (the resource manager, with handles)
GpuBuffer  create_buffer(GpuContext&, const GpuBufferDesc&, const void* data = nullptr);
bool       write_buffer(GpuContext&, GpuBuffer, const void* data, u64 size, u64 offset);   // host-visible
bool       upload_buffer(GpuContext&, GpuBuffer, const void* data, u64 size, u64 offset);  // staging, sync
GpuTexture create_texture(GpuContext&, const GpuTextureDesc&);
bool       upload_texture(GpuContext&, GpuTexture, const GpuTextureUpload* mips, u32 count);
GpuSampler sampler(GpuContext&, const GpuSamplerDesc&);              // cached by hash, never released
void       release(GpuContext&, GpuBuffer | GpuTexture | GpuPipeline | GpuBindGroup);   // deferred to the slot
void       destroy(GpuContext&, ...);                                 // immediate, caller guarantees idle

// Binding
GpuBindLayout bind_layout(GpuContext&, const GpuBindLayoutDesc&);    // cached by hash
GpuBindGroup  create_bind_group(GpuContext&, GpuBindLayout, const GpuBindGroupDesc&);        // long-lived
GpuBindGroup  transient_bind_group(GpuContext&, GpuBindLayout, const GpuBindGroupDesc&);     // valid this frame
GpuUniformRange push_uniforms(GpuContext&, const void* data, u32 size);   // per-slot ring, valid this frame

// Pipelines
GpuPipeline create_pipeline(GpuContext&, const GpuPipelineDesc&);

// Commands, on the frame's list
void cmd_barrier(GpuCommandList, GpuTexture, GpuResourceState from, GpuResourceState to);
void cmd_begin_render_pass(GpuCommandList, const GpuRenderPassDesc&);
void cmd_end_render_pass(GpuCommandList);
void cmd_bind_pipeline(GpuCommandList, GpuPipeline);
void cmd_bind_group(GpuCommandList, u32 set, GpuBindGroup);
void cmd_push_constants(GpuCommandList, const void* data, u32 size);
void cmd_bind_vertex_buffer(GpuCommandList, u32 binding, GpuBuffer, u64 offset);
void cmd_bind_index_buffer(GpuCommandList, GpuBuffer, GpuIndexType);
void cmd_draw_indexed(GpuCommandList, u32 index_count, u32 first_index, i32 vertex_offset);
void cmd_draw(GpuCommandList, u32 vertex_count);
void cmd_set_viewport(GpuCommandList, u32 width, u32 height);   // viewport + scissor, always dynamic
void cmd_clear(GpuCommandList, GpuTexture, clear value);
void cmd_blit(GpuCommandList, GpuTexture src, GpuTexture dst, bool linear);
```

Decisions baked in so the API is not Vulkan-shaped:

- **Render passes are begin-time descriptions.** `cmd_begin_render_pass`
  takes textures and load/store ops. Attachments enter in their attachment
  state and leave in it; every other transition is an explicit
  `cmd_barrier` by the frontend (a `from` of UNDEFINED when the pass clears
  or discards, so nothing is preserved needlessly). That makes the Vulkan
  render pass cache key just formats + ops + textures (what
  `RenderGraphBackend::create_render_pass` / `create_framebuffer` and
  `SwapchainTargets` do today, minus the layout juggling), and the planner's
  per-attachment initial/final states stop mattering; only `previous` does.
  D3D12 maps it to `OMSetRenderTargets` + clears, Metal to
  `MTLRenderPassDescriptor`. Vulkan 1.1 is respected: no dynamic rendering,
  the render pass objects just stop being visible.
- **Pipelines carry target formats, not a render pass.** `GpuPipelineDesc`
  has `color_formats[]`, `depth_format`, sample count. The Vulkan backend
  creates a compatible template render pass from those (compatibility only
  depends on formats and sample counts). The frontend's pipeline cache key
  becomes (shader, generation, pass tag, vertex layout hash, format hash)
  and the planner's `compat_key` is no longer needed by the frontend.
- **Barriers are state transitions.** `cmd_barrier(from, to)`; the Vulkan
  backend derives layouts and stage/access masks, D3D12 derives resource
  states, Metal mostly ignores them. The backend tracks nothing; the
  planner already tells the frontend every state before every pass.
- **Bind groups are allocate-and-write in one call.** No separate writer, no
  pool handling in the frontend. Transient ones come from a per-slot pool
  the backend resets in `begin_frame`; long-lived ones are freed with
  `release`. `push_uniforms` is a per-slot host-visible ring so that "put
  this struct in a uniform buffer for this frame" is one call returning a
  buffer + offset to drop into a bind group entry.
- **Push constants are one block** of at most 128 bytes, both stages. Root
  constants on D3D12, `setBytes` on Metal.
- **Shader bytecode is bytes**, with a `GpuShaderBytecode` kind, not SPIR-V
  words. `create_pipeline` takes `CompiledShader`s and creates the modules
  itself; `shaders/module.hpp` goes away.
- **Separate textures and samplers**, no combined image samplers.

### Vulkan backend layout (`src/gpu/vulkan/`)

Mostly code that exists today, moved and reshaped:

| File | From |
| --- | --- |
| `vk_context.hpp` | `VulkanContext`: device, swapchain, frame slots, the `SparseList` pools, caches; one namespace per file |
| `vk_backend.cpp` | the `GpuBackend` table, init / shutdown order, pool lookups (`VK_CONTEXT::`) |
| `vk_device.cpp` | `device.cpp`, as is, plus filling `GpuInfo` |
| `vk_swapchain.cpp` | `swapchain.cpp`; images registered as external textures in the pool |
| `vk_frame.cpp` | `frame_scheduler.cpp` + `window_presenter.cpp`: slots, begin / end frame, deferred releases |
| `vk_resources.cpp` | `resource_manager.cpp`, handles in front |
| `vk_binding.cpp` | `descriptor.cpp`: layout cache, per-slot transient pools, long-lived pool, sampler cache, uniform ring |
| `vk_pipeline.cpp` | `pipeline.cpp` (neutral descs in, modules created inside) + template render passes |
| `vk_render_pass.cpp` | render pass / framebuffer caches, `cmd_begin/end_render_pass` |
| `vk_commands.cpp` | the other `cmd_*` functions and the barrier |
| `vk_formats.cpp` | `asset_formats.cpp` + `format_utils.cpp`: `GpuFormat` <-> `VkFormat`, state table |
| `vk_access.cpp` | the editor's escape hatch (`engine/gpu/vulkan/vk_access.hpp`) |

`vk_check.hpp` stays as the private assert helper, routing through
`GPU::log`. The neutral side (`src/gpu/gpu.cpp` dispatch and log sink,
`src/gpu/gpu_types.cpp` format tables and desc helpers) has no Vulkan.

Facts learned while bringing it up:

- Render pass compatibility requires identical subpass dependencies, not
  only formats, so the template passes carry the same two external
  dependencies as the real ones (`VK_RENDER_PASS::standard_dependencies`).
- `vertex_input.cpp` (reflection + mesh layout -> attributes) is frontend
  logic and did not move into the backend; the frontend fills
  `GpuVertexInput` itself.
- The uniform ring is 4 MB per frame slot; `push_uniforms` logs an error
  when it is full rather than growing.

## Shader stack (`shaders/`)

Same four steps, same files, moved from `gpu/shaders/` to `shaders/`, with
these changes:

- `CompiledShader`: `const u8* bytes`, `usz size`, `GpuShaderBytecode kind`.
- `ShaderReflection`: `GpuBindingType` instead of `VkDescriptorType`,
  `GpuFormat` instead of `VkFormat`, `ShaderStageMask` instead of
  `VkShaderStageFlags`. `layout_bindings` returns a `GpuBindLayoutDesc`.
- `ShaderProgram` / `ShaderPassProgram` hold bytecode + reflection only. No
  descriptor set layouts, no push constant range struct. Loading needs no
  device, so the program is a pure function of the file and the compile
  options. The frontend asks the backend for layouts from the reflection
  when it builds pipelines.
- `module.hpp` stays in `gpu-old/shaders/` for the old pipeline code and
  goes with it (the backend creates modules inside `create_pipeline`).
- Reflection reports a combined image sampler (Slang `Sampler2D`) as a
  TEXTURE with `combined_sampler` set. `layout_bindings` refuses it with a
  message naming the binding, so the new frontend rejects such shaders;
  the old renderer still binds them through the shim. `render/*.slang`
  switch to `Texture2D` + `SamplerState` with the new frontend.
- `CompiledShader::stage_desc()` is the `GpuShaderStageDesc` for
  `GpuPipelineDesc`; `from_spirv` became `from_bytecode(stage, kind, ...)`.
- Target selection: `SHADER_COMPILER::compile` takes the bytecode kind. Slang
  emits SPIR-V, DXIL or Metal from one source; the GLSL path (shaderc) only
  emits SPIR-V, so `.glsl` shaders become a Vulkan-only convenience.
- Reflection today runs spirv-reflect over SPIR-V. For DXIL it will have to
  come from Slang's own reflection API, which is target-independent. That is
  a later swap behind the same `SHADER_REFLECT::merge`.

Shader-side conventions that change, in `render/engine/*.slang`:

- `Sampler2D` becomes `Texture2D` plus a `SamplerState`. Set 0 gains a fixed
  table of engine samplers (`linear_repeat`, `linear_clamp`, `nearest`, ...)
  so most materials never declare their own sampler and
  `set_texture(material, name, guid)` needs no `SamplerDesc`. A material
  can still declare a `SamplerState` in set 2 and the frontend binds it
  from the material's sampler settings.
- `vertex.slang` macros keep `[[vk::location(N)]]`; when D3D12 arrives they
  also expand to the HLSL semantic (`: POSITION`, `: TEXCOORD0`, ...) since
  DXIL matches vertex inputs by semantic. One-line change per macro.

## Frontend (`render/`)

All plain structs plus namespaces. Files:

- `render/components.hpp`: `Transform`, `Camera`, `MeshRenderer`,
  `PrimitiveRenderer`, `Shader`, `Material`. A `PrimitiveRenderer` names one
  of the core's unit-sized shapes (`engine/geometry/primitives.hpp`: cube,
  sphere, cylinder) instead of a mesh asset; the asset cache generates and
  uploads each shape once and every entity using it shares that `GpuMesh`.
  The shape is a closed form, so physics can later derive a collider from
  it and the Transform's scale without any triangles. `Material` becomes CPU data only: shader entity, shader
  generation, `u8* params` blob laid out by the reflection, texture slots
  (binding + GUID). No uniform buffer, no descriptor sets, no dirty mask,
  no retirement. `Shader` is the `ShaderProgram` plus `generation`.
- `render/shader_library.hpp`: `SHADER_LIBRARY::load(renderer, world, name)`
  / `reload` / `reload_all`: path resolution (project then engine render
  dir), program load, entity creation, generation bump. Pipelines keyed by
  the old generation simply stop matching.
- `render/materials.hpp`: `MATERIAL::create`, `set_float` ... `set_texture`; `MATERIAL::load(renderer, guid)` builds the same entity from a `.material` text asset through the provider (`docs/asset_format.md`, "Material files"), typing each value by the shader's reflection; `reload` and `save` round-trip it.
  Each is a reflection lookup and a `memcpy` into the blob.
- `render/pipelines.hpp`: `PipelineCache` (hash map from
  `{shader, generation, tag, vertex_layout, target_formats}` to
  `GpuPipeline`) and `PIPELINES::get(cache, ...)`, which builds the
  `GpuPipelineDesc` from the pass's reflection, the mesh layout
  (`VERTEX_LAYOUT` convention, the old `vertex_input.cpp` logic without
  Vulkan types) and the target formats. Prunes entries whose shader entity
  died or whose generation moved on.
- `render/gpu_asset_cache.hpp`: unchanged in role, handles instead of
  structs.
- `render/graph_executor.hpp`: what `RenderGraphBackend` does today, written
  once against `GPU::` so every backend shares it. `GRAPH::realize(plan)`
  creates one `GpuTexture` per used transient (reused while the description
  hash is unchanged, released otherwise) including the backbuffer depth,
  which stops being the app's job. `GRAPH::execute(plan, graph)` walks the
  passes in order: barriers the plan asks for, `cmd_begin_render_pass` with
  the attachments, then a switch on pass kind to `draw_scene`, `fullscreen`,
  `clear`, `blit`, `custom`. No executor interface.
- `render/draw_scene.hpp`: the DRAW_SCENE pass in two steps. Collect: walk
  `Transform + MeshRenderer` and `Transform + PrimitiveRenderer`, resolve
  mesh (asset or shared primitive) and material, get the pipeline,
  append a `DrawItem { pipeline, material bind group, mesh, submesh, model }`
  to a flat `DynamicArray`. Sort by pipeline then material. Record: one loop
  binding only what changed. Materials used this frame get their params
  pushed through `push_uniforms` and a transient bind group once per frame
  (cached per material entity in a per-frame map, so ten cubes with one
  material cost one upload).
- `render/renderer.hpp`: the `Renderer` struct is the state bag the Engine
  owns as a singleton: `GpuContext*`, graph + plan + last good plan +
  diagnostics, the executor's textures, the pipeline cache, the asset
  cache, the shader library, the `FrameUniforms` struct the app writes to,
  custom pass table, log sink. `RENDERER::init`, `shutdown`,
  `render(renderer, world, frame)`, `build_default_graph`,
  `register_custom_pass`. `render` writes set 0 (frame uniforms through
  `push_uniforms` + the sampler table, one transient group), recompiles the
  graph when dirty or the target changed, realizes, executes.

Globals: `renderer.frame_uniforms` is a struct mirroring `frame.slang`; the
Engine fills view/projection/time before render from the camera tagged
`RenderCamera` (the app's choice: the editor's fly camera or the scene's), or
the first camera when none is tagged.
Anything added to the struct is one line in C++ and one in Slang. If scripts
later need by-name access, the same by-name setters as materials work
against `frame.slang`'s reflection, since the frame block is reflected like
any other.

`FrameContext` stays the handoff from app to engine, Vulkan-free:

```cpp
struct FrameContext {
    GpuFrame frame;          // command list, slot, frame index
    GpuTexture target;       // where the graph's backbuffer goes: swapchain image or editor texture
    u32 width, height;
    GpuFormat format;
};
```

## Apps

- Standalone: `GPU::init(window)`, loop `GPU::begin_frame` ->
  `engine.render({frame, GPU::swapchain_texture(frame)})` -> `GPU::end_frame`.
  `WindowPresenter`, `SwapchainTargets`, `OffscreenTarget`, `DepthAttachment`
  and `RenderTarget` disappear.
- Editor: creates one `GpuTexture` per slot (COLOR_ATTACHMENT | SAMPLED) for
  the viewport and hands it as the target. ImGui keeps using
  `imgui_impl_vulkan`, fed through `vk_access.hpp` (instance, device, queue,
  the swapchain's render pass, the viewport texture's view and a sampler).
  That file is the one Vulkan-specific piece of the editor and would be
  swapped for `imgui_impl_dx12` glue on D3D12.

## Migration, in steps that each leave the tree building

0. **Make the tree build again.** The old sources still include
   `engine/gpu/...`, which now resolves to the new stubs. Move
   `src/gpu/*.cpp` (not `shaders/`) to `src/gpu-old/` and sed their
   includes and the runtime/editor/standalone includes to
   `engine/gpu-old/...`. The old renderer keeps running on `gpu-old` until
   step 5 replaces it, so every step below can be built and run.
1. **Backend API headers** (`gpu/gpu_types.hpp`, `gpu/gpu.hpp`). Review
   point: everything else sits on these.
2. **Vulkan backend** in `src/gpu/vulkan/`, one file at a time from the
   table above, with a throwaway `standalone` path (clear + one triangle)
   to validate device, frame, swapchain and render pass cache under the
   validation layers before anything depends on them.
3. **Shader stack** moved to `shaders/` with neutral types. Pure move plus
   type substitutions; `preprocessing_tests.cpp` keeps covering it.
4. **Frontend** in `graphics/render/`, bottom-up: components, materials,
   pipelines, asset cache, graph executor, draw scene, renderer. Engine
   switches over when `RENDERER::render` draws the demo cube.
5. **Apps**: standalone first (trivial), then the editor's viewport and
   ImGui glue. Verify post-process, shadow and textured demo toggles, shader
   reload, resize, and both apps under validation.
6. **Delete** `gpu-old/`, `src/gpu-old/`, `runtime/render/`. Update CLAUDE.md
   and this document to describe what exists.

Tests that become possible because the new pieces are Vulkan-free:
`gpu_types` format tables, `PipelineCache` keys and pruning, `DrawItem`
sorting, the graph executor's reuse/release decisions (with a recording stub
backend built only for `engine_tests`). Not planned for the first pass, but
the layering should not make them impossible.

## Decisions (2026-10-09)

1. Frontend lives in `graphics/render/`.
2. Material uniforms are re-pushed every frame through the uniform ring;
   a material has no GPU state of its own.
3. Sampler convention: engine sampler table in set 0 plus optional material
   samplers. (Default taken; not discussed.)
4. Backend selection at runtime through a function table, chosen once by
   `GPU::init` and immutable afterwards. Switching is a restart.
5. Resources are structs holding a `SparseId` into backend pools plus their
   API-neutral facts (size, format, extent, usage).

The backend API itself is drafted in `engine/graphics/include/engine/gpu/
gpu_types.hpp` and `gpu.hpp`; the other files currently in that directory
are leftovers to delete when the Vulkan backend lands.
