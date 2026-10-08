#pragma once

#include "engine/asset/asset_resource_provider.hpp"
#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/gpu/descriptor.hpp"
#include "engine/gpu/device.hpp"
#include "engine/gpu/pipeline.hpp"
#include "engine/gpu/render_graph_backend.hpp"
#include "engine/gpu/render_target.hpp"
#include "engine/gpu/resource_manager.hpp"
#include "engine/gpu/shaders/reflection.hpp"
#include "engine/gpu/vertex_input.hpp"
#include "engine/math/math.hpp"
#include "engine/render/components.hpp"
#include "engine/render/gpu_asset_cache.hpp"
#include "engine/render_graph/render_graph.hpp"
#include "engine/render_graph/render_graph_plan.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "engine/templates/hash_map.hpp"

// The engine's renderer: the render graph that describes the frame, the
// shaders and materials as ECS entities, and everything shared between
// them (pipeline cache, descriptor pools, uploaded assets, the per-frame
// uniforms). It is the Engine singleton that apps and (later) scripts reach
// for to shape the frame:
//
//     Renderer* renderer = engine.get_singleton<Renderer>();
//     EntityId unlit = renderer->load_shader("unlit");            // render/unlit.slang
//     EntityId red = renderer->create_material(unlit);
//     renderer->set_vec4(red, "color", Vector4(1, 0, 0, 1));
//     world->set(cube, MeshRenderer{cube_guid, red});
//
//     RenderGraph& graph = renderer->graph;                       // takes effect next frame
//     RenderPassHandle post = graph.add_pass("post", RENDER_PASS_FULLSCREEN);
//
// The frame. render() recompiles the graph when it is dirty or the target's
// format or size changed, keeps the last good plan when a compile fails
// (printing the diagnostics once per graph version), and clears the target
// when there is no good plan at all. Before executing it writes the slot's
// FrameUniforms from the first Camera entity and flushes every Material
// whose values changed into that slot's uniform region and descriptor set;
// nothing touches a descriptor set once recording has started.
//
// The renderer is the graph's executor: draw_scene() draws every entity
// with a Transform and a MeshRenderer whose material's shader has a pass
// for the graph pass's tag; fullscreen() draws a pass's shader entity over
// the target with the pass inputs in set 1; custom() dispatches the
// callbacks registered with register_custom_pass().
//
// Shaders are found by name: <project render dir>/<name>.slang (then .glsl)
// first, then the same in <engine render dir>; both are include roots.
// Loading the same name twice returns the same entity.

using CustomPassFn = void (*)(const RenderPassContext& ctx, void* user_data);

// Where the renderer's messages go (shader logs, graph diagnostics,
// material mistakes). The default prints to stderr; the editor routes them
// to its Output panel.
enum RenderLogLevel : u32 {
    RENDER_LOG_INFO = 0,
    RENDER_LOG_WARNING = 1,
    RENDER_LOG_ERROR = 2,
};

using RenderLogFn = void (*)(RenderLogLevel level, const char* text, void* user_data);

struct RenderLogSink {
    RenderLogFn fn = nullptr;
    void* user_data = nullptr;
};

struct World;

static constexpr u32 RENDERER_PATH_MAX = 512;

struct Renderer : RenderPassExecutor {
    bool init(GpuDevice* gpu, GpuResourceManager* resources, World* world, AssetResourceProvider* provider, const char* engine_render_dir);
    // Destroys everything. The GPU must be idle and the World's Shader and
    // Material entities already removed (World::free fires their hooks).
    void shutdown();

    // The project's render/ directory, searched before the engine's.
    // nullptr clears it.
    void set_project_render_dir(const char* path);

    // Records the frame. The GpuResourceManager's begin_frame(frame.slot)
    // must already have been called.
    void render(const FrameContext& frame);

    // --- Shaders -------------------------------------------------------------

    // Loads <name>.slang (or .glsl) into a new entity with a Shader component, or
    // returns the entity already loaded under that name. 0 on failure, with
    // the compiler's log printed.
    EntityId load_shader(const char* name);
    // Recompiles the entity's file. On success the new program replaces the
    // old one in place, its pipelines are released and `generation` bumps;
    // on failure the old program keeps drawing and the log is printed.
    bool reload_shader(EntityId shader);
    // Reloads every Shader entity.
    void reload_all_shaders();

    // --- Materials -----------------------------------------------------------

    // A new entity with a Material for `shader`, every value zero and every
    // texture white. 0 if `shader` is not a Shader entity.
    EntityId create_material(EntityId shader);
    // Set a member of the material block by name. False (with a message) if
    // the member does not exist or has another type.
    bool set_float(EntityId material, const char* name, f32 value);
    bool set_int(EntityId material, const char* name, i32 value);
    bool set_vec2(EntityId material, const char* name, Vector2 value);
    bool set_vec3(EntityId material, const char* name, Vector3 value);
    bool set_vec4(EntityId material, const char* name, Vector4 value);
    bool set_mat4(EntityId material, const char* name, const Matrix4x4& value);
    // Binds a texture asset to a sampler binding of set 2 by name.
    bool set_texture(EntityId material, const char* name, const AssetGuid& texture, const SamplerDesc& sampler = SamplerDesc{});

