#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/gpu.hpp"

#include <volk.h>

// The one place above src/gpu/ that sees Vulkan: raw handles for code that
// has to talk to the API directly, today the editor's imgui_impl_vulkan
// glue. Everything else goes through GPU::. Only valid while the Vulkan
// backend is the selected one (GPU_BACKEND_VULKAN); the functions return
// null handles otherwise.

namespace VK_ACCESS {

VkInstance instance(GpuContext* gpu);
VkPhysicalDevice physical_device(GpuContext* gpu);
VkDevice device(GpuContext* gpu);
VkQueue graphics_queue(GpuContext* gpu);
u32 graphics_queue_family(GpuContext* gpu);

// A frame's command buffer, between begin_frame and end_frame.
VkCommandBuffer command_buffer(GpuCommandList cmd);
// The view to sample the texture through (its sampled_format when it has one).
VkImageView image_view(GpuContext* gpu, GpuTexture texture);
VkSampler sampler(GpuContext* gpu, GpuSampler sampler);
// A render pass compatible with every pass that draws into these formats
// (the one pipelines are built against). For ImGui's pipeline creation.
VkRenderPass render_pass(GpuContext* gpu, const GpuTargetFormats& formats);

u32 swapchain_image_count(GpuContext* gpu);
u32 swapchain_min_image_count(GpuContext* gpu);

} // namespace VK_ACCESS
