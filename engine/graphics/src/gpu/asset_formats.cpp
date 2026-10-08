#include "engine/gpu/asset_formats.hpp"

#include "engine/asset/asset_types/mesh_asset.hpp"
#include "engine/asset/asset_types/texture_asset.hpp"

VkFormat ASSET_FORMATS::vertex_format(const u32 format) {
    switch (format) {
    case VERTEX_FORMAT_F32:
        return VK_FORMAT_R32_SFLOAT;
    case VERTEX_FORMAT_F32x2:
        return VK_FORMAT_R32G32_SFLOAT;
    case VERTEX_FORMAT_F32x3:
        return VK_FORMAT_R32G32B32_SFLOAT;
    case VERTEX_FORMAT_F32x4:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case VERTEX_FORMAT_F16x2:
        return VK_FORMAT_R16G16_SFLOAT;
    case VERTEX_FORMAT_F16x4:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VERTEX_FORMAT_UNORM8x4:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case VERTEX_FORMAT_SNORM8x4:
        return VK_FORMAT_R8G8B8A8_SNORM;
    case VERTEX_FORMAT_UNORM16x2:
        return VK_FORMAT_R16G16_UNORM;
    case VERTEX_FORMAT_UNORM16x4:
        return VK_FORMAT_R16G16B16A16_UNORM;
    case VERTEX_FORMAT_SNORM16x2:
        return VK_FORMAT_R16G16_SNORM;
    case VERTEX_FORMAT_SNORM16x4:
        return VK_FORMAT_R16G16B16A16_SNORM;
    case VERTEX_FORMAT_UINT8x4:
        return VK_FORMAT_R8G8B8A8_UINT;
    case VERTEX_FORMAT_UINT16x4:
        return VK_FORMAT_R16G16B16A16_UINT;
    case VERTEX_FORMAT_UINT32:
        return VK_FORMAT_R32_UINT;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

bool ASSET_FORMATS::vertex_format_is_integer(const u32 format) {
    return format == VERTEX_FORMAT_UINT8x4 || format == VERTEX_FORMAT_UINT16x4 || format == VERTEX_FORMAT_UINT32;
}

VkFormat ASSET_FORMATS::texture_format(const u32 format) {
    switch (format) {
    case TEXTURE_FORMAT_R8_UNORM:
        return VK_FORMAT_R8_UNORM;
    case TEXTURE_FORMAT_RG8_UNORM:
        return VK_FORMAT_R8G8_UNORM;
    case TEXTURE_FORMAT_RGBA8_UNORM:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case TEXTURE_FORMAT_RGBA8_SRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case TEXTURE_FORMAT_R16_UNORM:
        return VK_FORMAT_R16_UNORM;
    case TEXTURE_FORMAT_RG16_UNORM:
        return VK_FORMAT_R16G16_UNORM;
    case TEXTURE_FORMAT_RGBA16_UNORM:
        return VK_FORMAT_R16G16B16A16_UNORM;
    case TEXTURE_FORMAT_R16_FLOAT:
        return VK_FORMAT_R16_SFLOAT;
    case TEXTURE_FORMAT_RG16_FLOAT:
        return VK_FORMAT_R16G16_SFLOAT;
    case TEXTURE_FORMAT_RGBA16_FLOAT:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TEXTURE_FORMAT_R32_FLOAT:
        return VK_FORMAT_R32_SFLOAT;
    case TEXTURE_FORMAT_RG32_FLOAT:
        return VK_FORMAT_R32G32_SFLOAT;
    case TEXTURE_FORMAT_RGBA32_FLOAT:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case TEXTURE_FORMAT_BC1_RGB_UNORM:
        return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case TEXTURE_FORMAT_BC1_RGB_SRGB:
        return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
    case TEXTURE_FORMAT_BC3_UNORM:
        return VK_FORMAT_BC3_UNORM_BLOCK;
    case TEXTURE_FORMAT_BC3_SRGB:
        return VK_FORMAT_BC3_SRGB_BLOCK;
    case TEXTURE_FORMAT_BC4_UNORM:
        return VK_FORMAT_BC4_UNORM_BLOCK;
    case TEXTURE_FORMAT_BC5_UNORM:
        return VK_FORMAT_BC5_UNORM_BLOCK;
    case TEXTURE_FORMAT_BC6H_UFLOAT:
        return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case TEXTURE_FORMAT_BC7_UNORM:
        return VK_FORMAT_BC7_UNORM_BLOCK;
    case TEXTURE_FORMAT_BC7_SRGB:
        return VK_FORMAT_BC7_SRGB_BLOCK;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}
