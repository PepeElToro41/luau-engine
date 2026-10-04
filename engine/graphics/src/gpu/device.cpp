#include "engine/gpu/device.hpp"

#include "engine/memory/temporal_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "gpu/vk_check.hpp"

#include <SDL3/SDL_vulkan.h>

#include <cstdio>
#include <cstring>

#ifndef NDEBUG
static constexpr bool enable_validation = true;
#else
static constexpr bool enable_validation = false;
#endif

static constexpr const char* validation_layer_name = "VK_LAYER_KHRONOS_validation";

// --- Helpers -----------------------------------------------------------------

static bool has_instance_layer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    TemporalAllocator temp = TemporalAllocator::create();
    DynamicArray<VkLayerProperties> layers(&temp);
    layers.resize(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data);

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
    TemporalAllocator temp = TemporalAllocator::create();
    DynamicArray<VkExtensionProperties> extensions(&temp);
    extensions.resize(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data);

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
    TemporalAllocator temp = TemporalAllocator::create();
    DynamicArray<VkExtensionProperties> extensions(&temp);
    extensions.resize(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data);

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
    TemporalAllocator temp = TemporalAllocator::create();
    DynamicArray<VkQueueFamilyProperties> families(&temp);
    families.resize(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data);

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

// --- Init steps --------------------------------------------------------------

bool create_gpu_instance(GpuDevice& gpu) {
    if (!vk_check(volkInitialize(), "volkInitialize")) {
        return false;
    }

    uint32_t sdl_extension_count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_extension_count);
    if (sdl_extensions == nullptr) {
        fprintf(stderr, "[vulkan] SDL_Vulkan_GetInstanceExtensions failed: %s\n", SDL_GetError());
        return false;
    }

    TemporalAllocator temp = TemporalAllocator::create();
    // Reserve the final sizes up front: the arrays live on an arena, so growing
    // them would leak the old block until the scope ends. At most one extension
    // (debug utils) and one layer (validation) are added to SDL's list.
    DynamicArray<const char*> extensions(&temp);
    extensions.reserve(sdl_extension_count + 1);
    DynamicArray<const char*> layers(&temp);
    layers.reserve(1);
    for (uint32_t i = 0; i < sdl_extension_count; ++i) {
        extensions.push(sdl_extensions[i]);
    }

    bool use_debug_messenger = false;
    if (enable_validation) {
        if (has_instance_layer(validation_layer_name)) {
            layers.push(validation_layer_name);
        } else {
            fprintf(stderr, "[vulkan] %s not available, running without validation\n", validation_layer_name);
        }
        if (has_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            extensions.push(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
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
    create_info.enabledLayerCount = static_cast<uint32_t>(layers.count);
    create_info.ppEnabledLayerNames = layers.data;
    create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.count);
    create_info.ppEnabledExtensionNames = extensions.data;

    if (!vk_check(vkCreateInstance(&create_info, nullptr, &gpu.instance), "vkCreateInstance")) {
        gpu.instance = VK_NULL_HANDLE;
        return false;
    }
    volkLoadInstance(gpu.instance);

    if (use_debug_messenger) {
        if (!vk_check(vkCreateDebugUtilsMessengerEXT(gpu.instance, &debug_info, nullptr, &gpu.debug_messenger),
                      "vkCreateDebugUtilsMessengerEXT")) {
            gpu.debug_messenger = VK_NULL_HANDLE;
        }
    }

    return true;
}

bool create_gpu_surface(GpuDevice& gpu, SDL_Window* window) {
    if (!SDL_Vulkan_CreateSurface(window, gpu.instance, nullptr, &gpu.surface)) {
        fprintf(stderr, "[vulkan] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        gpu.surface = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool pick_gpu_physical_device(GpuDevice& gpu) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(gpu.instance, &count, nullptr);
    if (count == 0) {
        fprintf(stderr, "[vulkan] no physical devices with Vulkan support\n");
        return false;
    }
    TemporalAllocator temp = TemporalAllocator::create();
    DynamicArray<VkPhysicalDevice> devices(&temp);
    devices.resize(count);
    vkEnumeratePhysicalDevices(gpu.instance, &count, devices.data);

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

        QueueFamilies families = find_queue_families(candidate, gpu.surface);
        if (!families.complete()) {
            continue;
        }

        uint32_t format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, gpu.surface, &format_count, nullptr);
        uint32_t present_mode_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, gpu.surface, &present_mode_count, nullptr);
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

    gpu.physical_device = best;
    vkGetPhysicalDeviceProperties(best, &gpu.properties);
    vkGetPhysicalDeviceMemoryProperties(best, &gpu.memory_properties);
    gpu.graphics_queue_family = best_families.graphics;
    gpu.present_queue_family = best_families.present;
    fprintf(stderr, "[vulkan] using %s\n", gpu.properties.deviceName);
    return true;
}

bool create_gpu_device(GpuDevice& gpu) {
    float priority = 1.0f;

    TemporalAllocator temp = TemporalAllocator::create();
    uint32_t families[] = {gpu.graphics_queue_family, gpu.present_queue_family};
    uint32_t family_count = (families[0] == families[1]) ? 1 : 2;
    DynamicArray<VkDeviceQueueCreateInfo> queue_infos(&temp);
    queue_infos.reserve(family_count);
    for (uint32_t i = 0; i < family_count; ++i) {
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = families[i];
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        queue_infos.push(queue_info);
    }

    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    // Vulkan 1.1 target: no optional features enabled yet.
    VkPhysicalDeviceFeatures features{};

    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.pEnabledFeatures = &features;
    create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.count);
    create_info.pQueueCreateInfos = queue_infos.data;
    create_info.enabledExtensionCount = 1;
    create_info.ppEnabledExtensionNames = extensions;

    if (!vk_check(vkCreateDevice(gpu.physical_device, &create_info, nullptr, &gpu.device), "vkCreateDevice")) {
        gpu.device = VK_NULL_HANDLE;
        return false;
    }
    volkLoadDevice(gpu.device);

    vkGetDeviceQueue(gpu.device, gpu.graphics_queue_family, 0, &gpu.graphics_queue);
    vkGetDeviceQueue(gpu.device, gpu.present_queue_family, 0, &gpu.present_queue);
    return true;
}

// --- GpuDevice ---------------------------------------------------------------

bool GpuDevice::init(SDL_Window* window) {
    return create_gpu_instance(*this)
        && create_gpu_surface(*this, window)
        && pick_gpu_physical_device(*this)
        && create_gpu_device(*this);
}

void GpuDevice::shutdown() {
    if (this->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(this->device);
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
}

void GpuDevice::wait_idle() const {
    if (this->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(this->device);
    }
}

u32 GpuDevice::find_memory_type(const u32 type_bits, const VkMemoryPropertyFlags properties) const {
    for (u32 i = 0; i < this->memory_properties.memoryTypeCount; ++i) {
        const bool allowed = (type_bits & (1u << i)) != 0;
        const bool matches = (this->memory_properties.memoryTypes[i].propertyFlags & properties) == properties;
        if (allowed && matches) {
            return i;
        }
    }
    return UINT32_MAX;
}
