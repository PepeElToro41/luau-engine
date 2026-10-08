#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/device.hpp"
#include "engine/gpu/render_target.hpp"
#include "engine/gpu/resource_manager.hpp"
#include "engine/gpu/shaders/compilation.hpp"

#include <volk.h>

// A graphics pipeline and its layout, built from a GraphicsPipelineDesc
// against a RenderTarget's render pass.
//
//     GraphicsPipelineDesc desc;
//     desc.vertex = &vs;                                  // CompiledShaders
//     desc.fragment = &fs;
//     desc.add_binding(sizeof(Vertex));
//     desc.add_attribute(0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position));
//     desc.add_push_constants(VK_SHADER_STAGE_VERTEX_BIT, sizeof(Matrix4x4));
//
//     GraphicsPipeline pipeline;
//     pipeline.init(&gpu, frame.target, desc);            // once
//     ...
//     pipeline.bind(cmd);                                 // inside the render pass
//     pipeline.push_constants(cmd, VK_SHADER_STAGE_VERTEX_BIT, &mvp, sizeof(mvp));
//     vkCmdDraw(cmd, ...);
//
// A pipeline is built against a render pass and may be used with any
// compatible one: same attachment count, formats and sample counts (load
// and store ops do not matter). Every RenderTarget has one color and one
// depth attachment (render_target.hpp), so the standalone swapchain and the
// editor's offscreen viewport share pipelines as long as they share formats;
// the render graph's own passes (N color attachments, optional depth) take
// the init(gpu, VkRenderPass, desc) overload with `color_attachment_count`
// set to match the subpass.
//
// Viewport and scissor are dynamic state; the Engine sets them per frame.
// Shader modules are created from the CompiledShaders inside init() and
// destroyed before it returns, so the CompiledShaders may be freed after.
//
// shutdown() destroys immediately and needs an idle GPU. To replace a
// pipeline while frames that bound it are in flight (a shader reload), build
// the new one, swap, and release() the old one: it is queued on the
// GpuResourceManager and freed once its frame slot has completed. Never
// destroy a pipeline that the command buffer being recorded has bound.
//
// Facing: the projection matrices in math/matrix4x4.hpp negate clip-space Y
// for Vulkan, which together with the default COUNTER_CLOCKWISE front face
// means counter-clockwise winding in model space is front-facing, as in
// OpenGL.

// Vulkan guarantees at least 16 vertex bindings and attributes; mesh assets
// use up to 16 attributes (MESH_ASSET::MAX_ATTRIBUTES) and shader inputs
// go up to location 9 (VERTEX_LAYOUT), so 16 covers both.
static constexpr u32 PIPELINE_MAX_VERTEX_BINDINGS = 16;
static constexpr u32 PIPELINE_MAX_VERTEX_ATTRIBUTES = 16;
static constexpr u32 PIPELINE_MAX_PUSH_CONSTANT_RANGES = 2;
static constexpr u32 PIPELINE_MAX_DESCRIPTOR_SET_LAYOUTS = 4;
// Vulkan guarantees at least 4 color attachments; 8 is the usual limit.
static constexpr u32 PIPELINE_MAX_COLOR_ATTACHMENTS = 8;

enum PipelineBlend : u32 {
    // Writes replace the destination.
    PIPELINE_BLEND_OPAQUE = 0,
    // Standard alpha: src * a + dst * (1 - a).
    PIPELINE_BLEND_ALPHA = 1,
    // src + dst.
    PIPELINE_BLEND_ADDITIVE = 2,
};

struct GraphicsPipelineDesc {
    // Required.
    const CompiledShader* vertex = nullptr;
    // Optional: a depth-only pipeline has none.
    const CompiledShader* fragment = nullptr;

    // Vertex input. Binding i is what vkCmdBindVertexBuffers slot i feeds.
    VkVertexInputBindingDescription bindings[PIPELINE_MAX_VERTEX_BINDINGS] = {};
    u32 binding_count = 0;
    VkVertexInputAttributeDescription attributes[PIPELINE_MAX_VERTEX_ATTRIBUTES] = {};
    u32 attribute_count = 0;

    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPolygonMode polygon_mode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags cull_mode = VK_CULL_MODE_BACK_BIT;
    VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    bool depth_test = true;
    bool depth_write = true;
    // LESS pairs with the depth clear of 1.0 in render_target_clear_values.
    VkCompareOp depth_compare = VK_COMPARE_OP_LESS;

    // Applied to every color attachment.
    PipelineBlend blend = PIPELINE_BLEND_OPAQUE;
    // Color attachments of the subpass the pipeline draws in; the blend
    // state needs one entry per attachment. 0 for a depth-only pass.
    u32 color_attachment_count = 1;

    // Layout.
    VkPushConstantRange push_constants[PIPELINE_MAX_PUSH_CONSTANT_RANGES] = {};
    u32 push_constant_count = 0;
    VkDescriptorSetLayout set_layouts[PIPELINE_MAX_DESCRIPTOR_SET_LAYOUTS] = {};
    u32 set_layout_count = 0;

    // Appends a vertex binding of `stride` bytes and returns its index, or
    // UINT32_MAX (with an error) when full.
    u32 add_binding(u32 stride, VkVertexInputRate rate = VK_VERTEX_INPUT_RATE_VERTEX);
    // Appends an attribute at `location` reading `format` at `offset` bytes
    // into vertices of `binding`. False when full.
    bool add_attribute(u32 location, VkFormat format, u32 offset, u32 binding = 0);
    // Appends a push constant range. Ranges for different stages may not
    // overlap; one range covering several stages is the usual choice.
    bool add_push_constants(VkShaderStageFlags stages, u32 size, u32 offset = 0);
    bool add_set_layout(VkDescriptorSetLayout layout);
};

struct GraphicsPipeline {
    // Builds the layout and pipeline for `target`'s render pass. On failure
    // returns false with nothing left to clean up. `target` only needs its
    // render_pass; the handles are not kept.
    bool init(GpuDevice* gpu, const RenderTarget& target, const GraphicsPipelineDesc& desc);
    // The same against any render pass, subpass 0. `desc.color_attachment_count`
    // must match the subpass's color attachment count.
    bool init(GpuDevice* gpu, VkRenderPass render_pass, const GraphicsPipelineDesc& desc);
    // Destroys now. The GPU must be done with every command buffer that
    // bound this pipeline.
    void shutdown();
    // Hands the pipeline and layout to `resources` for deferred destruction
    // and leaves this object empty.
    void release(GpuResourceManager& resources);

    void bind(VkCommandBuffer cmd) const;
    // vkCmdPushConstants on this layout; `stages`, `offset` and `size` must
    // fall inside a range the pipeline was built with.
    void push_constants(VkCommandBuffer cmd, VkShaderStageFlags stages, const void* data, u32 size, u32 offset = 0) const;

    bool is_valid() const { return this->pipeline != VK_NULL_HANDLE; }

    GpuDevice* gpu = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
};
