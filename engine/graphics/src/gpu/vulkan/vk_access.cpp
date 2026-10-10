#include "engine/gpu/vulkan/vk_access.hpp"

#include "gpu/vulkan/vk_context.hpp"

static VulkanContext* context_of(GpuContext* gpu) {
    if (gpu == nullptr || gpu->info.backend != GPU_BACKEND_VULKAN || gpu->impl == nullptr) {
        return nullptr;
    }
    return &VK_CONTEXT::of(gpu);
}

VkInstance VK_ACCESS::instance(GpuContext* gpu) {
    VulkanContext* vk = context_of(gpu);
    return vk != nullptr ? vk->instance : VK_NULL_HANDLE;
}

VkPhysicalDevice VK_ACCESS::physical_device(GpuContext* gpu) {
    VulkanContext* vk = context_of(gpu);
    return vk != nullptr ? vk->physical_device : VK_NULL_HANDLE;
}

VkDevice VK_ACCESS::device(GpuContext* gpu) {
    VulkanContext* vk = context_of(gpu);
    return vk != nullptr ? vk->device : VK_NULL_HANDLE;
}

VkQueue VK_ACCESS::graphics_queue(GpuContext* gpu) {
    VulkanContext* vk = context_of(gpu);
    return vk != nullptr ? vk->graphics_queue : VK_NULL_HANDLE;
}

u32 VK_ACCESS::graphics_queue_family(GpuContext* gpu) {
    VulkanContext* vk = context_of(gpu);
    return vk != nullptr ? vk->graphics_queue_family : UINT32_MAX;
}

VkCommandBuffer VK_ACCESS::command_buffer(const GpuCommandList cmd) {
    VkCommandListEntry* list = nullptr;
    VK_CONTEXT::of_command_list(cmd, &list);
    return list != nullptr ? list->cmd : VK_NULL_HANDLE;
}

VkImageView VK_ACCESS::image_view(GpuContext* gpu, const GpuTexture texture) {
    VulkanContext* vk = context_of(gpu);
    const VkTextureEntry* entry = vk != nullptr ? VK_CONTEXT::texture(*vk, texture.id) : nullptr;
    return entry != nullptr ? entry->sampled_view : VK_NULL_HANDLE;
}

VkSampler VK_ACCESS::sampler(GpuContext* gpu, const GpuSampler sampler) {
    VulkanContext* vk = context_of(gpu);
    const VkSamplerEntry* entry = vk != nullptr ? VK_CONTEXT::sampler(*vk, sampler.id) : nullptr;
    return entry != nullptr ? entry->sampler : VK_NULL_HANDLE;
}

VkRenderPass VK_ACCESS::render_pass(GpuContext* gpu, const GpuTargetFormats& formats) {
    VulkanContext* vk = context_of(gpu);
    return vk != nullptr ? VK_PIPELINE::template_render_pass(*vk, formats) : VK_NULL_HANDLE;
}

u32 VK_ACCESS::swapchain_image_count(GpuContext* gpu) {
    VulkanContext* vk = context_of(gpu);
    return vk != nullptr ? vk->swapchain_image_count : 0;
}

u32 VK_ACCESS::swapchain_min_image_count(GpuContext* gpu) {
    VulkanContext* vk = context_of(gpu);
    return vk != nullptr ? vk->swapchain_min_image_count : 0;
}
