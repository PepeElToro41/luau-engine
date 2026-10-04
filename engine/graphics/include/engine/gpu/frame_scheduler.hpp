#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/device.hpp"
#include "engine/gpu/render_target.hpp"

#include <volk.h>

// Per-slot CPU/GPU pacing: FRAMES_IN_FLIGHT sets of {fence, image-available
// semaphore, command pool, primary command buffer}. begin() blocks until the
// slot's previous submission finished and hands back a command buffer in the
// recording state; submit() ends and submits it, fencing the slot.
//
// It knows nothing about swapchains: the semaphores to wait on and signal are
// passed in by the presenter.
struct FrameScheduler {
    struct Slot {
        VkFence in_flight = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
        VkCommandPool command_pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
    };

    bool init(GpuDevice* gpu);
    void shutdown();

    // Waits for the current slot's fence, then resets and begins its command
    // buffer. Returns the slot. The fence is left signalled so a frame that
    // is abandoned after begin() (swapchain out of date) does not deadlock the
    // next begin().
    const Slot& begin();
    // Ends the current slot's command buffer and submits it on the graphics
    // queue: waits `wait` at color-attachment output, signals `signal`, and
    // fences the slot. Advances to the next slot on success.
    bool submit(VkSemaphore wait, VkSemaphore signal);

    GpuDevice* gpu = nullptr;
    Slot slots[FRAMES_IN_FLIGHT] = {};
    u32 current = 0;
};
