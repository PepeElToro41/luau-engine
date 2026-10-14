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

// The engine's renderer, in two halves that belong to different scopes.
//
// RenderResources is everything device-level and shared by every view of
// every world: the pipeline cache (keyed by shader entity and generation,
// which a world cloned id for id keeps valid), the uploaded assets (by
// GUID), the frame's set-0 layout and interface, the engine sampler table,
// the white texture and the two shader directories. One per process, an
// Engine singleton declared SCOPE_SHARED.
//
// Renderer is the state of one view being drawn from one World: the render
// graph that describes the frame and its plans, the executor that owns the
// graph's transient textures, the custom passes, the per-frame bind groups,
// the loaded materials of that world, the camera and the clock. One per
// instance (the authoring world's, and a play instance's while it runs),
// declared SCOPE_LOCAL, each pointing at the shared RenderResources. The
// split is what makes a play instance's Stop clean: a material or a shader
// loaded during play lives in the instance's maps and goes with them, and
// the editor's own passes (grid, selection outline) sit in the authoring
// renderer's graph and never in the play instance's.
//
// Both are plain state structs the Engine owns as singletons; RENDERER::,
// RENDER_RESOURCES:: and the namespaces of the headers above act on them:
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

// The shared half: see above.
struct RenderResources {
    GpuContext* gpu = nullptr;
    AssetResourceProvider* provider = nullptr;

    PipelineCache pipelines;
    GpuAssetCache assets;

    // Set 0: the frame block and the sampler table.
    ShaderReflection frame_interface;
    GpuBindLayout frame_layout;
    GpuSampler samplers[RENDERER_SAMPLER_COUNT] = {};
    // What unset texture slots sample.
    GpuTexture white;

    char engine_render_dir[RENDERER_PATH_MAX] = {};
    char project_render_dir[RENDERER_PATH_MAX] = {};
};

// The per-instance half: see above.
struct Renderer {
    GpuContext* gpu = nullptr;
    World* world = nullptr;
    AssetResourceProvider* provider = nullptr;
    // The shared half this view draws with. Never null after init.
    RenderResources* resources = nullptr;
    // Scratch for the frame being recorded (draw lists and the like): the
    // instance's FrameAllocator, reset by the Engine once per frame, never
    // here. Never null after init.
    BaseAllocator* frame_allocator = nullptr;

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
    HashMap<u64, CustomPass> custom_passes;

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
    // The entity the frame is drawn from when it is alive and holds a
    // Transform and a Camera, whatever the RenderCamera tag says: how the
    // editor keeps its own fly camera while the scene's camera carries the
    // tag for the standalone. 0 (the default) falls back to the tag, then
    // to the first Camera.
    EntityId camera = 0;
    // The first entity id this view's world owns outright: the removed hook
    // of a Shader at or above it frees the program and its pipelines, one
    // below it only borrows them. 0 for the authoring world, which owns
    // everything; a play instance sets it to its clone's fence, since its
    // shaders below the fence are the authoring world's programs shared by
    // id (see RENDER_COMPONENTS::install_hooks).
    EntityIdLow owns_from = 0;
};

namespace RENDER_RESOURCES {

// The device-level half, once per process: set 0, the samplers, the white
// texture and the caches, with `engine_render_dir` as the engine's shader
// root. False (with messages) if a GPU object could not be made.
bool init(RenderResources& resources, GpuContext* gpu, AssetResourceProvider* provider, const char* engine_render_dir);
// Destroys every pipeline, GPU asset and the white texture. The GPU must
// be idle and every Renderer that used the resources shut down first.
void shutdown(RenderResources& resources);

// The project's shader root, searched before the engine's render/
// directory and an include root beside it (the editor passes the project
// root, since a project shader may sit in any folder and is normally found
// through its asset entity's AssetPath anyway). nullptr clears it.
void set_project_render_dir(RenderResources& resources, const char* path);

} // namespace RENDER_RESOURCES

namespace RENDERER {

// A view of `world` drawn with `resources`. `frame_allocator` is where
// per-frame scratch goes; the caller resets it once per frame, before
// render(). Builds the default graph.
bool init(Renderer& renderer, RenderResources* resources, World* world, AssetResourceProvider* provider, BaseAllocator* frame_allocator);
// Releases what the view owns: its graph and plans, the executor's
// transient textures, the groups and maps. The World's Shader and Material
// entities must already be removed (World::free fires their hooks, which
// reach into the renderer). With `gpu_idle` the textures are destroyed at
// once (the engine's shutdown, after its GPU idle); without it they are
// released at the end of the frames in flight, which is what stopping a
// play instance between two frames needs. The shared resources are not
// touched.
void shutdown(Renderer& renderer, bool gpu_idle = true);

// Records the frame into frame.frame.cmd, drawing into frame.target.
// Returns the state the target is left in.
GpuResourceState render(Renderer& renderer, const FrameContext& frame);

// Resets the graph to the built-in forward pass.
void build_default_graph(Renderer& renderer);
// Registers `fn` as the callback CUSTOM passes with this name run.
// Replaces an existing registration of the same name.
bool register_custom_pass(Renderer& renderer, const char* name, CustomPassFn fn, void* user_data);
bool unregister_custom_pass(Renderer& renderer, const char* name);

} // namespace RENDERER
