#include "engine/gpu/frame_scheduler.hpp"

#include "gpu/vk_check.hpp"

bool FrameScheduler::init(GpuDevice* gpu) {
    this->gpu = gpu;
    this->current = 0;
    VkDevice device = gpu->device;

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = gpu->graphics_queue_family;

    for (Slot& slot : this->slots) {
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
    }
    return true;
}

void FrameScheduler::shutdown() {
    if (this->gpu == nullptr) {
        return;
    }
    VkDevice device = this->gpu->device;
    for (Slot& slot : this->slots) {
        // Freeing the pool frees its command buffers.
        if (slot.command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, slot.command_pool, nullptr);
        }
        if (slot.image_available != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, slot.image_available, nullptr);
        }
        if (slot.in_flight != VK_NULL_HANDLE) {
            vkDestroyFence(device, slot.in_flight, nullptr);
        }
        slot = Slot{};
    }
    this->gpu = nullptr;
}

const FrameScheduler::Slot& FrameScheduler::begin() {
    Slot& slot = this->slots[this->current];
    vkWaitForFences(this->gpu->device, 1, &slot.in_flight, VK_TRUE, UINT64_MAX);

    vkResetCommandBuffer(slot.cmd, 0);
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(slot.cmd, &begin_info);
    return slot;
}

bool FrameScheduler::submit(VkSemaphore wait, VkSemaphore signal) {
    Slot& slot = this->slots[this->current];
    if (!vk_check(vkEndCommandBuffer(slot.cmd), "vkEndCommandBuffer")) {
        return false;
    }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = wait != VK_NULL_HANDLE ? 1 : 0;
    submit_info.pWaitSemaphores = &wait;
    submit_info.pWaitDstStageMask = &wait_stage;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &slot.cmd;
    submit_info.signalSemaphoreCount = signal != VK_NULL_HANDLE ? 1 : 0;
    submit_info.pSignalSemaphores = &signal;

    // Reset only now: between begin() and here the fence stays signalled so an
    // abandoned frame cannot deadlock the next wait.
    vkResetFences(this->gpu->device, 1, &slot.in_flight);
    if (!vk_check(vkQueueSubmit(this->gpu->graphics_queue, 1, &submit_info, slot.in_flight), "vkQueueSubmit")) {
        return false;
    }

    this->current = (this->current + 1) % FRAMES_IN_FLIGHT;
    return true;
}
