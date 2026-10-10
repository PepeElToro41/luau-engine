#pragma once

#include "engine/asset/asset_resource_provider.hpp"
#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/gpu/gpu.hpp"
#include "engine/render/components.hpp"
#include "engine/render/gpu_asset_cache.hpp"
#include "engine/render/graph_executor.hpp"
#include "engine/render/materials.hpp"
#include "engine/render/pipelines.hpp"
#include "engine/render/shader_library.hpp"
#include "engine/render_graph/render_graph.hpp"
#include "engine/render_graph/render_graph_plan.hpp"
#include "engine/shaders/reflection.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "engine/templates/hash_map.hpp"

// The engine's renderer: the render graph that describes the frame, the
// shaders and materials as ECS entities, and everything shared between
// them (pipeline cache, uploaded assets, the frame's set 0). It is a plain
// state struct the Engine owns as a singleton; RENDERER:: and the
// namespaces of the headers above act on it:
//
//     Renderer* renderer = engine.get_singleton<Renderer>();
//     EntityId unlit = SHADER_LIBRARY::load(*renderer, "unlit");   // render/unlit.slang
//     EntityId red = MATERIAL::create(*renderer, unlit);
//     MATERIAL::set_vec4(*renderer, red, "color", Vector4(1, 0, 0, 1));
//     EntityId rock = MATERIAL::load(*renderer, rock_guid);         // a registered .material asset
//     world->set(cube, PrimitiveRenderer{PRIMITIVE_CUBE, red});     // or MeshRenderer{guid, rock}
//
//     RenderGraph& graph = renderer->graph;                          // takes effect next frame
//     RenderPassHandle post = graph.add_pass("post", RENDER_PASS_FULLSCREEN);
//
// The frame. render() recompiles the graph when it is dirty or the
// target's format or size changed, keeps the last good plan when a compile
// fails (reporting the diagnostics once per graph version), and clears the
// target when there is no good plan at all. It writes the frame uniforms
// from the first Camera entity into the uniform ring, builds the frame's
// set-0 group (the block plus the engine sampler table), then runs the
// plan through the graph executor: DRAW_SCENE passes collect every entity
// with a Transform and a MeshRenderer or PrimitiveRenderer whose material's
// shader has a pass for the tag into a sorted draw list, FULLSCREEN passes draw a pass's
// shader entity over the target with the inputs in set 1, CUSTOM passes
// call the callbacks registered with register_custom_pass().
//
// Materials have no GPU state: the first draw of a material in a frame
// pushes its block through the ring and makes a transient bind group that
// every later draw of it that frame reuses.

static constexpr u32 RENDERER_PATH_MAX = 512;

// The engine sampler table of set 0 (engine/frame.slang), bindings 1..4.
enum RendererSampler : u32 {
    RENDERER_SAMPLER_LINEAR = 0,
    RENDERER_SAMPLER_LINEAR_CLAMP = 1,
    RENDERER_SAMPLER_NEAREST = 2,
    RENDERER_SAMPLER_NEAREST_CLAMP = 3,
    RENDERER_SAMPLER_COUNT = 4,
};

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

// What the app hands the engine for one frame: the GPU frame to record
// into and the texture the graph's backbuffer resource stands for.
// `target_state` is the state the target is in on entry (UNDEFINED for a
// swapchain image, SAMPLED for an editor viewport shown last frame) and is
// updated to the state it is left in.
struct FrameContext {
    GpuFrame frame;
    GpuTexture target;
    GpuResourceState target_state = GPU_STATE_UNDEFINED;
};

// What a pass function (and a custom pass callback) gets.
struct RenderPassContext {
    GpuCommandList cmd;
    u32 slot = 0;
    u64 frame_index = 0;
    // The live graph description and the planned pass.
    const RenderPassDesc* desc = nullptr;
    const PlannedPass* pass = nullptr;
    // Raster passes: the attachments' formats (what pipelines are built
    // for) and extent; the pass is open on `cmd`.
    GpuTargetFormats formats;
    u32 width = 0;
    u32 height = 0;
    bool in_render_pass = false;
    // The pass's inputs, in SAMPLED state.
    GpuTexture inputs[RENDER_GRAPH::MAX_INPUTS] = {};
    u32 input_count = 0;
    // The frame's set 0.
    GpuBindGroup frame_group;
};

using CustomPassFn = void (*)(const RenderPassContext& ctx, void* user_data);

struct CustomPass {
    CustomPassFn fn = nullptr;
    void* user_data = nullptr;
};

struct Renderer {
    GpuContext* gpu = nullptr;
    World* world = nullptr;
    AssetResourceProvider* provider = nullptr;

    // The graph and its plans.
    RenderGraph graph;
    RenderGraphPlan plan;
    RenderGraphPlan next_plan;
    DynamicArray<RenderDiagnostic> diagnostics;
    RenderBackbufferInfo last_backbuffer;
    bool plan_valid = false;
    RenderPassHandle default_forward;
    f32 clear_color[4] = {0.05f, 0.05f, 0.08f, 1.0f};
    u32 reported_version = 0xffffffffu;

    GraphExecutor executor;
    PipelineCache pipelines;
    GpuAssetCache assets;
    HashMap<u64, CustomPass> custom_passes;

    // Set 0: the frame block and the sampler table.
    ShaderReflection frame_interface;
    GpuBindLayout frame_layout;
    GpuSampler samplers[RENDERER_SAMPLER_COUNT] = {};
    // What unset texture slots sample.
    GpuTexture white;
    FrameUniforms frame_uniforms;
    // This frame's set-0 group and per-material set-2 groups.
    GpuBindGroup frame_group;
    HashMap<EntityId, GpuBindGroup> material_groups;
    // Material entities loaded from .material assets, by asset GUID
    // (MATERIAL::load returns the same entity for the same asset; the
    // removed hook drops the entry).
    HashMap<AssetGuid, EntityId, AssetGuidHash> materials;

    // Written by the Engine before render().
    f32 time = 0.0f;
    f32 delta_time = 0.0f;
    RenderLogSink log_sink;

    char engine_render_dir[RENDERER_PATH_MAX] = {};
    char project_render_dir[RENDERER_PATH_MAX] = {};
};

namespace RENDERER {

bool init(Renderer& renderer, GpuContext* gpu, World* world, AssetResourceProvider* provider, const char* engine_render_dir);
// Destroys everything. The GPU must be idle and the World's Shader and
// Material entities already removed (World::free fires their hooks).
void shutdown(Renderer& renderer);

// The project's render/ directory, searched before the engine's. nullptr
// clears it.
void set_project_render_dir(Renderer& renderer, const char* path);

// Records the frame into frame.frame.cmd, drawing into frame.target.
// Returns the state the target is left in.
GpuResourceState render(Renderer& renderer, const FrameContext& frame);

// Resets the graph to the built-in forward pass.
void build_default_graph(Renderer& renderer);
// Registers `fn` as the callback CUSTOM passes with this name run.
// Replaces an existing registration of the same name.
bool register_custom_pass(Renderer& renderer, const char* name, CustomPassFn fn, void* user_data);
bool unregister_custom_pass(Renderer& renderer, const char* name);

// Formats and sends a message to the log sink (stderr when unset).
void log(const Renderer& renderer, RenderLogLevel level, const char* format, ...);

} // namespace RENDERER
