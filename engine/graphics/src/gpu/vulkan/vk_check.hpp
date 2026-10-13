#pragma once

#include "gpu/backend.hpp"

#include "engine/utils/log.hpp"

#include <volk.h>

// Logs a failed VkResult with the name of the call. Returns true on success.
inline bool vk_check(const VkResult result, const char* what) {
    if (result == VK_SUCCESS) {
        return true;
    }
    LOG::error(LOG_CATEGORY::GPU, "[vulkan] %s failed (VkResult %d)", what, static_cast<int>(result));
    return false;
}
