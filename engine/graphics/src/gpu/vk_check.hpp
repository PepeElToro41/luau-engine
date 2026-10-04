#pragma once

#include <volk.h>

#include <cstdio>

// Logs a failed VkResult with the name of the call. Returns true on success.
inline bool vk_check(VkResult result, const char* what) {
    if (result == VK_SUCCESS) {
        return true;
    }
    fprintf(stderr, "[vulkan] %s failed (VkResult %d)\n", what, static_cast<int>(result));
    return false;
}
