#include "engine/memory/temporal_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "gpu/vulkan/vk_check.hpp"
#include "gpu/vulkan/vk_context.hpp"

#include <SDL3/SDL_vulkan.h>

#include <cstring>

static constexpr const char* VALIDATION_LAYER_NAME = "VK_LAYER_KHRONOS_validation";

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

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data) {
    (void)type;
    (void)user_data;
    GPU::LogLevel level = GPU::LOG_INFO;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        level = GPU::LOG_ERROR;
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        level = GPU::LOG_WARNING;
    }
    GPU::log(level, "[vulkan] %s", data->pMessage);
    return VK_FALSE;
}

static VkDebugUtilsMessengerCreateInfoEXT debug_messenger_info() {
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debug_callback;
    return info;
}

struct QueueFamilies {
    uint32_t graphics = UINT32_MAX;
    uint32_t present = UINT32_MAX;

    bool complete() const { return this->graphics != UINT32_MAX && this->present != UINT32_MAX; }
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
        const bool graphics = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
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

static bool create_instance(VulkanContext& vk) {
    if (!vk_check(volkInitialize(), "volkInitialize")) {
        return false;
    }

    uint32_t sdl_extension_count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_extension_count);
    if (sdl_extensions == nullptr) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        return false;
    }

    TemporalAllocator temp = TemporalAllocator::create();
    // Reserve the final sizes up front: the arrays live on an arena, so
    // growing them would leak the old block until the scope ends.
    DynamicArray<const char*> extensions(&temp);
    extensions.reserve(sdl_extension_count + 1);
    DynamicArray<const char*> layers(&temp);
    layers.reserve(1);
    for (uint32_t i = 0; i < sdl_extension_count; ++i) {
        extensions.push(sdl_extensions[i]);
    }

    bool use_debug_messenger = false;
    if (vk.validation) {
        if (has_instance_layer(VALIDATION_LAYER_NAME)) {
            layers.push(VALIDATION_LAYER_NAME);
        } else {
            GPU::log(GPU::LOG_WARNING, "[vulkan] %s not available, running without validation", VALIDATION_LAYER_NAME);
        }
    }
    if (has_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        extensions.push(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        vk.has_debug_utils = true;
        use_debug_messenger = vk.validation;
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
    VkDebugUtilsMessengerCreateInfoEXT debug_info = debug_messenger_info();

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pNext = use_debug_messenger ? &debug_info : nullptr;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledLayerCount = static_cast<uint32_t>(layers.count);
    create_info.ppEnabledLayerNames = layers.data;
    create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.count);
    create_info.ppEnabledExtensionNames = extensions.data;

    if (!vk_check(vkCreateInstance(&create_info, nullptr, &vk.instance), "vkCreateInstance")) {
        vk.instance = VK_NULL_HANDLE;
        return false;
    }
    volkLoadInstance(vk.instance);

    if (use_debug_messenger) {
        if (!vk_check(vkCreateDebugUtilsMessengerEXT(vk.instance, &debug_info, nullptr, &vk.debug_messenger), "vkCreateDebugUtilsMessengerEXT")) {
            vk.debug_messenger = VK_NULL_HANDLE;
        }
    }
    return true;
}

static bool create_surface(VulkanContext& vk) {
    if (!SDL_Vulkan_CreateSurface(vk.window, vk.instance, nullptr, &vk.surface)) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        vk.surface = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

static bool pick_physical_device(VulkanContext& vk) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(vk.instance, &count, nullptr);
    if (count == 0) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] no physical devices with Vulkan support");
        return false;
    }
    TemporalAllocator temp = TemporalAllocator::create();
    DynamicArray<VkPhysicalDevice> devices(&temp);
    devices.resize(count);
    vkEnumeratePhysicalDevices(vk.instance, &count, devices.data);

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
        const QueueFamilies families = find_queue_families(candidate, vk.surface);
        if (!families.complete()) {
            continue;
        }
        uint32_t format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, vk.surface, &format_count, nullptr);
        uint32_t present_mode_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, vk.surface, &present_mode_count, nullptr);
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
        GPU::log(GPU::LOG_ERROR, "[vulkan] no suitable physical device (need Vulkan 1.1, swapchain support, graphics + present queues)");
        return false;
    }

    vk.physical_device = best;
    vkGetPhysicalDeviceProperties(best, &vk.properties);
    vkGetPhysicalDeviceMemoryProperties(best, &vk.memory_properties);
    vk.graphics_queue_family = best_families.graphics;
    vk.present_queue_family = best_families.present;
    GPU::log(GPU::LOG_INFO, "[vulkan] using %s", vk.properties.deviceName);
    return true;
}

