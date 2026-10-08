#pragma once

#include "engine/defines.hpp"
#include "engine/render_graph/render_graph.hpp"
#include "engine/templates/dynamic_array.hpp"

// What a RenderGraph compiles to: every resource with its resolved format,
// size and usage, and every enabled pass in order with its attachments'
// load/store ops and image states worked out, the states each pass needs its
// inputs in, and a render-pass compatibility key per raster pass. The
// graphics backend creates Vulkan objects straight from this; nothing here
// is Vulkan, so the compiler is unit-tested without a GPU.
//
//     RenderGraphPlan plan;
//     plan.init(allocator);
//     DynamicArray<RenderDiagnostic> diagnostics;
//     RenderBackbufferInfo backbuffer = {color_format, depth_format, width, height};
//     if (RENDER_GRAPH::compile(graph, backbuffer, plan, &diagnostics)) { ... }
//
// Image states. Every image is in exactly one state at any point of the
// frame; a raster pass's render pass moves its attachments from `initial`
// to `final` by itself (the backend turns those into initialLayout /
// finalLayout plus subpass dependencies), and the states a pass needs
// before it runs that no render pass provides (sampled inputs, clear and
// blit targets) are listed in `before` for the backend to transition
// explicitly. `final` is chosen from the resource's next use in the frame:
// an attachment sampled by a later pass ends in SAMPLED, one attached again
// stays an attachment.
//
// Frames in flight share one image per transient resource: everything
// records into one command buffer on one queue, so the render pass
// dependencies order frame N+1's writes after frame N's reads. The frame
// therefore wraps: a resource's `frame_start_state` is its
// `frame_end_state`, and a LOAD on first use loads what the previous frame
// left. The backend tracks the real layout as well, so the first frame after
// an image is created (UNDEFINED) is handled there, not here.

enum RenderResourceState : u32 {
    RENDER_STATE_UNDEFINED = 0,
    RENDER_STATE_COLOR_ATTACHMENT = 1,
    RENDER_STATE_DEPTH_ATTACHMENT = 2,
    RENDER_STATE_SAMPLED = 3,
    RENDER_STATE_TRANSFER_SRC = 4,
    RENDER_STATE_TRANSFER_DST = 5,
    // Reserved for compute.
    RENDER_STATE_STORAGE = 6,
    // The backbuffer after its pass: the app's render pass decides.
    RENDER_STATE_EXTERNAL = 7,
};

enum RenderUsageFlags : u32 {
    RENDER_USAGE_COLOR = 1u << 0,
    RENDER_USAGE_DEPTH = 1u << 1,
    RENDER_USAGE_SAMPLED = 1u << 2,
    RENDER_USAGE_TRANSFER_SRC = 1u << 3,
    RENDER_USAGE_TRANSFER_DST = 1u << 4,
    RENDER_USAGE_STORAGE = 1u << 5,
};

// The FrameContext target as the compiler needs to know it.
struct RenderBackbufferInfo {
    RenderFormat color_format = RENDER_FORMAT_UNDEFINED;
    RenderFormat depth_format = RENDER_FORMAT_UNDEFINED;
    u32 width = 0;
    u32 height = 0;

    bool operator==(const RenderBackbufferInfo& other) const {
        return this->color_format == other.color_format && this->depth_format == other.depth_format &&
               this->width == other.width && this->height == other.height;
    }
    bool operator!=(const RenderBackbufferInfo& other) const { return !(*this == other); }
};

struct PlannedResource {
    RenderResourceHandle handle;
    RenderResourceKind kind = RENDER_RESOURCE_TRANSIENT;
    RenderResourceAspect aspect = RENDER_ASPECT_COLOR;
    RenderFormat format = RENDER_FORMAT_UNDEFINED;
    u32 width = 0;
    u32 height = 0;
    // RenderUsageFlags over every use in the frame; 0 if unused.
    u32 usage = 0;
    // Referenced by at least one enabled pass. Unused transients get no image.
    bool used = false;
    RenderResourceState frame_start_state = RENDER_STATE_UNDEFINED;
    RenderResourceState frame_end_state = RENDER_STATE_UNDEFINED;
};

struct PlannedAttachment {
    // Index into RenderGraphPlan::resources.
    u32 resource = 0;
    RenderFormat format = RENDER_FORMAT_UNDEFINED;
    RenderLoadOp load = RENDER_LOAD_CLEAR;
    RenderStoreOp store = RENDER_STORE_STORE;
    // The state the image must be in when the pass begins: `previous` for
    // LOAD, UNDEFINED otherwise (the contents are discarded anyway).
    RenderResourceState initial = RENDER_STATE_UNDEFINED;
    // The state the render pass leaves the image in.
    RenderResourceState final = RENDER_STATE_UNDEFINED;
    // The state before the pass, whatever the load op: what the pass's
    // incoming dependency has to wait for.
    RenderResourceState previous = RENDER_STATE_UNDEFINED;
};

