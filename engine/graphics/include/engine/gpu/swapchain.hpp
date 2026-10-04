#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/device.hpp"

#include <SDL3/SDL.h>
#include <volk.h>

// Upper bound on swapchain images we keep state for. Drivers hand out 2-4;
// the request is capped well below this.
static constexpr u32 MAX_SWAPCHAIN_IMAGES = 8;

// The presentation images for the device's surface. It only acquires and
// presents: no command buffers, no framebuffers (see SwapchainTargets) and no
// frame pacing (see FrameScheduler). Each image carries the semaphore that
// signals when rendering into it is done, because that semaphore's lifetime is
// the image's, not the frame slot's.
struct Swapchain {
    // `preferred_format` is used if the surface supports it with the sRGB
    // non-linear color space, otherwise the first supported format is taken.
    // Pick an _SRGB format when the swapchain receives linear-light scene
    // output (the hardware encodes on write) and a _UNORM one when it receives
    // already-encoded colors such as ImGui's, which must not be encoded twice.
    bool init(GpuDevice* gpu, SDL_Window* window, VkFormat preferred_format);
    void shutdown();

    // Rebuilds the swapchain for the window's current pixel size, reusing the
    // old one as oldSwapchain. The caller must make sure nothing is still
    // using the images (wait the device idle). Returns false if the surface is
    // zero-sized (minimized) or creation failed; the old swapchain is kept.
    bool recreate();

    // Acquires the next image, signalling `image_available` when it can be
    // written. Fills `image_index` and returns the raw result so the caller
    // can react to VK_ERROR_OUT_OF_DATE_KHR / VK_SUBOPTIMAL_KHR.
    VkResult acquire(VkSemaphore image_available, u32* image_index);
    // Presents `image_index` once render_finished[image_index] is signalled.
    VkResult present(u32 image_index);

    GpuDevice* gpu = nullptr;
    SDL_Window* window = nullptr;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat preferred_format = VK_FORMAT_B8G8R8A8_SRGB;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent = {0, 0};
    u32 min_image_count = 0;

    u32 image_count = 0;
    VkImage images[MAX_SWAPCHAIN_IMAGES] = {};
    VkImageView views[MAX_SWAPCHAIN_IMAGES] = {};
    VkSemaphore render_finished[MAX_SWAPCHAIN_IMAGES] = {};

private:
    bool create_swapchain();
    bool fetch_images();
    void destroy_images();
};