static bool create_device(VulkanContext& vk) {
    float priority = 1.0f;
    TemporalAllocator temp = TemporalAllocator::create();
    const uint32_t families[] = {vk.graphics_queue_family, vk.present_queue_family};
    const uint32_t family_count = (families[0] == families[1]) ? 1 : 2;
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

    // Vulkan 1.1 target. The one feature beyond the core 1.0 set is
    // shaderDrawParameters (core in 1.1): Slang's SV_VertexID is relative to
    // the draw's base vertex, so its SPIR-V reads BaseVertex.
    VkPhysicalDeviceVulkan11Features available_11{};
    available_11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceFeatures2 available{};
    available.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    available.pNext = &available_11;
    vkGetPhysicalDeviceFeatures2(vk.physical_device, &available);
    if (!available_11.shaderDrawParameters) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] the device does not support shaderDrawParameters");
        return false;
    }
    VkPhysicalDeviceVulkan11Features features_11{};
    features_11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features_11.shaderDrawParameters = VK_TRUE;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features_11;
    features.features.samplerAnisotropy = available.features.samplerAnisotropy;
    features.features.fillModeNonSolid = available.features.fillModeNonSolid;

    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.pNext = &features;
    create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.count);
    create_info.pQueueCreateInfos = queue_infos.data;
    create_info.enabledExtensionCount = 1;
    create_info.ppEnabledExtensionNames = extensions;

    if (!vk_check(vkCreateDevice(vk.physical_device, &create_info, nullptr, &vk.device), "vkCreateDevice")) {
        vk.device = VK_NULL_HANDLE;
        return false;
    }
    volkLoadDevice(vk.device);
    vkGetDeviceQueue(vk.device, vk.graphics_queue_family, 0, &vk.graphics_queue);
    vkGetDeviceQueue(vk.device, vk.present_queue_family, 0, &vk.present_queue);

    // What the frontend may read.
    GpuInfo& info = vk.gpu->info;
    strncpy(info.device_name, vk.properties.deviceName, sizeof(info.device_name) - 1);
    info.bytecode = GPU_BYTECODE_SPIRV;
    info.limits.max_push_constant_size = vk.properties.limits.maxPushConstantsSize < GPU_MAX_PUSH_CONSTANT_SIZE ? vk.properties.limits.maxPushConstantsSize : GPU_MAX_PUSH_CONSTANT_SIZE;
    info.limits.uniform_buffer_alignment = static_cast<u32>(vk.properties.limits.minUniformBufferOffsetAlignment);
    info.limits.max_anisotropy = available.features.samplerAnisotropy ? static_cast<u32>(vk.properties.limits.maxSamplerAnisotropy) : 1;
    info.limits.max_texture_size = vk.properties.limits.maxImageDimension2D;
    vk.depth_format = VK_FORMATS::find_depth_format(vk);
    info.depth_format = VK_FORMATS::from_vk(vk.depth_format);
    return true;
}

// --- VK_DEVICE ---------------------------------------------------------------

bool VK_DEVICE::init(VulkanContext& vk) {
    return create_instance(vk) && create_surface(vk) && pick_physical_device(vk) && create_device(vk);
}

void VK_DEVICE::shutdown(VulkanContext& vk) {
    if (vk.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vk.device);
        vkDestroyDevice(vk.device, nullptr);
        vk.device = VK_NULL_HANDLE;
    }
    vk.graphics_queue = VK_NULL_HANDLE;
    vk.present_queue = VK_NULL_HANDLE;
    vk.graphics_queue_family = UINT32_MAX;
    vk.present_queue_family = UINT32_MAX;
    vk.physical_device = VK_NULL_HANDLE;
    if (vk.surface != VK_NULL_HANDLE) {
        SDL_Vulkan_DestroySurface(vk.instance, vk.surface, nullptr);
        vk.surface = VK_NULL_HANDLE;
    }
    if (vk.debug_messenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(vk.instance, vk.debug_messenger, nullptr);
        vk.debug_messenger = VK_NULL_HANDLE;
    }
    if (vk.instance != VK_NULL_HANDLE) {
        vkDestroyInstance(vk.instance, nullptr);
        vk.instance = VK_NULL_HANDLE;
    }
}

u32 VK_DEVICE::find_memory_type(const VulkanContext& vk, const u32 type_bits, const VkMemoryPropertyFlags properties) {
    for (u32 i = 0; i < vk.memory_properties.memoryTypeCount; ++i) {
        const bool allowed = (type_bits & (1u << i)) != 0;
        const bool matches = (vk.memory_properties.memoryTypes[i].propertyFlags & properties) == properties;
        if (allowed && matches) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool VK_DEVICE::allocate_memory(VulkanContext& vk, const VkMemoryRequirements& requirements, const VkMemoryPropertyFlags properties, VkDeviceMemory& out, const char* what) {
    const u32 memory_type = find_memory_type(vk, requirements.memoryTypeBits, properties);
    if (memory_type == UINT32_MAX) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] no memory type with properties 0x%x for %s", static_cast<unsigned>(properties), what);
        return false;
    }
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = requirements.size;
    alloc_info.memoryTypeIndex = memory_type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (!vk_check(vkAllocateMemory(vk.device, &alloc_info, nullptr, &memory), "vkAllocateMemory")) {
        return false;
    }
    out = memory;
    return true;
}
