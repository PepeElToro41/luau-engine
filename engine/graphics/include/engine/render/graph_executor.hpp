#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/gpu.hpp"
#include "engine/render_graph/render_graph.hpp"
#include "engine/render_graph/render_graph_plan.hpp"
#include "engine/templates/dynamic_array.hpp"

// Runs a RenderGraphPlan against GPU::, once for every backend. realize()
// gives every used transient resource (and the backbuffer depth) a texture,
// reusing the previous one while its description is unchanged; execute()
// walks the passes in order, putting each resource in the state the plan
// asks for with cmd_barrier, opening a render pass per raster pass and
// handing the recording to the renderer's pass functions. The backbuffer
// resource is the FrameContext target, set anew every frame. Every image
// keeps its last state across frames, which is what the next frame's first
// barrier leaves from.

struct Renderer;
struct FrameContext;

struct GraphImage {
    RenderResourceHandle handle;
    GpuTexture texture;
    RenderFormat format = RENDER_FORMAT_UNDEFINED;
    u32 width = 0;
    u32 height = 0;
    u32 usage = 0;
    GpuResourceState state = GPU_STATE_UNDEFINED;
    // The FrameContext target: not owned.
    bool external = false;
};

struct GraphExecutor {
    // Index-aligned with RenderGraphPlan::resources of the realized plan.
    DynamicArray<GraphImage> images;
};

namespace GRAPH_EXECUTOR {

// Creates, reuses or releases textures so `images` matches `plan`.
bool realize(Renderer& renderer, const RenderGraphPlan& plan);
// Records the plan's passes into the frame. Returns the state the target
// is left in.
GpuResourceState execute(Renderer& renderer, const FrameContext& frame, const RenderGraphPlan& plan);
// Destroys every texture now. The GPU must be idle.
void free(Renderer& renderer);

} // namespace GRAPH_EXECUTOR
