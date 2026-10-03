#pragma once

#include "engine/backends/renderer_backend.hpp"

#include <SDL3/SDL.h>
#include <volk.h>

struct VulkanBackend : RendererBackend {
    bool init(SDL_Window* window) override;
    void shutdown() override;

    void begin_frame() override;
    void end_frame() override;

    void resize(uint32_t width, uint32_t height) override;

    SDL_Window* window = nullptr;

    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

    uint32_t graphics_queue_family = UINT32_MAX;
    uint32_t present_queue_family = UINT32_MAX;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkQueue present_queue = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR swapchain_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR swapchain_present_mode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D swapchain_extent = {0, 0};
};

// Init steps, in the order VulkanBackend::init runs them. Each one fills in
// the matching members of `backend` and returns false on failure, leaving
// whatever earlier steps created intact so shutdown() can still clean up.
bool create_vulkan_instance(VulkanBackend& backend);
bool create_vulkan_surface(VulkanBackend& backend);
bool pick_vulkan_physical_device(VulkanBackend& backend);
bool create_vulkan_device(VulkanBackend& backend);

// Creates (or recreates, if one already exists) the swapchain itself. It does
// not fetch the swapchain images or build views for them.
bool create_vulkan_swapchain(VulkanBackend& backend);
