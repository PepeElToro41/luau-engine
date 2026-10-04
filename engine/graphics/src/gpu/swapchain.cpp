#include "engine/gpu/swapchain.hpp"

#include "engine/memory/temporal_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "gpu/vk_check.hpp"

#include <algorithm>
#include <cstdio>

bool Swapchain::init(GpuDevice* gpu, SDL_Window* window, const VkFormat preferred_format) {
    this->gpu = gpu;
    this->window = window;
    this->preferred_format = preferred_format;
    return this->create_swapchain() && this->fetch_images();
}

void Swapchain::shutdown() {
    if (this->gpu == nullptr) {
        return;
    }
    this->destroy_images();
    if (this->swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(this->gpu->device, this->swapchain, nullptr);
        this->swapchain = VK_NULL_HANDLE;
    }
    this->extent = {0, 0};
    this->gpu = nullptr;
    this->window = nullptr;
}

bool Swapchain::recreate() {
    this->destroy_images();
    return this->create_swapchain() && this->fetch_images();
}

VkResult Swapchain::acquire(VkSemaphore image_available, u32* image_index) {
    return vkAcquireNextImageKHR(this->gpu->device, this->swapchain, UINT64_MAX, image_available, VK_NULL_HANDLE, image_index);
}

VkResult Swapchain::present(const u32 image_index) {
    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &this->render_finished[image_index];
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &this->swapchain;
    present_info.pImageIndices = &image_index;
    return vkQueuePresentKHR(this->gpu->present_queue, &present_info);
}

// --- Internals ---------------------------------------------------------------

bool Swapchain::create_swapchain() {
    VkSurfaceCapabilitiesKHR capabilities;
    if (!vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(this->gpu->physical_device, this->gpu->surface, &capabilities),
                  "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
        return false;
    }

    // Format: the preferred one if offered, otherwise whatever comes first.
    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(this->gpu->physical_device, this->gpu->surface, &format_count, nullptr);
    TemporalAllocator temp = TemporalAllocator::create();
    DynamicArray<VkSurfaceFormatKHR> formats(&temp);
    formats.resize(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(this->gpu->physical_device, this->gpu->surface, &format_count, formats.data);

    VkSurfaceFormatKHR surface_format = formats[0];
    for (const VkSurfaceFormatKHR& candidate : formats) {
        if (candidate.format == this->preferred_format && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surface_format = candidate;
            break;
        }
    }

    // Present mode: mailbox if available (low latency, no tearing), else FIFO,
    // which is the only mode the spec guarantees.
    uint32_t present_mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(this->gpu->physical_device, this->gpu->surface, &present_mode_count, nullptr);
    DynamicArray<VkPresentModeKHR> present_modes(&temp);
    present_modes.resize(present_mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(this->gpu->physical_device, this->gpu->surface, &present_mode_count, present_modes.data);

    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR candidate : present_modes) {
        if (candidate == VK_PRESENT_MODE_MAILBOX_KHR) {
            present_mode = candidate;
            break;
        }
    }

    // Extent: the surface usually dictates it; if it reports the "special
    // value" we pick from the window's pixel size within the allowed range.
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(this->window, &width, &height);
        extent.width = std::clamp(static_cast<uint32_t>(width),
                                  capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(height),
                                   capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        // Minimized: nothing to present to until the window comes back.
        return false;
    }

    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }
    image_count = std::min(image_count, MAX_SWAPCHAIN_IMAGES);

    VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((capabilities.supportedCompositeAlpha & composite_alpha) == 0) {
        // Take the lowest supported bit instead.
        composite_alpha = static_cast<VkCompositeAlphaFlagBitsKHR>(
            capabilities.supportedCompositeAlpha & (~capabilities.supportedCompositeAlpha + 1));
    }

    VkSwapchainKHR old_swapchain = this->swapchain;

    VkSwapchainCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = this->gpu->surface;
    create_info.minImageCount = image_count;
    create_info.imageFormat = surface_format.format;
    create_info.imageColorSpace = surface_format.colorSpace;
    create_info.imageExtent = extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    create_info.preTransform = capabilities.currentTransform;
    create_info.compositeAlpha = composite_alpha;
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;
    create_info.oldSwapchain = old_swapchain;

    uint32_t families[] = {this->gpu->graphics_queue_family, this->gpu->present_queue_family};
    if (families[0] != families[1]) {
        create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices = families;
    } else {
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    if (!vk_check(vkCreateSwapchainKHR(this->gpu->device, &create_info, nullptr, &swapchain), "vkCreateSwapchainKHR")) {
        return false;
    }

    if (old_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(this->gpu->device, old_swapchain, nullptr);
    }

    this->swapchain = swapchain;
    this->format = surface_format.format;
    this->color_space = surface_format.colorSpace;
    this->present_mode = present_mode;
    this->extent = extent;
    this->min_image_count = capabilities.minImageCount;
    return true;
}

bool Swapchain::fetch_images() {
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(this->gpu->device, this->swapchain, &count, nullptr);
    if (count > MAX_SWAPCHAIN_IMAGES) {
        fprintf(stderr, "[vulkan] swapchain has %u images, more than the supported %u\n", count, MAX_SWAPCHAIN_IMAGES);
        return false;
    }
    if (!vk_check(vkGetSwapchainImagesKHR(this->gpu->device, this->swapchain, &count, this->images), "vkGetSwapchainImagesKHR")) {
        return false;
    }
    this->image_count = count;

    for (u32 i = 0; i < count; ++i) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = this->images[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = this->format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        if (!vk_check(vkCreateImageView(this->gpu->device, &view_info, nullptr, &this->views[i]), "vkCreateImageView (swapchain)")) {
            this->views[i] = VK_NULL_HANDLE;
            return false;
        }

        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (!vk_check(vkCreateSemaphore(this->gpu->device, &semaphore_info, nullptr, &this->render_finished[i]), "vkCreateSemaphore (render_finished)")) {
            this->render_finished[i] = VK_NULL_HANDLE;
            return false;
        }
    }
    return true;
}

void Swapchain::destroy_images() {
    for (u32 i = 0; i < MAX_SWAPCHAIN_IMAGES; ++i) {
        if (this->views[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(this->gpu->device, this->views[i], nullptr);
            this->views[i] = VK_NULL_HANDLE;
        }
        if (this->render_finished[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(this->gpu->device, this->render_finished[i], nullptr);
            this->render_finished[i] = VK_NULL_HANDLE;
        }
        this->images[i] = VK_NULL_HANDLE;
    }
    this->image_count = 0;
}
