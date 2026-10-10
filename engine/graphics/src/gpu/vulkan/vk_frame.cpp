#include "gpu/vulkan/vk_check.hpp"
#include "gpu/vulkan/vk_context.hpp"

// --- Deferred destruction -------------------------------------------------------

void VK_FRAME::destroy(VulkanContext& vk, const VkPending& pending) {
    VkDevice device = vk.device;
    if (pending.framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, pending.framebuffer, nullptr);
    }
    if (pending.sampled_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, pending.sampled_view, nullptr);
    }
    if (pending.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, pending.view, nullptr);
    }
    if (pending.image != VK_NULL_HANDLE) {
        vkDestroyImage(device, pending.image, nullptr);
    }
    if (pending.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, pending.buffer, nullptr);
    }
    // Mapped memory is unmapped implicitly by vkFreeMemory.
    if (pending.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, pending.memory, nullptr);
    }
    if (pending.pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pending.pipeline, nullptr);
    }
    if (pending.pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, pending.pipeline_layout, nullptr);
    }
    if (pending.set != VK_NULL_HANDLE && pending.set_pool != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(device, pending.set_pool, 1, &pending.set);
    }
}

void VK_FRAME::release(VulkanContext& vk, const VkPending& pending) {
    vk.slots[vk.current_slot].releases.push(pending);
}

void VK_FRAME::flush(VulkanContext& vk) {
    for (VkFrameSlot& slot : vk.slots) {
        for (const VkPending& pending : slot.releases) {
            VK_FRAME::destroy(vk, pending);
        }
        slot.releases.clear();
    }
}

// --- Slots ----------------------------------------------------------------------

bool VK_FRAME::init(VulkanContext& vk) {
    VkDevice device = vk.device;

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = vk.graphics_queue_family;

    // Transient descriptor sets: per-frame and per-material-per-frame groups.
    const VkDescriptorPoolSize transient_sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1024},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 128},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2048},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 128},
    };

    for (VkFrameSlot& slot : vk.slots) {
        if (!vk_check(vkCreateFence(device, &fence_info, nullptr, &slot.in_flight), "vkCreateFence")) {
            return false;
        }
        if (!vk_check(vkCreateSemaphore(device, &semaphore_info, nullptr, &slot.image_available), "vkCreateSemaphore (image_available)")) {
            return false;
        }
        if (!vk_check(vkCreateCommandPool(device, &pool_info, nullptr, &slot.command_pool), "vkCreateCommandPool")) {
            return false;
        }
        VkCommandBufferAllocateInfo cmd_info{};
        cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_info.commandPool = slot.command_pool;
        cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandBufferCount = 1;
        if (!vk_check(vkAllocateCommandBuffers(device, &cmd_info, &slot.cmd), "vkAllocateCommandBuffers")) {
            return false;
        }
        slot.command_list = vk.command_lists.new_element();
        vk.command_lists.get_element_alive(slot.command_list)->cmd = slot.cmd;

        if (!VK_BINDING::pools_init(vk, slot.transient_sets, transient_sizes, 5, 512, 0)) {
            return false;
        }

        GpuBufferDesc ring_desc;
        ring_desc.size = VK_UNIFORM_RING_SIZE;
        ring_desc.usage = GPU_BUFFER_USAGE_UNIFORM;
        ring_desc.memory = GPU_MEMORY_HOST_VISIBLE;
        slot.uniform_ring = VULKAN_BACKEND::table()->create_buffer(vk.gpu, ring_desc, nullptr);
        if (!slot.uniform_ring.is_valid()) {
            return false;
        }
        slot.uniform_cursor = 0;
    }
    return true;
}

void VK_FRAME::shutdown(VulkanContext& vk) {
    flush(vk);
    VkDevice device = vk.device;
    for (VkFrameSlot& slot : vk.slots) {
        slot.releases.free();
        if (slot.uniform_ring.is_valid()) {
            VULKAN_BACKEND::table()->destroy_buffer(vk.gpu, slot.uniform_ring);
        }
        VK_BINDING::pools_shutdown(vk, slot.transient_sets);
        slot.transient_groups.free();
        if (slot.command_list != GPU_NULL_ID) {
            vk.command_lists.delete_element(slot.command_list);
        }
        // Freeing the pool frees its command buffer.
        if (slot.command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, slot.command_pool, nullptr);
        }
        if (slot.image_available != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, slot.image_available, nullptr);
        }
        if (slot.in_flight != VK_NULL_HANDLE) {
            vkDestroyFence(device, slot.in_flight, nullptr);
        }
        slot = VkFrameSlot{};
    }
}

