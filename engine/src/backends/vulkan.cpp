#include "engine/backends/vulkan.hpp"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#ifndef NDEBUG
static constexpr bool enable_validation = true;
#else
static constexpr bool enable_validation = false;
#endif

static constexpr const char* validation_layer_name = "VK_LAYER_KHRONOS_validation";

// --- Helpers -----------------------------------------------------------------

static bool vk_check(VkResult result, const char* what) {
    if (result == VK_SUCCESS) {
        return true;
    }
    fprintf(stderr, "[vulkan] %s failed (VkResult %d)\n", what, static_cast<int>(result));
    return false;
}

static bool has_instance_layer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());

    for (const VkLayerProperties& layer : layers) {
        if (strcmp(layer.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

static bool has_instance_extension(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());

    for (const VkExtensionProperties& extension : extensions) {
        if (strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

static bool has_device_extension(VkPhysicalDevice device, const char* name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());

    for (const VkExtensionProperties& extension : extensions) {
        if (strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL vulkan_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* user_data
    ) {
    (void)type;
    (void)user_data;

    const char* level = "info";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        level = "error";
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        level = "warning";
    }

    fprintf(stderr, "[vulkan][%s] %s\n", level, data->pMessage);
    return VK_FALSE;
}

static VkDebugUtilsMessengerCreateInfoEXT make_debug_messenger_info() {
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                     | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                     | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = vulkan_debug_callback;
    return info;
}

struct QueueFamilies {
    uint32_t graphics = UINT32_MAX;
    uint32_t present = UINT32_MAX;

    bool complete() const {
        return this->graphics != UINT32_MAX && this->present != UINT32_MAX;
    }
};

static QueueFamilies find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    QueueFamilies result;
    for (uint32_t i = 0; i < count; ++i) {
        bool graphics = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;

        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present);

        // A single family that does both is ideal: no ownership transfers.
        if (graphics && present) {
            result.graphics = i;
            result.present = i;
            return result;
        }
        if (graphics && result.graphics == UINT32_MAX) {
            result.graphics = i;
        }
        if (present && result.present == UINT32_MAX) {
            result.present = i;
        }
    }
    return result;
}

bool create_vulkan_instance(VulkanBackend& backend) {
    if (!vk_check(volkInitialize(), "volkInitialize")) {
        return false;
    }

    uint32_t sdl_extension_count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_extension_count);
    if (sdl_extensions == nullptr) {
        fprintf(stderr, "[vulkan] SDL_Vulkan_GetInstanceExtensions failed: %s\n", SDL_GetError());
        return false;
    }

    std::vector<const char*> extensions(sdl_extensions, sdl_extensions + sdl_extension_count);
    std::vector<const char*> layers;

    bool use_debug_messenger = false;
    if (enable_validation) {
        if (has_instance_layer(validation_layer_name)) {
            layers.push_back(validation_layer_name);
        } else {
            fprintf(stderr, "[vulkan] %s not available, running without validation\n", validation_layer_name);
        }
        if (has_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            use_debug_messenger = true;
        }
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "LuauEngine";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "LuauEngine";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_1;

    // Chained into the instance create info so messages emitted during
    // vkCreateInstance / vkDestroyInstance themselves are also reported.
    VkDebugUtilsMessengerCreateInfoEXT debug_info = make_debug_messenger_info();

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pNext = use_debug_messenger ? &debug_info : nullptr;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    create_info.ppEnabledLayerNames = layers.data();
    create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();

    if (!vk_check(vkCreateInstance(&create_info, nullptr, &backend.instance), "vkCreateInstance")) {
        backend.instance = VK_NULL_HANDLE;
        return false;
    }
    volkLoadInstance(backend.instance);

    if (use_debug_messenger) {
        if (!vk_check(vkCreateDebugUtilsMessengerEXT(backend.instance, &debug_info, nullptr, &backend.debug_messenger),
                      "vkCreateDebugUtilsMessengerEXT")) {
            backend.debug_messenger = VK_NULL_HANDLE;
        }
    }

    return true;
}

bool create_vulkan_surface(VulkanBackend& backend) {
    if (!SDL_Vulkan_CreateSurface(backend.window, backend.instance, nullptr, &backend.surface)) {
        fprintf(stderr, "[vulkan] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        backend.surface = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool pick_vulkan_physical_device(VulkanBackend& backend) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(backend.instance, &count, nullptr);
    if (count == 0) {
        fprintf(stderr, "[vulkan] no physical devices with Vulkan support\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(backend.instance, &count, devices.data());

    VkPhysicalDevice best = VK_NULL_HANDLE;
    QueueFamilies best_families;
    int best_score = -1;

    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(candidate, &properties);

        if (properties.apiVersion < VK_API_VERSION_1_1) {
            continue;
        }
        if (!has_device_extension(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            continue;
        }

        QueueFamilies families = find_queue_families(candidate, backend.surface);
        if (!families.complete()) {
            continue;
        }

        uint32_t format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, backend.surface, &format_count, nullptr);
        uint32_t present_mode_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, backend.surface, &present_mode_count, nullptr);
        if (format_count == 0 || present_mode_count == 0) {
            continue;
        }

        int score = 0;
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += 1000;
        } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            score += 100;
        }
        if (families.graphics == families.present) {
            score += 10;
        }

        if (score > best_score) {
            best = candidate;
            best_families = families;
            best_score = score;
        }
    }

    if (best == VK_NULL_HANDLE) {
        fprintf(stderr, "[vulkan] no suitable physical device (need Vulkan 1.1, swapchain support, graphics + present queues)\n");
        return false;
    }

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(best, &properties);
    fprintf(stderr, "[vulkan] using %s\n", properties.deviceName);

    backend.physical_device = best;
    backend.graphics_queue_family = best_families.graphics;
    backend.present_queue_family = best_families.present;
    return true;
}

bool create_vulkan_device(VulkanBackend& backend) {
    float priority = 1.0f;

    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    uint32_t families[] = {backend.graphics_queue_family, backend.present_queue_family};
    uint32_t family_count = (families[0] == families[1]) ? 1 : 2;
    for (uint32_t i = 0; i < family_count; ++i) {
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = families[i];
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        queue_infos.push_back(queue_info);
    }

    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    // Vulkan 1.1 target: no optional features enabled yet.
    VkPhysicalDeviceFeatures features{};

    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.pEnabledFeatures = &features;
    create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
    create_info.pQueueCreateInfos = queue_infos.data();
    create_info.enabledExtensionCount = 1;
    create_info.ppEnabledExtensionNames = extensions;

    if (!vk_check(vkCreateDevice(backend.physical_device, &create_info, nullptr, &backend.device), "vkCreateDevice")) {
        backend.device = VK_NULL_HANDLE;
        return false;
    }
    volkLoadDevice(backend.device);

    vkGetDeviceQueue(backend.device, backend.graphics_queue_family, 0, &backend.graphics_queue);
    vkGetDeviceQueue(backend.device, backend.present_queue_family, 0, &backend.present_queue);
    return true;
}

bool create_vulkan_swapchain(VulkanBackend& backend) {
    VkSurfaceCapabilitiesKHR capabilities;
    if (!vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(backend.physical_device, backend.surface, &capabilities),
                  "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
        return false;
    }

    // Format: prefer 8-bit sRGB, otherwise take whatever comes first.
    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(backend.physical_device, backend.surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(backend.physical_device, backend.surface, &format_count, formats.data());

    VkSurfaceFormatKHR surface_format = formats[0];
    for (const VkSurfaceFormatKHR& candidate : formats) {
        if (candidate.format == VK_FORMAT_B8G8R8A8_SRGB && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surface_format = candidate;
            break;
        }
    }

    // Present mode: mailbox if available (low latency, no tearing), else FIFO,
    // which is the only mode the spec guarantees.
    uint32_t present_mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(backend.physical_device, backend.surface, &present_mode_count, nullptr);
    std::vector<VkPresentModeKHR> present_modes(present_mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(backend.physical_device, backend.surface, &present_mode_count, present_modes.data());

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
        SDL_GetWindowSizeInPixels(backend.window, &width, &height);
        extent.width = std::clamp(static_cast<uint32_t>(width),
                                  capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(height),
                                   capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        fprintf(stderr, "[vulkan] cannot create a swapchain for a zero-sized surface (window minimized?)\n");
        return false;
    }

    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }

    VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((capabilities.supportedCompositeAlpha & composite_alpha) == 0) {
        // Take the lowest supported bit instead.
        composite_alpha = static_cast<VkCompositeAlphaFlagBitsKHR>(
            capabilities.supportedCompositeAlpha & (~capabilities.supportedCompositeAlpha + 1));
    }

    VkSwapchainKHR old_swapchain = backend.swapchain;

    VkSwapchainCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = backend.surface;
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

    uint32_t families[] = {backend.graphics_queue_family, backend.present_queue_family};
    if (families[0] != families[1]) {
        create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices = families;
    } else {
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    if (!vk_check(vkCreateSwapchainKHR(backend.device, &create_info, nullptr, &swapchain), "vkCreateSwapchainKHR")) {
        return false;
    }

    if (old_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(backend.device, old_swapchain, nullptr);
    }

    backend.swapchain = swapchain;
    backend.swapchain_format = surface_format.format;
    backend.swapchain_color_space = surface_format.colorSpace;
    backend.swapchain_present_mode = present_mode;
    backend.swapchain_extent = extent;
    return true;
}

// --- VulkanBackend -----------------------------------------------------------

bool VulkanBackend::init(SDL_Window* window) {
    this->window = window;

    return create_vulkan_instance(*this)
        && create_vulkan_surface(*this)
        && pick_vulkan_physical_device(*this)
        && create_vulkan_device(*this)
        && create_vulkan_swapchain(*this);
}

void VulkanBackend::shutdown() {
    if (this->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(this->device);

        if (this->swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(this->device, this->swapchain, nullptr);
            this->swapchain = VK_NULL_HANDLE;
        }

        vkDestroyDevice(this->device, nullptr);
        this->device = VK_NULL_HANDLE;
    }
    this->graphics_queue = VK_NULL_HANDLE;
    this->present_queue = VK_NULL_HANDLE;
    this->graphics_queue_family = UINT32_MAX;
    this->present_queue_family = UINT32_MAX;
    this->physical_device = VK_NULL_HANDLE;

    if (this->surface != VK_NULL_HANDLE) {
        SDL_Vulkan_DestroySurface(this->instance, this->surface, nullptr);
        this->surface = VK_NULL_HANDLE;
    }

    if (this->debug_messenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(this->instance, this->debug_messenger, nullptr);
        this->debug_messenger = VK_NULL_HANDLE;
    }

    if (this->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(this->instance, nullptr);
        this->instance = VK_NULL_HANDLE;
    }

    this->window = nullptr;
}

void VulkanBackend::begin_frame() {}

void VulkanBackend::end_frame() {}

void VulkanBackend::resize(uint32_t width, uint32_t height) {
    (void)width;
    (void)height;
}
