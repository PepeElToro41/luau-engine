#pragma once

#include "gpu/backend.hpp"

#include <volk.h>

// Logs a failed VkResult with the name of the call. Returns true on success.
inline bool vk_check(const VkResult result, const char* what) {
    if (result == VK_SUCCESS) {
        return true;
    }
    GPU::log(GPU::LOG_ERROR, "[vulkan] %s failed (VkResult %d)", what, static_cast<int>(result));
    return false;
}
