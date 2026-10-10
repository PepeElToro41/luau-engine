# engine/graphics

`engine/graphics` -> `LuauEngine::graphics`: `display_window`, `gpu/` (backend),
`shaders/` (shader path),
`render/` (frontend).
Owns SDL3, volk, Slang, shaderc and imgui.
`docs/render_architecture.md` holds the design reasoning.
`gpu/` is the API-neutral backend API and includes no Vulkan: `gpu_types.hpp` is the vocabulary (`GpuFormat`, one enum for vertex, texture and depth formats with `GPU_FORMAT::` queries and `from_vertex` / `from_texture`;
`GpuResourceState`;
usage flags;
the resource structs `GpuBuffer` / `GpuTexture` / `GpuSampler` / `GpuPipeline` / `GpuBindLayout` / `GpuBindGroup` / `GpuCommandList`, each a `SparseId` into a backend pool plus its neutral facts, id 0 = null;
the descs `GpuBufferDesc`, `GpuTextureDesc` (optional `sampled_format` reinterpretation),
`GpuSamplerDesc`, `GpuBindLayoutDesc` (slots of `GpuBindingType`;
no combined image samplers, D3D12 and Metal have none),
`GpuBindGroupDesc` (`bind_buffer` / `bind_texture` / `bind_sampler`),
`GpuPipelineDesc` (stages as `GpuShaderStageDesc` bytes + kind, `GpuVertexInput`, raster/depth/blend state, up to 4 bind layouts, one push constant block, `GpuTargetFormats`: pipelines are tied to formats, never to a render pass object),
`GpuRenderPassDesc` (begin-time attachments with load/store/clear),
`GpuFrame`).
`gpu.hpp` is `GpuContext` plus `GPU::`: `init(GpuInitDesc)` selects the backend once (runtime choice, immutable afterwards, one context per process) and every call forwards through the table in `src/gpu/backend.hpp`;
`begin_frame` / `end_frame` (frames in flight, deferred releases, transient pools, swapchain acquire and present;
`GpuFrame::backbuffer` is the swapchain image as an ordinary texture, in UNDEFINED state);
resources with `destroy_*` (now, GPU idle) versus `release_*` (deferred to the current slot);
`sampler` and `bind_layout` cached by hash;
`create_bind_group` (long-lived) / `transient_bind_group` (this frame) allocate and write in one call;
`push_uniforms` copies into the slot's 4 MB uniform ring and returns a `GpuUniformRange` to bind;
`create_pipeline`;
the `cmd_*` functions on the frame's command list (`cmd_barrier(texture, from, to)` always emits, even for from == to;
`cmd_begin_render_pass` takes attachments already in their attachment state and leaves them there, every other transition is the caller's barrier;
it also sets a full viewport).
The Vulkan backend is `src/gpu/vulkan/` (`vk_context.hpp` lists its files and the `VulkanContext` pools;
`VkRenderPass` + `VkFramebuffer` cached by formats + ops + textures;
pipelines built against a template pass per `GpuTargetFormats` that shares `VK_RENDER_PASS::standard_dependencies` with the real ones, since compatibility needs identical dependencies;
validation layers in debug builds;
messages through `GPU::set_log_sink`).
`gpu/vulkan/vk_access.hpp` is the only public header with Vulkan in it, for the editor's ImGui glue.
`shaders/` is the shader path, one header per step, Vulkan-free: `shader.hpp` (`ShaderStage`, `ShaderStageMask`, `ShaderLanguage`, `SHADER::stage_name`),
`preprocessing.hpp` (`ShaderDefine`, the `#pragma pass <tag> [vertex] [fragment]` directives (`SHADER_PREPROCESSING::parse_directives` / `strip_directives`),
the `STAGE_*` / `PASS_<TAG>` define names, Slang entry point names, `enable_includes` (GLSL) and `resolve_include`;
its source is the one graphics file compiled into `engine_tests`),
`compilation.hpp` (`ShaderSource` -> `SHADER_COMPILER::compile` / `compile_file` -> `CompiledShader`: allocator-owned bytes, `GpuShaderBytecode` kind, entry point and a `log`;
`stage_desc()` is what `GpuPipelineDesc` takes, `from_bytecode` wraps precompiled code, `ShaderCompileOptions::target` is SPIR-V only so far;
one backend per language behind the private `src/shaders/compilation_backends.hpp`: `compilation_slang.cpp` (libslang, SPIR-V 1.3, column-major matrices so `mul(m, v)` is GLSL's `m * v`, `on_include` reports dependency files) and `compilation.cpp` (shaderc for GLSL, `#include` through `resolve_include`)),
`reflection.hpp` (spirv-reflect confined to `reflection.cpp`;
`SHADER_REFLECT::merge(CompiledShader, ShaderReflection&)` folds a stage's bindings (`GpuBindingType`, a `combined_sampler` flag for Slang `Sampler2D`, uniform block members),
push constants and vertex inputs (`GpuFormat`) into one fixed-array `ShaderReflection`;
`layout_bindings(reflection, set, GpuBindLayoutDesc&)` gives a set's layout with both stages, refusing combined samplers;
`same_set`),
`program.hpp` (`SHADER_PROGRAM::load`: a shader file -> `ShaderProgram`, per `#pragma pass` the compiled stages and reflection, plus the material interface shared by the passes;
needs no device;
the set convention it enforces: set 0 a subset of `ShaderProgramLoadDesc::frame_interface`, set 1 pass inputs, set 2 the material (one uniform block plus textures and samplers, identical in every pass that declares it),
set 3 reserved).
`render/` is the frontend, written once against `GPU::` so every backend shares it: `components.hpp` (`Transform`, `Camera`, the `RenderCamera` tag that marks the one camera the frame is drawn from (the renderer falls back to the first `Camera` when none is tagged;
the editor tags its own fly camera, the standalone the scene's),
`MeshRenderer` (a mesh asset GUID + material entity),
`PrimitiveRenderer` (a `PrimitiveShape` + material entity;
drawn exactly like a MeshRenderer, the geometry generated and uploaded once per shape through `GPU_ASSETS::get_primitive` and shared by every entity using it),
`Shader` = `ShaderProgram` + `generation` + its bind layouts, `Material` = CPU block + texture and sampler slots, no GPU state, `FrameUniforms`;
`RENDER_COMPONENTS::register_all` names the component entities and installs the removed hooks),
`renderer.hpp` (the `Renderer` state struct: graph + plans + diagnostics, executor, pipeline cache, asset cache, custom passes, the frame's set 0 (`frame_layout`, `frame_interface`, the `RENDERER_SAMPLER_*` table, the `white` texture),
per-frame `frame_group` and `material_groups`, `log_sink`;
`FrameContext` = `GpuFrame` + target texture + `target_state` in/out;
`RenderPassContext` for pass functions and custom callbacks;
`RENDERER::init` / `shutdown` / `render` / `build_default_graph` (one DRAW_SCENE pass tagged `forward` into the backbuffer) / `register_custom_pass` / `log`),
`shader_library.hpp` (`SHADER_LIBRARY::load(renderer, name)` finds `<project>/render/<name>.slang` (or `.glsl`) then the engine's `render/`, both include roots, into a named unparented `Shader` entity;
`reload` / `reload_all` swap the program in place and bump `generation`),
`materials.hpp` (`MATERIAL::create(renderer, shader)` and the by-name setters `set_float` ...
`set_mat4` / `set_texture` / `set_sampler`;
`sync` rebuilds the block after a reload;
`MATERIAL::load(renderer, guid)` turns a registered `.material` asset into a Material on the asset's own entity (the one holding its `AssetUuid`, found with `ASSET_ENTITY::find`;
a plain new entity when there is none) named after the file (shader loaded by name through `SHADER_LIBRARY::load`, every param / texture / sampler applied by reflection, mismatches logged and skipped, the text unloaded from the provider afterwards;
`Renderer::materials` maps asset GUID to entity so the same asset loads once, `Material::asset` points back, the removed hook drops the entry),
`reload` / `reload_all` re-read the file into the same entity, `to_asset` / `save(renderer, entity, path)` write an entity's current values back as a file (the entity needs a non-null `Material::asset` guid),
`sampler_desc` / `sampler_asset` convert between the file's `MaterialSamplerDesc` and `GpuSamplerDesc`),
`pipelines.hpp` (`PIPELINES::get(renderer, shader, program, tag, formats, mesh layout)` cached by shader entity, generation, pass tag, vertex layout hash and target formats;
`release_shader`),
`vertex_input.hpp` (`GpuMeshLayout`, `VERTEX_INPUT::build` matches reflected inputs to mesh attributes under `VERTEX_LAYOUT`),
`gpu_asset_cache.hpp` (`GPU_ASSETS::get_mesh` / `get_texture` upload on first use by GUID, `add_mesh` for procedural geometry, `get_primitive(shape)` builds and uploads a primitive on first use into `GpuAssetCache::primitives[shape]`, tessellated per `primitive_tessellation`),
`graph_executor.hpp` (`GRAPH_EXECUTOR::realize` owns one texture per used transient including `backbuffer_depth`, reusing unchanged ones;
`execute` barriers resources to the planned states, opens a render pass per raster pass and dispatches to the pass functions in `src/render/passes.cpp`: `draw_scene` collects every `Transform` + `MeshRenderer` or `PrimitiveRenderer` whose material's shader has the tag into a draw list sorted by pipeline then material, pushes each used material's block through the ring once per frame into a transient set-2 group, and records;
`fullscreen` draws the pass's shader entity with the inputs in set 1;
`custom` calls the callback).
