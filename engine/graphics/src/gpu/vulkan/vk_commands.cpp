#include "gpu/vulkan/vk_check.hpp"
#include "gpu/vulkan/vk_context.hpp"

// --- Barriers ---------------------------------------------------------------------

void VK_COMMANDS::barrier(const VkCommandBuffer cmd, VkTextureEntry& texture, const GpuResourceState from, const GpuResourceState to) {
    const VK_FORMATS::StateInfo src = VK_FORMATS::state_info(from, true);
    const VK_FORMATS::StateInfo dst = VK_FORMATS::state_info(to, false);
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = src.access;
    barrier.dstAccessMask = dst.access;
    barrier.oldLayout = src.layout;
    barrier.newLayout = dst.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange.aspectMask = texture.aspect;
    barrier.subresourceRange.levelCount = texture.mip_levels;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, src.stage, dst.stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    texture.state = to;
}

static void vk_cmd_barrier(const GpuCommandList cmd, const GpuTexture texture, const GpuResourceState from, const GpuResourceState to) {
    VkCommandListEntry* list = nullptr;
    VulkanContext& vk = VK_CONTEXT::of_command_list(cmd, &list);
    VkTextureEntry* entry = VK_CONTEXT::texture(vk, texture.id);
    if (list == nullptr || entry == nullptr) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] barrier: invalid command list or texture");
        return;
    }
    if (to == GPU_STATE_UNDEFINED) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] barrier: cannot transition into UNDEFINED");
        return;
    }
    VK_COMMANDS::barrier(list->cmd, *entry, from, to);
}

// --- Binding ----------------------------------------------------------------------

static void vk_cmd_bind_pipeline(const GpuCommandList cmd, const GpuPipeline pipeline) {
    VkCommandListEntry* list = nullptr;
    VulkanContext& vk = VK_CONTEXT::of_command_list(cmd, &list);
    const VkPipelineEntry* entry = VK_CONTEXT::pipeline(vk, pipeline.id);
    if (list == nullptr || entry == nullptr) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] bind_pipeline: invalid command list or pipeline");
        return;
    }
    vkCmdBindPipeline(list->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, entry->pipeline);
    list->bound_layout = entry->layout;
    list->bound_push_size = entry->push_constant_size;
}

static void vk_cmd_bind_group(const GpuCommandList cmd, const u32 index, const GpuBindGroup group) {
    VkCommandListEntry* list = nullptr;
    VulkanContext& vk = VK_CONTEXT::of_command_list(cmd, &list);
    const VkBindGroupEntry* entry = VK_CONTEXT::bind_group(vk, group.id);
    if (list == nullptr || entry == nullptr) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] bind_group: invalid command list or group");
        return;
    }
    if (list->bound_layout == VK_NULL_HANDLE) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] bind_group: no pipeline is bound");
        return;
    }
    vkCmdBindDescriptorSets(list->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, list->bound_layout, index, 1, &entry->set, 0, nullptr);
}

static void vk_cmd_push_constants(const GpuCommandList cmd, const void* data, const u32 size) {
    VkCommandListEntry* list = nullptr;
    VK_CONTEXT::of_command_list(cmd, &list);
    if (list == nullptr) {
        return;
    }
    if (list->bound_layout == VK_NULL_HANDLE) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] push_constants: no pipeline is bound");
        return;
    }
    if (size > list->bound_push_size) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] push_constants: %u bytes but the pipeline declares %u", size, list->bound_push_size);
        return;
    }
    vkCmdPushConstants(list->cmd, list->bound_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, data);
}

static void vk_cmd_bind_vertex_buffer(const GpuCommandList cmd, const u32 binding, const GpuBuffer buffer, const u64 offset) {
    VkCommandListEntry* list = nullptr;
    VulkanContext& vk = VK_CONTEXT::of_command_list(cmd, &list);
    const VkBufferEntry* entry = VK_CONTEXT::buffer(vk, buffer.id);
    if (list == nullptr || entry == nullptr) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] bind_vertex_buffer: invalid command list or buffer");
        return;
    }
    const VkDeviceSize vk_offset = offset;
    vkCmdBindVertexBuffers(list->cmd, binding, 1, &entry->buffer, &vk_offset);
}

static void vk_cmd_bind_index_buffer(const GpuCommandList cmd, const GpuBuffer buffer, const GpuIndexType type, const u64 offset) {
    VkCommandListEntry* list = nullptr;
    VulkanContext& vk = VK_CONTEXT::of_command_list(cmd, &list);
    const VkBufferEntry* entry = VK_CONTEXT::buffer(vk, buffer.id);
    if (list == nullptr || entry == nullptr) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] bind_index_buffer: invalid command list or buffer");
        return;
    }
    vkCmdBindIndexBuffer(list->cmd, entry->buffer, offset, type == GPU_INDEX_U32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
}

// --- Draws ------------------------------------------------------------------------

