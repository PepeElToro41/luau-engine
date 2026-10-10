#include "gpu/vulkan/vk_context.hpp"

// Indexed by GpuFormat.
static const VkFormat VK_FORMAT_TABLE[GPU_FORMAT_COUNT] = {
    VK_FORMAT_UNDEFINED,

    VK_FORMAT_R8_UNORM,
    VK_FORMAT_R8G8_UNORM,
    VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_R8G8B8A8_SRGB,
    VK_FORMAT_R8G8B8A8_SNORM,
    VK_FORMAT_R8G8B8A8_UINT,
    VK_FORMAT_B8G8R8A8_UNORM,
    VK_FORMAT_B8G8R8A8_SRGB,

    VK_FORMAT_R16_UNORM,
    VK_FORMAT_R16G16_UNORM,
    VK_FORMAT_R16G16B16A16_UNORM,
    VK_FORMAT_R16G16_SNORM,
    VK_FORMAT_R16G16B16A16_SNORM,
    VK_FORMAT_R16G16B16A16_UINT,
    VK_FORMAT_R16_SFLOAT,
    VK_FORMAT_R16G16_SFLOAT,
    VK_FORMAT_R16G16B16A16_SFLOAT,

    VK_FORMAT_R32_UINT,
    VK_FORMAT_R32_SFLOAT,
    VK_FORMAT_R32G32_SFLOAT,
    VK_FORMAT_R32G32B32_SFLOAT,
    VK_FORMAT_R32G32B32A32_SFLOAT,

    VK_FORMAT_BC1_RGB_UNORM_BLOCK,
    VK_FORMAT_BC1_RGB_SRGB_BLOCK,
    VK_FORMAT_BC3_UNORM_BLOCK,
    VK_FORMAT_BC3_SRGB_BLOCK,
    VK_FORMAT_BC4_UNORM_BLOCK,
    VK_FORMAT_BC5_UNORM_BLOCK,
    VK_FORMAT_BC6H_UFLOAT_BLOCK,
    VK_FORMAT_BC7_UNORM_BLOCK,
    VK_FORMAT_BC7_SRGB_BLOCK,

    VK_FORMAT_D16_UNORM,
    VK_FORMAT_D32_SFLOAT,
    VK_FORMAT_D24_UNORM_S8_UINT,
    VK_FORMAT_D32_SFLOAT_S8_UINT,
};

VkFormat VK_FORMATS::to_vk(const GpuFormat format) {
    const u32 index = static_cast<u32>(format);
    return index < GPU_FORMAT_COUNT ? VK_FORMAT_TABLE[index] : VK_FORMAT_UNDEFINED;
}

GpuFormat VK_FORMATS::from_vk(const VkFormat format) {
    for (u32 i = 1; i < GPU_FORMAT_COUNT; ++i) {
        if (VK_FORMAT_TABLE[i] == format) {
            return static_cast<GpuFormat>(i);
        }
    }
    return GPU_FORMAT_UNDEFINED;
}

VkImageAspectFlags VK_FORMATS::aspect(const GpuFormat format) {
    VkImageAspectFlags bits = 0;
    if (GPU_FORMAT::is_depth(format)) {
        bits |= VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    if (GPU_FORMAT::has_stencil(format)) {
        bits |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    return bits != 0 ? bits : VK_IMAGE_ASPECT_COLOR_BIT;
}

VK_FORMATS::StateInfo VK_FORMATS::state_info(const GpuResourceState state, const bool as_source) {
    switch (state) {
    case GPU_STATE_COLOR_ATTACHMENT:
        return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    case GPU_STATE_DEPTH_ATTACHMENT:
        return {VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    case GPU_STATE_SAMPLED:
        return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
    case GPU_STATE_TRANSFER_SRC:
        return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    case GPU_STATE_TRANSFER_DST:
        return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
    case GPU_STATE_STORAGE:
        return {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    case GPU_STATE_PRESENT:
        // Leaving PRESENT: the acquire semaphore is waited at color output,
        // so that is the stage a transition must come after. Entering it:
        // nothing reads it on the queue, the end of the pipe is fine.
        return {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, as_source ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0};
    case GPU_STATE_UNDEFINED:
        break;
    }
    // UNDEFINED as a source waits for every earlier command: frames in
    // flight share images, and a discard must still come after the previous
    // frame's reads of the same image (and after the swapchain acquire).
    return {VK_IMAGE_LAYOUT_UNDEFINED, as_source ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0};
}

VkAttachmentLoadOp VK_FORMATS::to_vk(const GpuLoadOp op) {
    switch (op) {
    case GPU_LOAD_CLEAR:
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case GPU_LOAD_LOAD:
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    case GPU_LOAD_DONT_CARE:
        break;
    }
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

VkAttachmentStoreOp VK_FORMATS::to_vk(const GpuStoreOp op) {
    return op == GPU_STORE_STORE ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

VkDescriptorType VK_FORMATS::to_vk(const GpuBindingType type) {
    switch (type) {
    case GPU_BINDING_UNIFORM_BUFFER:
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case GPU_BINDING_STORAGE_BUFFER:
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case GPU_BINDING_TEXTURE:
        return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case GPU_BINDING_SAMPLER:
        return VK_DESCRIPTOR_TYPE_SAMPLER;
    case GPU_BINDING_STORAGE_TEXTURE:
        return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

VkShaderStageFlags VK_FORMATS::to_vk_stages(const u8 stages) {
    VkShaderStageFlags bits = 0;
    if (stages & GPU_STAGE_VERTEX) {
        bits |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if (stages & GPU_STAGE_FRAGMENT) {
        bits |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    if (stages & GPU_STAGE_COMPUTE) {
        bits |= VK_SHADER_STAGE_COMPUTE_BIT;
    }
    return bits;
}

VkBufferUsageFlags VK_FORMATS::buffer_usage(const u32 usage) {
    VkBufferUsageFlags bits = 0;
    if (usage & GPU_BUFFER_USAGE_VERTEX) {
        bits |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if (usage & GPU_BUFFER_USAGE_INDEX) {
        bits |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (usage & GPU_BUFFER_USAGE_UNIFORM) {
        bits |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if (usage & GPU_BUFFER_USAGE_STORAGE) {
        bits |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if (usage & GPU_BUFFER_USAGE_TRANSFER_SRC) {
        bits |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if (usage & GPU_BUFFER_USAGE_TRANSFER_DST) {
        bits |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    return bits;
}

VkImageUsageFlags VK_FORMATS::texture_usage(const u32 usage) {
    VkImageUsageFlags bits = 0;
    if (usage & GPU_TEXTURE_USAGE_SAMPLED) {
        bits |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (usage & GPU_TEXTURE_USAGE_COLOR_ATTACHMENT) {
        bits |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (usage & GPU_TEXTURE_USAGE_DEPTH_ATTACHMENT) {
        bits |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (usage & GPU_TEXTURE_USAGE_TRANSFER_SRC) {
        bits |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (usage & GPU_TEXTURE_USAGE_TRANSFER_DST) {
        bits |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (usage & GPU_TEXTURE_USAGE_STORAGE) {
        bits |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    return bits;
}

VkFormat VK_FORMATS::find_depth_format(const VulkanContext& vk) {
    const VkFormat candidates[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
    for (const VkFormat candidate : candidates) {
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(vk.physical_device, candidate, &properties);
        if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            return candidate;
        }
    }
    GPU::log(GPU::LOG_ERROR, "[vulkan] no supported depth attachment format");
    return VK_FORMAT_UNDEFINED;
}