// A state the backend puts a resource in before the pass runs.
struct PlannedRequirement {
    u32 resource = 0;
    RenderResourceState state = RENDER_STATE_UNDEFINED;
};

namespace RENDER_GRAPH {

constexpr u32 MAX_REQUIREMENTS = MAX_INPUTS + 2;
constexpr u32 NO_RESOURCE = 0xffffffffu;

} // namespace RENDER_GRAPH

struct PlannedPass {
    RenderPassHandle handle;
    RenderPassKind kind = RENDER_PASS_DRAW_SCENE;
    // Runs inside a render pass of its own (or the target's).
    bool is_raster = false;
    // The one pass that draws the FrameContext target; it borrows the
    // target's render pass instead of getting one.
    bool writes_backbuffer = false;
    // Framebuffer extent; every attachment has it.
    u32 width = 0;
    u32 height = 0;

    PlannedAttachment color[RENDER_GRAPH::MAX_COLOR_ATTACHMENTS] = {};
    u32 color_count = 0;
    PlannedAttachment depth;
    bool has_depth = false;

    // Plan resource indices, in the pass's input order.
    u32 inputs[RENDER_GRAPH::MAX_INPUTS] = {};
    u32 input_count = 0;

    PlannedRequirement before[RENDER_GRAPH::MAX_REQUIREMENTS] = {};
    u32 before_count = 0;

    // Render pass compatibility: pipelines built against any render pass
    // with this key draw in this pass. Vulkan compatibility covers the
    // attachment formats and counts and the subpass dependencies, so the
    // key hashes the formats plus every attachment's `previous` and `final`
    // state (what the backend builds the dependencies from); the backbuffer
    // pass, which runs through the app's render pass, has its own key. 0
    // for non-raster passes.
    u64 compat_key = 0;

    // CLEAR: the target. BLIT: source and destination. NO_RESOURCE otherwise.
    u32 clear_target = RENDER_GRAPH::NO_RESOURCE;
    u32 blit_src = RENDER_GRAPH::NO_RESOURCE;
    u32 blit_dst = RENDER_GRAPH::NO_RESOURCE;
};

enum RenderDiagnosticSeverity : u32 {
    RENDER_DIAG_WARNING = 0,
    RENDER_DIAG_ERROR = 1,
};

// One compile message. `message` is a string literal; `pass` and `resource`
// are whichever the message is about (either may be invalid).
struct RenderDiagnostic {
    RenderDiagnosticSeverity severity = RENDER_DIAG_ERROR;
    RenderPassHandle pass;
    RenderResourceHandle resource;
    const char* message = "";
};

struct RenderGraphPlan {
    void init(BaseAllocator* allocator = nullptr);
    void free();
    // Empties the plan, keeping its storage.
    void clear();

    // Index of `handle` in `resources`, or RENDER_GRAPH::NO_RESOURCE.
    u32 find_resource(RenderResourceHandle handle) const;
    // Index of `handle` in `passes` (enabled passes only), or NO_RESOURCE.
    u32 find_pass(RenderPassHandle handle) const;

    // Every alive resource of the graph, used or not.
    DynamicArray<PlannedResource> resources;
    // Enabled passes in execution order.
    DynamicArray<PlannedPass> passes;
    // Every distinct compat_key of the raster passes.
    DynamicArray<u64> compat_keys;
    RenderBackbufferInfo backbuffer;
    // Index in `passes` of the pass that writes the backbuffer.
    u32 backbuffer_pass = RENDER_GRAPH::NO_RESOURCE;
    // RenderGraph::version this plan was compiled from.
    u32 graph_version = 0;
};

namespace RENDER_GRAPH {

// Compiles `graph` for a frame drawn into `backbuffer`. True on success
// (warnings may still have been appended to `diagnostics`); false leaves
// `out` cleared with at least one error appended. `diagnostics` may be null.
bool compile(const RenderGraph& graph, const RenderBackbufferInfo& backbuffer, RenderGraphPlan& out, DynamicArray<RenderDiagnostic>* diagnostics);

// The attachment part of a compatibility key: a single-sample subpass with
// these attachments; `depth_format` is RENDER_FORMAT_UNDEFINED for no
// depth attachment.
u64 compat_key(u32 color_count, const RenderFormat* color_formats, RenderFormat depth_format);
// The key of the pass that draws `backbuffer` through the app's render
// pass (one color, one depth).
u64 target_compat_key(const RenderBackbufferInfo& backbuffer);
// The key of a raster pass the backend builds a render pass for: its
// attachment key folded with every attachment's `previous` and `final`.
u64 pass_compat_key(const PlannedPass& pass);

const char* state_name(RenderResourceState state);
const char* pass_kind_name(RenderPassKind kind);

} // namespace RENDER_GRAPH
