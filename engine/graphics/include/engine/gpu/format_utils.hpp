#pragma once

#include "engine/defines.hpp"

#include <volk.h>

// Small VkFormat facts several GPU files need.
namespace GPU_FORMAT {

inline bool has_stencil(const VkFormat format) {
    return format == VK_FORMAT_S8_UINT || format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
           format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

inline bool is_depth(const VkFormat format) {
    return format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_X8_D24_UNORM_PACK32 || format == VK_FORMAT_D32_SFLOAT ||
           format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// The aspect bits an image of `format` is addressed through.
inline VkImageAspectFlags aspect(const VkFormat format) {
    VkImageAspectFlags bits = 0;
    if (is_depth(format)) {
        bits |= VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    if (has_stencil(format)) {
        bits |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    return bits != 0 ? bits : VK_IMAGE_ASPECT_COLOR_BIT;
}

} // namespace GPU_FORMAT
