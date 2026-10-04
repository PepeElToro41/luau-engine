#pragma once

#include "engine/defines.hpp"

#include <SDL3/SDL.h>
#include <volk.h>

// Long-lived Vulkan context: instance, physical device, logical device and
// queues. Created once by the application and shared by everything else
// through a GpuDevice*. It also owns the presentation surface, because the
// physical device and queue families are chosen against it; the Swapchain
// presents to it but does not own it.
struct GpuDevice {
    // Brings up the whole chain: volk, instance (+ validation in debug
    // builds), surface for `window`, physical device, logical device, queues.
    // Returns false on failure, leaving the earlier steps intact so shutdown()
    // can still clean them up.
    bool init(SDL_Window* window);
    void shutdown();

    void wait_idle() const;

    // Index of a memory type that matches `type_bits` and has every flag in
    // `properties`, or UINT32_MAX if there is none.
    u32 find_memory_type(u32 type_bits, VkMemoryPropertyFlags properties) const;

    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties = {};
    VkPhysicalDeviceMemoryProperties memory_properties = {};
    VkDevice device = VK_NULL_HANDLE;

    u32 graphics_queue_family = UINT32_MAX;
    u32 present_queue_family = UINT32_MAX;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkQueue present_queue = VK_NULL_HANDLE;
};

// Init steps, in the order GpuDevice::init runs them. Each one fills in the
// matching members and returns false on failure.
bool create_gpu_instance(GpuDevice& gpu);
bool create_gpu_surface(GpuDevice& gpu, SDL_Window* window);
bool pick_gpu_physical_device(GpuDevice& gpu);
bool create_gpu_device(GpuDevice& gpu);
