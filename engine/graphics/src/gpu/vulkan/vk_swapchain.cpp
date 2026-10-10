#include "engine/memory/temporal_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "gpu/vulkan/vk_check.hpp"
#include "gpu/vulkan/vk_context.hpp"

#include <algorithm>

static void destroy_images(VulkanContext& vk) {
    for (u32 i = 0; i < VK_MAX_SWAPCHAIN_IMAGES; ++i) {
        if (vk.swapchain_textures[i] != GPU_NULL_ID) {
            VK_RESOURCES::unregister_external_texture(vk, vk.swapchain_textures[i]);
            vk.swapchain_textures[i] = GPU_NULL_ID;
        }
        if (vk.render_finished[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(vk.device, vk.render_finished[i], nullptr);
            vk.render_finished[i] = VK_NULL_HANDLE;
        }
        vk.swapchain_images[i] = VK_NULL_HANDLE;
    }
    vk.swapchain_image_count = 0;
}

static bool create_swapchain(VulkanContext& vk) {
    VkSurfaceCapabilitiesKHR capabilities;
    if (!vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical_device, vk.surface, &capabilities), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
        return false;
    }

    // Format: the preferred one if offered with the sRGB non-linear color
    // space, otherwise whatever comes first.
    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical_device, vk.surface, &format_count, nullptr);
    TemporalAllocator temp = TemporalAllocator::create();
    DynamicArray<VkSurfaceFormatKHR> formats(&temp);
    formats.resize(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical_device, vk.surface, &format_count, formats.data);
    VkSurfaceFormatKHR surface_format = formats[0];
    for (const VkSurfaceFormatKHR& candidate : formats) {
        if (candidate.format == vk.preferred_swapchain_format && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surface_format = candidate;
            break;
        }
    }

    // Present mode: mailbox if available (low latency, no tearing), else
    // FIFO, the only mode the spec guarantees.
    uint32_t present_mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical_device, vk.surface, &present_mode_count, nullptr);
    DynamicArray<VkPresentModeKHR> present_modes(&temp);
    present_modes.resize(present_mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical_device, vk.surface, &present_mode_count, present_modes.data);
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (const VkPresentModeKHR candidate : present_modes) {
        if (candidate == VK_PRESENT_MODE_MAILBOX_KHR) {
            present_mode = candidate;
            break;
        }
    }

    // Extent: the surface usually dictates it; the "special value" means
    // pick from the window's pixel size within the allowed range.
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(vk.window, &width, &height);
        extent.width = std::clamp(static_cast<uint32_t>(width), capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(height), capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        // Minimized: nothing to present to until the window comes back.
        return false;
    }

    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }
    image_count = std::min(image_count, VK_MAX_SWAPCHAIN_IMAGES);

    VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((capabilities.supportedCompositeAlpha & composite_alpha) == 0) {
        // The lowest supported bit instead.
        composite_alpha = static_cast<VkCompositeAlphaFlagBitsKHR>(capabilities.supportedCompositeAlpha & (~capabilities.supportedCompositeAlpha + 1));
    }

    const VkSwapchainKHR old_swapchain = vk.swapchain;

    VkSwapchainCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = vk.surface;
    create_info.minImageCount = image_count;
    create_info.imageFormat = surface_format.format;
    create_info.imageColorSpace = surface_format.colorSpace;
    create_info.imageExtent = extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    create_info.preTransform = capabilities.currentTransform;
    create_info.compositeAlpha = composite_alpha;
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;
    create_info.oldSwapchain = old_swapchain;

    const uint32_t families[] = {vk.graphics_queue_family, vk.present_queue_family};
    if (families[0] != families[1]) {
        create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices = families;
    } else {
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    if (!vk_check(vkCreateSwapchainKHR(vk.device, &create_info, nullptr, &swapchain), "vkCreateSwapchainKHR")) {
        return false;
    }
    if (old_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(vk.device, old_swapchain, nullptr);
    }

    vk.swapchain = swapchain;
    vk.swapchain_format = surface_format.format;
    vk.swapchain_color_space = surface_format.colorSpace;
    vk.present_mode = present_mode;
    vk.swapchain_extent = extent;
    vk.swapchain_min_image_count = capabilities.minImageCount;
    vk.gpu->width = extent.width;
    vk.gpu->height = extent.height;
    vk.gpu->info.swapchain_format = VK_FORMATS::from_vk(surface_format.format);
    if (vk.gpu->info.swapchain_format == GPU_FORMAT_UNDEFINED) {
        GPU::log(GPU::LOG_WARNING, "[vulkan] the swapchain format (VkFormat %d) has no GpuFormat; pipelines drawing into it cannot be described",
            static_cast<int>(surface_format.format));
    }
    return true;
}

static bool fetch_images(VulkanContext& vk) {
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &count, nullptr);
    if (count > VK_MAX_SWAPCHAIN_IMAGES) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] swapchain has %u images, more than the supported %u", count, VK_MAX_SWAPCHAIN_IMAGES);
        return false;
    }
    if (!vk_check(vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &count, vk.swapchain_images), "vkGetSwapchainImagesKHR")) {
        return false;
    }
    vk.swapchain_image_count = count;

    for (u32 i = 0; i < count; ++i) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = vk.swapchain_images[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = vk.swapchain_format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        VkImageView view = VK_NULL_HANDLE;
        if (!vk_check(vkCreateImageView(vk.device, &view_info, nullptr, &view), "vkCreateImageView (swapchain)")) {
            return false;
        }
        vk.swapchain_textures[i] = VK_RESOURCES::register_external_texture(vk, vk.swapchain_images[i], view, vk.swapchain_format, vk.swapchain_extent.width, vk.swapchain_extent.height);

        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (!vk_check(vkCreateSemaphore(vk.device, &semaphore_info, nullptr, &vk.render_finished[i]), "vkCreateSemaphore (render_finished)")) {
            vk.render_finished[i] = VK_NULL_HANDLE;
            return false;
        }
    }
    return true;
}

bool VK_SWAPCHAIN::init(VulkanContext& vk) {
    return create_swapchain(vk) && fetch_images(vk);
}

void VK_SWAPCHAIN::shutdown(VulkanContext& vk) {
    destroy_images(vk);
    if (vk.swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(vk.device, vk.swapchain, nullptr);
        vk.swapchain = VK_NULL_HANDLE;
    }
    vk.swapchain_extent = {0, 0};
}

bool VK_SWAPCHAIN::recreate(VulkanContext& vk) {
    vkDeviceWaitIdle(vk.device);
    destroy_images(vk);
    return create_swapchain(vk) && fetch_images(vk);
}

VkResult VK_SWAPCHAIN::acquire(VulkanContext& vk, const VkSemaphore image_available, u32* image_index) {
    return vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, image_available, VK_NULL_HANDLE, image_index);
}

VkResult VK_SWAPCHAIN::present(VulkanContext& vk, const u32 image_index) {
    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &vk.render_finished[image_index];
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &vk.swapchain;
    present_info.pImageIndices = &image_index;
    return vkQueuePresentKHR(vk.present_queue, &present_info);
}
