#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/device.hpp"
#include "engine/gpu/descriptor.hpp"
#include "engine/gpu/render_target.hpp"
#include "engine/gpu/resource_manager.hpp"
#include "engine/render_graph/render_graph.hpp"
#include "engine/render_graph/render_graph_plan.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "engine/templates/hash_map.hpp"

#include <volk.h>

// Turns a RenderGraphPlan into Vulkan objects and records it every frame.
//
//     RenderGraphBackend backend;
//     backend.init(&gpu, &resources);
//     ...
//     // whenever the plan changes (graph edit, backbuffer resize):
//     backend.realize(plan);
//     // every frame, inside Engine::render:
//     backend.execute(frame, plan, graph, executor);
//
// realize() owns one image per used transient resource and one render pass
// plus framebuffer per raster pass, reusing whatever an earlier plan already
// had with the same description and releasing the rest through the
// GpuResourceManager's deferred queue, so it may be called between frames
// without stalling. The pass that writes the backbuffer owns nothing: it
// runs through the FrameContext target's render pass and framebuffer, which
// execute() picks up every frame.
//
// execute() walks the plan in order: puts each pass's inputs (and clear or
// blit targets) in the state the plan asks for with explicit barriers
// against the layouts it tracks per image, begins the render pass with the
// live clear values from the graph, sets a full viewport and scissor, and
// hands the pass to the executor, which draws. The layout tracking is also
// what covers the first frame after an image is created.
//
// Pipelines are built against render_pass_for_key(): any render pass the
// backend owns (or borrows) whose attachments hash to that compatibility
// key. Pipelines outlive the render pass they were built with, since Vulkan
// only needs the one bound at draw time to be compatible.

// Everything a pass executor gets to record with.
struct RenderPassContext {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    u32 slot = 0;
    u64 frame_index = 0;
    // Live graph data: tag, shader, constants, callback name.
    const RenderPassDesc* desc = nullptr;
    const PlannedPass* pass = nullptr;
    // The render pass being recorded (VK_NULL_HANDLE for a non-raster
    // custom pass), its compatibility key and color attachment count, for
    // pipeline creation.
    VkRenderPass render_pass = VK_NULL_HANDLE;
    u64 compat_key = 0;
    u32 color_count = 0;
    VkExtent2D extent = {0, 0};
    // The pass's inputs, in order, plus the sampler to read them with.
    VkImageView input_views[RENDER_GRAPH::MAX_INPUTS] = {};
    u32 input_count = 0;
    VkSampler input_sampler = VK_NULL_HANDLE;
};

// What records the passes. Implemented by the runtime's Renderer. No
// destructor, like every engine interface: destroy it as its concrete type.
struct RenderPassExecutor {
    virtual void draw_scene(const RenderPassContext& ctx) = 0;
    virtual void fullscreen(const RenderPassContext& ctx) = 0;
    virtual void custom(const RenderPassContext& ctx) = 0;
};

// One transient image, index == plan resource index.
struct RenderGraphImage {
    RenderResourceHandle handle;
    GpuTexture texture;
    // What the texture was created for, to tell when it must be rebuilt.
    RenderFormat format = RENDER_FORMAT_UNDEFINED;
    u32 width = 0;
    u32 height = 0;
    u32 usage = 0;
    // Tracked across frames; UNDEFINED right after creation.
    RenderResourceState state = RENDER_STATE_UNDEFINED;
};

// The Vulkan side of one raster pass, index == plan pass index.
struct RenderPassInstance {
    RenderPassHandle handle;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkExtent2D extent = {0, 0};
    u32 attachment_count = 0;
    u64 compat_key = 0;
    // Hash of the attachment descriptions the render pass was built from.
    u64 description_hash = 0;
    // The backbuffer pass: handles come from the FrameContext, nothing owned.
    bool borrowed = false;
};

struct RenderGraphBackend {
    // `samplers` provides the sampler pass inputs are read with; it must
    // outlive the backend.
    bool init(GpuDevice* gpu, GpuResourceManager* resources, SamplerCache* samplers);
    // Destroys everything immediately. The GPU must be idle.
    void shutdown();

    // Creates or updates the images, render passes and framebuffers `plan`
    // needs. False if a Vulkan object could not be created; the backend is
    // then not safe to execute and the caller keeps its previous plan.
    bool realize(const RenderGraphPlan& plan);
    // Records `plan` into frame.cmd. `graph` supplies the live per-pass
    // values; `plan` must be the one last realized.
    void execute(const FrameContext& frame, const RenderGraphPlan& plan, const RenderGraph& graph, RenderPassExecutor& executor);

    // A render pass compatible with `key`, or VK_NULL_HANDLE if no realized
    // pass has it (the backbuffer pass's is known after the first execute).
    VkRenderPass render_pass_for_key(u64 key) const;
    // Color attachments of the passes with `key`, 0 if unknown.
    u32 color_count_for_key(u64 key) const;

    GpuDevice* gpu = nullptr;
    GpuResourceManager* resources = nullptr;
    DynamicArray<RenderGraphImage> images;
    DynamicArray<RenderPassInstance> instances;
    // Owned by the SamplerCache passed to init.
    VkSampler input_sampler = VK_NULL_HANDLE;

private:
    struct CompatEntry {
        VkRenderPass render_pass = VK_NULL_HANDLE;
        u32 color_count = 0;
    };

    bool create_image(const PlannedResource& resource, RenderGraphImage& out);
    VkRenderPass create_render_pass(const PlannedPass& pass);
    VkFramebuffer create_framebuffer(const PlannedPass& pass, VkRenderPass render_pass);
    // Moves `image` to `state` with a barrier, if it is not there already.
    void transition(VkCommandBuffer cmd, RenderGraphImage& image, RenderResourceState state);
    void release_image(RenderGraphImage& image);
    void release_instance(RenderPassInstance& instance);
    void record_clear(VkCommandBuffer cmd, const PlannedPass& pass, const RenderPassDesc& desc);
    void record_blit(VkCommandBuffer cmd, const PlannedPass& pass, const RenderPassDesc& desc);

    HashMap<u64, CompatEntry> keys;
};
