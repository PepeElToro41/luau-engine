#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/device.hpp"
#include "engine/gpu/swapchain.hpp"

#include <volk.h>

// How many frames the CPU may record ahead of the GPU. Anything written per
// frame (uniform buffers, descriptor sets, offscreen targets) needs one copy
// per slot, indexed by FrameContext::slot.
static constexpr u32 FRAMES_IN_FLIGHT = 2;

// Where a frame is drawn. Plain handles, copied by value: the provider that
// created them (SwapchainTargets, OffscreenTarget) owns them. Every target
// has exactly one color attachment, so render passes from different providers
// with the same format and sample count are compatible and the same pipelines
// draw into any of them.
struct RenderTarget {
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {0, 0};
};

// Everything the engine needs to record one frame.
struct FrameContext {
    // Primary command buffer in the recording state. Submitted by whoever
    // handed out the context; do not end it.
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    RenderTarget target;
    // Frame-in-flight slot, 0 <= slot < FRAMES_IN_FLIGHT.
    u32 slot = 0;
    // Frames begun since the presenter was created.
    u64 frame_index = 0;
};

// Creates a single-subpass render pass with one color attachment that is
// cleared on load and left in `final_layout`. The subpass dependencies are
// chosen from the final layout: present targets synchronise against the
// acquire semaphore, sampled targets against the fragment shader that reads
// them next.
VkRenderPass create_color_render_pass(const GpuDevice* gpu, VkFormat format, VkImageLayout final_layout);

// One RenderTarget per swapchain image, all sharing a present-layout render
// pass. Rebuilt whenever the swapchain is.
struct SwapchainTargets {
    bool init(GpuDevice* gpu, const Swapchain* swapchain);
    void shutdown();
    // Drops the framebuffers and builds them again for the swapchain's current
    // images and extent. The render pass is kept unless the format changed.
    bool recreate();

    RenderTarget target(u32 image_index) const;

    GpuDevice* gpu = nullptr;
    const Swapchain* swapchain = nullptr;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkFramebuffer framebuffers[MAX_SWAPCHAIN_IMAGES] = {};

private:
    void destroy_framebuffers();
};

// A device-local color image the engine can render into and a shader can
// sample afterwards (the editor's viewport). Owns the image, its memory, the
// views, a sampler, the render pass and the framebuffer.
//
// `format` is what the render pass writes through. `sampled_format`, if it
// differs, is a second view of the same pixels used when sampling: render in
// _SRGB (linear-light scene output gets encoded on write) and sample through
// _UNORM to read those encoded bytes back unchanged, which is what a UI that
// composites into a _UNORM swapchain needs. The two must be in the same
// format compatibility class (same bit layout).
struct OffscreenTarget {
    bool init(GpuDevice* gpu, VkFormat format, VkExtent2D extent, VkFormat sampled_format = VK_FORMAT_UNDEFINED);
    void shutdown();
    // Replaces the image with one of `extent`. The caller guarantees the GPU
    // is done with the old one.
    bool resize(VkExtent2D extent);

    RenderTarget target() const;

    GpuDevice* gpu = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkFormat sampled_format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {0, 0};
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    // Attachment view in `format`.
    VkImageView view = VK_NULL_HANDLE;
    // View to sample through, in `sampled_format`. Same handle as `view` when
    // the formats match.
    VkImageView sampled_view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;

private:
    bool create_image(VkExtent2D extent);
    void destroy_image();
};
