#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/device.hpp"
#include "engine/gpu/frame_scheduler.hpp"
#include "engine/gpu/render_target.hpp"
#include "engine/gpu/swapchain.hpp"

#include <SDL3/SDL.h>

// Bundles everything between "a window exists" and "here is a frame to record
// into": the swapchain, its render targets and the frame scheduler. Both the
// standalone and editor apps use it unchanged; they differ only in what they
// record between begin() and end().
//
//     FrameContext frame;
//     if (!presenter.begin(frame)) continue;   // minimized or just resized
//     ...record into frame.cmd, frame.target is this swapchain image...
//     presenter.end();
struct WindowPresenter {
    // `swapchain_format` is the preferred swapchain format; see Swapchain::init
    // for when to pick _SRGB versus _UNORM.
    bool init(GpuDevice* gpu, SDL_Window* window, VkFormat swapchain_format = VK_FORMAT_B8G8R8A8_SRGB);
    void shutdown();

    // Call on window resize events; the swapchain is rebuilt at the next
    // begin(). Acquire/present errors trigger the same path on their own.
    void mark_resized();

    // Rebuilds the swapchain if needed, then waits the slot fence, acquires an
    // image and starts recording. Returns false when no frame can be drawn
    // (minimized, or the swapchain was just rebuilt); skip the frame.
    bool begin(FrameContext& frame);
    // Submits the slot's command buffer and presents the acquired image.
    void end();

    GpuDevice* gpu = nullptr;
    SDL_Window* window = nullptr;
    Swapchain swapchain;
    SwapchainTargets targets;
    FrameScheduler scheduler;

    u64 frame_index = 0;
    // Index of the swapchain image acquired by the current begin().
    u32 image_index = 0;
    bool resize_pending = false;

private:
    bool recreate_swapchain();
};