// --- Frames ----------------------------------------------------------------------

static void vk_wait_idle(GpuContext* gpu) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    if (vk.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vk.device);
    }
}

static void vk_mark_resized(GpuContext* gpu) {
    VK_CONTEXT::of(gpu).resize_pending = true;
}

static bool vk_begin_frame(GpuContext* gpu, GpuFrame& out) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    out = GpuFrame{};
    if (vk.frame_open) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] begin_frame: the previous frame was not ended");
        return false;
    }
    if (vk.resize_pending) {
        vk.resize_pending = false;
        if (!VK_SWAPCHAIN::recreate(vk)) {
            // Still minimized: try again next frame.
            vk.resize_pending = true;
            return false;
        }
    }

    VkFrameSlot& slot = vk.slots[vk.current_slot];
    vkWaitForFences(vk.device, 1, &slot.in_flight, VK_TRUE, UINT64_MAX);

    // Everything this slot's previous frame could still have used is free now.
    for (const VkPending& pending : slot.releases) {
        VK_FRAME::destroy(vk, pending);
    }
    slot.releases.clear();
    VK_BINDING::begin_slot(vk, slot);

    const VkResult acquired = VK_SWAPCHAIN::acquire(vk, slot.image_available, &vk.image_index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        vk.resize_pending = true;
        return false;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        vk_check(acquired, "vkAcquireNextImageKHR");
        return false;
    }

    vkResetCommandBuffer(slot.cmd, 0);
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!vk_check(vkBeginCommandBuffer(slot.cmd, &begin_info), "vkBeginCommandBuffer")) {
        return false;
    }
    VkCommandListEntry* list = vk.command_lists.get_element_alive(slot.command_list);
    list->bound_layout = VK_NULL_HANDLE;
    list->bound_push_size = 0;

    const SparseId backbuffer_id = vk.swapchain_textures[vk.image_index];
    VkTextureEntry* backbuffer = VK_CONTEXT::texture(vk, backbuffer_id);
    backbuffer->state = GPU_STATE_UNDEFINED;

    out.cmd.id = slot.command_list;
    out.slot = vk.current_slot;
    out.frame_index = vk.frame_index;
    out.backbuffer.id = backbuffer_id;
    out.backbuffer.format = backbuffer->format;
    out.backbuffer.width = backbuffer->width;
    out.backbuffer.height = backbuffer->height;
    out.backbuffer.mip_levels = 1;
    out.backbuffer.usage = backbuffer->usage;
    gpu->slot = vk.current_slot;
    gpu->frame_index = vk.frame_index;
    vk.frame_open = true;
    return true;
}

static bool vk_end_frame(GpuContext* gpu, GpuFrame& frame) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    if (!vk.frame_open) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] end_frame without begin_frame");
        return false;
    }
    vk.frame_open = false;
    VkFrameSlot& slot = vk.slots[vk.current_slot];

    // Hand the backbuffer to the presentation engine.
    VkTextureEntry* backbuffer = VK_CONTEXT::texture(vk, vk.swapchain_textures[vk.image_index]);
    VK_COMMANDS::barrier(slot.cmd, *backbuffer, backbuffer->state, GPU_STATE_PRESENT);

    if (!vk_check(vkEndCommandBuffer(slot.cmd), "vkEndCommandBuffer")) {
        return false;
    }

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &slot.image_available;
    submit_info.pWaitDstStageMask = &wait_stage;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &slot.cmd;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &vk.render_finished[vk.image_index];

    // Reset only now: until here the fence stays signalled, so an abandoned
    // frame cannot deadlock the next wait.
    vkResetFences(vk.device, 1, &slot.in_flight);
    if (!vk_check(vkQueueSubmit(vk.graphics_queue, 1, &submit_info, slot.in_flight), "vkQueueSubmit")) {
        return false;
    }

    const VkResult presented = VK_SWAPCHAIN::present(vk, vk.image_index);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        vk.resize_pending = true;
    } else {
        vk_check(presented, "vkQueuePresentKHR");
    }

    frame = GpuFrame{};
    vk.current_slot = (vk.current_slot + 1) % FRAMES_IN_FLIGHT;
    vk.frame_index += 1;
    return true;
}

namespace VK_FRAME_TABLE {
void fill(GpuBackend& table) {
    table.wait_idle = vk_wait_idle;
    table.mark_resized = vk_mark_resized;
    table.begin_frame = vk_begin_frame;
    table.end_frame = vk_end_frame;
}
} // namespace VK_FRAME_TABLE
