#pragma once

#include "engine/defines.hpp"

#include <volk.h>

// The asset enums (VertexFormat, TextureFormat; engine/asset) as VkFormats.
// Core keeps its own enums so it stays free of Vulkan; this is the one place
// they meet.
namespace ASSET_FORMATS {

// VK_FORMAT_UNDEFINED for a value that is not a VertexFormat.
VkFormat vertex_format(u32 format);
// VK_FORMAT_UNDEFINED for a value that is not a TextureFormat.
VkFormat texture_format(u32 format);
// Whether a VertexFormat feeds integer (uint / int) shader inputs rather
// than float ones.
bool vertex_format_is_integer(u32 format);

} // namespace ASSET_FORMATS