static void vk_cmd_set_viewport(const GpuCommandList cmd, const u32 width, const u32 height) {
    VkCommandListEntry* list = nullptr;
    VK_CONTEXT::of_command_list(cmd, &list);
    if (list == nullptr) {
        return;
    }
    VkViewport viewport{};
    viewport.width = static_cast<f32>(width);
    viewport.height = static_cast<f32>(height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(list->cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {width, height};
    vkCmdSetScissor(list->cmd, 0, 1, &scissor);
}

static void vk_cmd_draw(const GpuCommandList cmd, const u32 vertex_count, const u32 first_vertex, const u32 instance_count) {
    VkCommandListEntry* list = nullptr;
    VK_CONTEXT::of_command_list(cmd, &list);
    if (list != nullptr) {
        vkCmdDraw(list->cmd, vertex_count, instance_count, first_vertex, 0);
    }
}

static void vk_cmd_draw_indexed(const GpuCommandList cmd, const u32 index_count, const u32 first_index, const i32 vertex_offset, const u32 instance_count) {
    VkCommandListEntry* list = nullptr;
    VK_CONTEXT::of_command_list(cmd, &list);
    if (list != nullptr) {
        vkCmdDrawIndexed(list->cmd, index_count, instance_count, first_index, vertex_offset, 0);
    }
}

// --- Transfers ----------------------------------------------------------------------

static void vk_cmd_clear_color(const GpuCommandList cmd, const GpuTexture texture, const f32 rgba[4]) {
    VkCommandListEntry* list = nullptr;
    VulkanContext& vk = VK_CONTEXT::of_command_list(cmd, &list);
    const VkTextureEntry* entry = VK_CONTEXT::texture(vk, texture.id);
    if (list == nullptr || entry == nullptr) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] clear_color: invalid command list or texture");
        return;
    }
    VkImageSubresourceRange range{};
    range.aspectMask = entry->aspect;
    range.levelCount = entry->mip_levels;
    range.layerCount = 1;
    VkClearColorValue value{};
    for (u32 i = 0; i < 4; ++i) {
        value.float32[i] = rgba[i];
    }
    vkCmdClearColorImage(list->cmd, entry->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
}

static void vk_cmd_clear_depth(const GpuCommandList cmd, const GpuTexture texture, const f32 depth, const u32 stencil) {
    VkCommandListEntry* list = nullptr;
    VulkanContext& vk = VK_CONTEXT::of_command_list(cmd, &list);
    const VkTextureEntry* entry = VK_CONTEXT::texture(vk, texture.id);
    if (list == nullptr || entry == nullptr) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] clear_depth: invalid command list or texture");
        return;
    }
    VkImageSubresourceRange range{};
    range.aspectMask = entry->aspect;
    range.levelCount = entry->mip_levels;
    range.layerCount = 1;
    VkClearDepthStencilValue value{};
    value.depth = depth;
    value.stencil = stencil;
    vkCmdClearDepthStencilImage(list->cmd, entry->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
}

static void vk_cmd_blit(const GpuCommandList cmd, const GpuTexture src, const GpuTexture dst, const bool linear) {
    VkCommandListEntry* list = nullptr;
    VulkanContext& vk = VK_CONTEXT::of_command_list(cmd, &list);
    const VkTextureEntry* source = VK_CONTEXT::texture(vk, src.id);
    const VkTextureEntry* target = VK_CONTEXT::texture(vk, dst.id);
    if (list == nullptr || source == nullptr || target == nullptr) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] blit: invalid command list or texture");
        return;
    }
    VkImageBlit region{};
    region.srcSubresource.aspectMask = source->aspect;
    region.srcSubresource.layerCount = 1;
    region.srcOffsets[1] = {static_cast<i32>(source->width), static_cast<i32>(source->height), 1};
    region.dstSubresource.aspectMask = target->aspect;
    region.dstSubresource.layerCount = 1;
    region.dstOffsets[1] = {static_cast<i32>(target->width), static_cast<i32>(target->height), 1};
    // Depth images may only be blitted with NEAREST.
    const bool depth = (source->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    vkCmdBlitImage(list->cmd, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
        linear && !depth ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

// --- Labels -----------------------------------------------------------------------

static void vk_cmd_begin_label(const GpuCommandList cmd, const char* name) {
    VkCommandListEntry* list = nullptr;
    VulkanContext& vk = VK_CONTEXT::of_command_list(cmd, &list);
    if (list == nullptr || !vk.has_debug_utils || vkCmdBeginDebugUtilsLabelEXT == nullptr) {
        return;
    }
    VkDebugUtilsLabelEXT label{};
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name;
    vkCmdBeginDebugUtilsLabelEXT(list->cmd, &label);
}

static void vk_cmd_end_label(const GpuCommandList cmd) {
    VkCommandListEntry* list = nullptr;
    VulkanContext& vk = VK_CONTEXT::of_command_list(cmd, &list);
    if (list == nullptr || !vk.has_debug_utils || vkCmdEndDebugUtilsLabelEXT == nullptr) {
        return;
    }
    vkCmdEndDebugUtilsLabelEXT(list->cmd);
}

namespace VK_COMMANDS_TABLE {
void fill(GpuBackend& table) {
    table.cmd_barrier = vk_cmd_barrier;
    table.cmd_bind_pipeline = vk_cmd_bind_pipeline;
    table.cmd_bind_group = vk_cmd_bind_group;
    table.cmd_push_constants = vk_cmd_push_constants;
    table.cmd_bind_vertex_buffer = vk_cmd_bind_vertex_buffer;
    table.cmd_bind_index_buffer = vk_cmd_bind_index_buffer;
    table.cmd_set_viewport = vk_cmd_set_viewport;
    table.cmd_draw = vk_cmd_draw;
    table.cmd_draw_indexed = vk_cmd_draw_indexed;
    table.cmd_clear_color = vk_cmd_clear_color;
    table.cmd_clear_depth = vk_cmd_clear_depth;
    table.cmd_blit = vk_cmd_blit;
    table.cmd_begin_label = vk_cmd_begin_label;
    table.cmd_end_label = vk_cmd_end_label;
}
} // namespace VK_COMMANDS_TABLE