    // --- Graph ---------------------------------------------------------------

    // Registers `fn` as the callback CUSTOM passes with this name run.
    // Replaces an existing registration of the same name.
    bool register_custom_pass(const char* name, CustomPassFn fn, void* user_data);
    bool unregister_custom_pass(const char* name);
    // Resets the graph to the built-in forward pass.
    void build_default_graph();

    // The pipeline drawing `shader`'s pass `tag` into render passes with
    // `compat_key`, with `mesh`'s vertex layout (nullptr for no vertex
    // input). Built on first request and cached; nullptr when the shader has
    // no such pass, the mesh lacks an attribute, or creation failed.
    GraphicsPipeline* pipeline_for(EntityId shader, u64 tag, u64 compat_key, const GpuMeshLayout* mesh);

    // Formats and sends a message to `log_sink` (stderr when unset).
    void log(RenderLogLevel level, const char* format, ...) const;

    // --- RenderPassExecutor --------------------------------------------------

    void draw_scene(const RenderPassContext& ctx) override;
    void fullscreen(const RenderPassContext& ctx) override;
    void custom(const RenderPassContext& ctx) override;

    // --- Hooks (RENDER_COMPONENTS::register_all) -----------------------------

    void on_shader_removed(EntityId entity, Shader& shader);
    void on_material_removed(EntityId entity, Material& material);

    // --- Members -------------------------------------------------------------

    GpuDevice* gpu = nullptr;
    GpuResourceManager* resources = nullptr;
    World* world = nullptr;
    AssetResourceProvider* provider = nullptr;

    RenderGraph graph;
    RenderGraphPlan plan;
    RenderGraphPlan next_plan;
    RenderGraphBackend backend;
    DynamicArray<RenderDiagnostic> diagnostics;
    RenderBackbufferInfo last_backbuffer;
    bool plan_valid = false;
    RenderPassHandle default_forward;
    f32 clear_color[4] = {0.05f, 0.05f, 0.08f, 1.0f};

    GpuAssetCache assets;
    DescriptorLayoutCache layouts;
    SamplerCache samplers;
    // Long-lived material sets, freed one by one.
    DescriptorAllocator material_sets;
    // Per-slot sets (frame, pass inputs), reset when the slot comes around.
    DescriptorAllocator frame_sets[FRAMES_IN_FLIGHT];
    VkDescriptorSetLayout frame_layout = VK_NULL_HANDLE;
    ShaderReflection frame_interface;
    GpuBuffer frame_uniforms;
    u32 frame_stride = 0;
    VkDescriptorSet frame_set = VK_NULL_HANDLE;
    GpuTexture white;

    // Written by the Engine before render().
    f32 time = 0.0f;
    f32 delta_time = 0.0f;
    RenderLogSink log_sink;

    char engine_render_dir[RENDERER_PATH_MAX] = {};
    char project_render_dir[RENDERER_PATH_MAX] = {};

private:
    struct PipelineKey {
        EntityId shader = 0;
        u64 tag = 0;
        u64 compat_key = 0;
        u64 vertex_layout = 0;

        bool operator==(const PipelineKey& other) const {
            return this->shader == other.shader && this->tag == other.tag && this->compat_key == other.compat_key && this->vertex_layout == other.vertex_layout;
        }
    };
    struct PipelineKeyHash {
        usz operator()(const PipelineKey& key) const;
    };
    struct CustomPass {
        CustomPassFn fn = nullptr;
        void* user_data = nullptr;
    };

    bool recompile(const RenderBackbufferInfo& info);
    void report_diagnostics();
    void render_fallback(const FrameContext& frame);
    void prune_pipelines();
    void release_shader_pipelines(EntityId shader);
    bool resolve_shader_path(const char* name, char* out, usz out_size) const;
    bool load_program(const char* path, ShaderProgram& out);

    void begin_frame(u32 slot, VkExtent2D extent);
    void write_frame_uniforms(u32 slot, VkExtent2D extent);
    bool rebuild_material(Material& material, const Shader& shader);
    void flush_material(Material& material, u32 slot);
    void retire_material_sets(Material& material);
    bool set_param(EntityId material, const char* name, const void* data, u32 size, ReflectedScalar scalar, u8 columns, u8 rows);
    // Binds set 1 for a pass if the shader declares it; VK_NULL_HANDLE otherwise.
    VkDescriptorSet pass_input_set(const RenderPassContext& ctx, const ShaderPassProgram& pass);

    HashMap<PipelineKey, GraphicsPipeline, PipelineKeyHash> pipelines;
    HashMap<u64, CustomPass> custom_passes;
    DynamicArray<DescriptorSetHandle> retired_sets[FRAMES_IN_FLIGHT];
    u32 current_slot = 0;
    u32 reported_version = 0xffffffffu;
};
