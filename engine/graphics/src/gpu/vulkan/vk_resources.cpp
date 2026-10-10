#include "engine/memory/temporal_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "gpu/vulkan/vk_check.hpp"
#include "gpu/vulkan/vk_context.hpp"

#include <cstring>

// Staging offsets for image copies must be a multiple of 4 and of the texel
// block size; 16 covers every uncompressed format and every block-compressed
// one.
static constexpr VkDeviceSize STAGING_ALIGNMENT = 16;

static VkDeviceSize align_up(const VkDeviceSize value, const VkDeviceSize alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static u32 mip_extent(const u32 base, const u32 level) {
    const u32 shifted = base >> level;
    return shifted == 0 ? 1 : shifted;
}

// --- Uploads -----------------------------------------------------------------------

bool VK_RESOURCES::init(VulkanContext& vk) {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = vk.graphics_queue_family;
    if (!vk_check(vkCreateCommandPool(vk.device, &pool_info, nullptr, &vk.upload_pool), "vkCreateCommandPool (upload)")) {
        vk.upload_pool = VK_NULL_HANDLE;
        return false;
    }
    VkCommandBufferAllocateInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_info.commandPool = vk.upload_pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    if (!vk_check(vkAllocateCommandBuffers(vk.device, &cmd_info, &vk.upload_cmd), "vkAllocateCommandBuffers (upload)")) {
        vk.upload_cmd = VK_NULL_HANDLE;
        return false;
    }
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (!vk_check(vkCreateFence(vk.device, &fence_info, nullptr, &vk.upload_fence), "vkCreateFence (upload)")) {
        vk.upload_fence = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

static void destroy_buffer_entry(VulkanContext& vk, const VkBufferEntry& entry) {
    VkPending pending;
    pending.buffer = entry.buffer;
    pending.memory = entry.memory;
    VK_FRAME::destroy(vk, pending);
}

static void destroy_texture_entry(VulkanContext& vk, const VkTextureEntry& entry) {
    VkPending pending;
    pending.view = entry.view;
    pending.sampled_view = entry.sampled_view != entry.view ? entry.sampled_view : VK_NULL_HANDLE;
    if (!entry.external) {
        pending.image = entry.image;
        pending.memory = entry.memory;
    }
    VK_FRAME::destroy(vk, pending);
}

void VK_RESOURCES::shutdown(VulkanContext& vk) {
    // Whatever the frontend did not destroy itself goes now; the GPU is idle.
    for (usz i = 0; i < vk.buffers.alive_count; ++i) {
        const SparseId id = vk.buffers.get_alive_id(i);
        if (id != GPU_NULL_ID) {
            destroy_buffer_entry(vk, *vk.buffers.get_element_alive(id));
        }
    }
    for (usz i = 0; i < vk.textures.alive_count; ++i) {
        const SparseId id = vk.textures.get_alive_id(i);
        if (id != GPU_NULL_ID) {
            destroy_texture_entry(vk, *vk.textures.get_element_alive(id));
        }
    }
    vk.buffers.free();
    vk.textures.free();

    if (vk.upload_fence != VK_NULL_HANDLE) {
        vkDestroyFence(vk.device, vk.upload_fence, nullptr);
        vk.upload_fence = VK_NULL_HANDLE;
    }
    if (vk.upload_pool != VK_NULL_HANDLE) {
        // Frees upload_cmd with it.
        vkDestroyCommandPool(vk.device, vk.upload_pool, nullptr);
        vk.upload_pool = VK_NULL_HANDLE;
    }
    vk.upload_cmd = VK_NULL_HANDLE;
}

static bool begin_upload(VulkanContext& vk) {
    if (!vk_check(vkResetCommandBuffer(vk.upload_cmd, 0), "vkResetCommandBuffer (upload)")) {
        return false;
    }
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vk_check(vkBeginCommandBuffer(vk.upload_cmd, &begin_info), "vkBeginCommandBuffer (upload)");
}

static bool end_upload(VulkanContext& vk) {
    if (!vk_check(vkEndCommandBuffer(vk.upload_cmd), "vkEndCommandBuffer (upload)")) {
        return false;
    }
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &vk.upload_cmd;
    if (!vk_check(vkQueueSubmit(vk.graphics_queue, 1, &submit_info, vk.upload_fence), "vkQueueSubmit (upload)")) {
        return false;
    }
    const bool ok = vk_check(vkWaitForFences(vk.device, 1, &vk.upload_fence, VK_TRUE, UINT64_MAX), "vkWaitForFences (upload)");
    vkResetFences(vk.device, 1, &vk.upload_fence);
    return ok;
}

// --- Buffers -------------------------------------------------------------------------

static bool create_buffer_entry(VulkanContext& vk, const GpuBufferDesc& desc, VkBufferEntry& out) {
    const bool device_local = desc.memory == GPU_MEMORY_DEVICE_LOCAL;
    VkBufferEntry entry;
    entry.size = desc.size;
    entry.usage = desc.usage;
    entry.memory_kind = desc.memory;

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = desc.size;
    buffer_info.usage = VK_FORMATS::buffer_usage(desc.usage) | (device_local ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : 0);
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!vk_check(vkCreateBuffer(vk.device, &buffer_info, nullptr, &entry.buffer), "vkCreateBuffer")) {
        return false;
    }

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk.device, entry.buffer, &requirements);
    const VkMemoryPropertyFlags properties = device_local ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!VK_DEVICE::allocate_memory(vk, requirements, properties, entry.memory, "buffer")) {
        vkDestroyBuffer(vk.device, entry.buffer, nullptr);
        return false;
    }
    if (!vk_check(vkBindBufferMemory(vk.device, entry.buffer, entry.memory, 0), "vkBindBufferMemory")) {
        destroy_buffer_entry(vk, entry);
        return false;
    }
    if (!device_local) {
        if (!vk_check(vkMapMemory(vk.device, entry.memory, 0, VK_WHOLE_SIZE, 0, &entry.mapped), "vkMapMemory")) {
            entry.mapped = nullptr;
            destroy_buffer_entry(vk, entry);
            return false;
        }
    }
    out = entry;
    return true;
}

static bool upload_to_entry(VulkanContext& vk, VkBufferEntry& entry, const void* data, const u64 size, const u64 offset) {
    if (offset > entry.size || size > entry.size - offset) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] upload_buffer: %llu bytes at %llu do not fit in %llu", static_cast<unsigned long long>(size),
            static_cast<unsigned long long>(offset), static_cast<unsigned long long>(entry.size));
        return false;
    }
    if (size == 0) {
        return true;
    }
    if (entry.memory_kind == GPU_MEMORY_HOST_VISIBLE) {
        // Coherent memory: visible to the device at the next submission.
        memcpy(static_cast<u8*>(entry.mapped) + offset, data, static_cast<usz>(size));
        return true;
    }

    GpuBufferDesc staging_desc;
    staging_desc.size = size;
    staging_desc.usage = GPU_BUFFER_USAGE_TRANSFER_SRC;
    staging_desc.memory = GPU_MEMORY_HOST_VISIBLE;
    VkBufferEntry staging;
    if (!create_buffer_entry(vk, staging_desc, staging)) {
        return false;
    }
    memcpy(staging.mapped, data, static_cast<usz>(size));

    bool ok = begin_upload(vk);
    if (ok) {
        VkBufferCopy region{};
        region.dstOffset = offset;
        region.size = size;
        vkCmdCopyBuffer(vk.upload_cmd, staging.buffer, entry.buffer, 1, &region);

        // Make the copy visible to whatever reads the buffer later.
        VkBufferMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = entry.buffer;
        barrier.offset = offset;
        barrier.size = size;
        vkCmdPipelineBarrier(vk.upload_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
        ok = end_upload(vk);
    }
    // end_upload waited for the copy, so the staging buffer can go.
    destroy_buffer_entry(vk, staging);
    return ok;
}

static GpuBuffer vk_create_buffer(GpuContext* gpu, const GpuBufferDesc& desc, const void* data) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    if (desc.size == 0) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] create_buffer: size must be non-zero");
        return GpuBuffer{};
    }
    VkBufferEntry entry;
    if (!create_buffer_entry(vk, desc, entry)) {
        return GpuBuffer{};
    }
    if (data != nullptr && !upload_to_entry(vk, entry, data, desc.size, 0)) {
        destroy_buffer_entry(vk, entry);
        return GpuBuffer{};
    }
    GpuBuffer buffer;
    buffer.id = vk.buffers.new_element();
    *vk.buffers.get_element_alive(buffer.id) = entry;
    buffer.size = desc.size;
    buffer.usage = desc.usage;
    buffer.memory = desc.memory;
    return buffer;
}

static bool vk_write_buffer(GpuContext* gpu, const GpuBuffer buffer, const void* data, const u64 size, const u64 offset) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    VkBufferEntry* entry = VK_CONTEXT::buffer(vk, buffer.id);
    if (entry == nullptr) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] write_buffer: invalid buffer");
        return false;
    }
    if (entry->memory_kind != GPU_MEMORY_HOST_VISIBLE) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] write_buffer: the buffer is not host-visible; use upload_buffer");
        return false;
    }
    return upload_to_entry(vk, *entry, data, size, offset);
}

static bool vk_upload_buffer(GpuContext* gpu, const GpuBuffer buffer, const void* data, const u64 size, const u64 offset) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    VkBufferEntry* entry = VK_CONTEXT::buffer(vk, buffer.id);
    if (entry == nullptr) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] upload_buffer: invalid buffer");
        return false;
    }
    return upload_to_entry(vk, *entry, data, size, offset);
}

static void vk_destroy_buffer(GpuContext* gpu, const GpuBuffer buffer) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    VkBufferEntry* entry = VK_CONTEXT::buffer(vk, buffer.id);
    if (entry == nullptr) {
        return;
    }
    destroy_buffer_entry(vk, *entry);
    vk.buffers.delete_element(buffer.id);
}

static void vk_release_buffer(GpuContext* gpu, const GpuBuffer buffer) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    VkBufferEntry* entry = VK_CONTEXT::buffer(vk, buffer.id);
    if (entry == nullptr) {
        return;
    }
    VkPending pending;
    pending.buffer = entry->buffer;
    pending.memory = entry->memory;
    VK_FRAME::release(vk, pending);
    vk.buffers.delete_element(buffer.id);
}

// --- Textures ------------------------------------------------------------------------

static GpuTexture vk_create_texture(GpuContext* gpu, const GpuTextureDesc& desc) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    if (desc.width == 0 || desc.height == 0) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] create_texture: extent must be non-zero");
        return GpuTexture{};
    }
    const VkFormat vk_format = VK_FORMATS::to_vk(desc.format);
    if (vk_format == VK_FORMAT_UNDEFINED) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] create_texture: format must be set");
        return GpuTexture{};
    }
    if (desc.mip_levels == 0) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] create_texture: mip_levels must be at least 1");
        return GpuTexture{};
    }

    VkTextureEntry entry;
    entry.vk_format = vk_format;
    entry.format = desc.format;
    entry.width = desc.width;
    entry.height = desc.height;
    entry.mip_levels = desc.mip_levels;
    entry.usage = desc.usage;
    entry.aspect = VK_FORMATS::aspect(desc.format);
    entry.state = GPU_STATE_UNDEFINED;

    const VkFormat sampled_vk_format = desc.sampled_format != GPU_FORMAT_UNDEFINED ? VK_FORMATS::to_vk(desc.sampled_format) : vk_format;
    if (sampled_vk_format == VK_FORMAT_UNDEFINED || GPU_FORMAT::size(desc.sampled_format != GPU_FORMAT_UNDEFINED ? desc.sampled_format : desc.format) != GPU_FORMAT::size(desc.format)) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] create_texture: sampled_format must be a format of the same size");
        return GpuTexture{};
    }

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.flags = sampled_vk_format != vk_format ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = vk_format;
    image_info.extent = {desc.width, desc.height, 1};
    image_info.mipLevels = desc.mip_levels;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_FORMATS::texture_usage(desc.usage) | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!vk_check(vkCreateImage(vk.device, &image_info, nullptr, &entry.image), "vkCreateImage")) {
        return GpuTexture{};
    }

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(vk.device, entry.image, &requirements);
    if (!VK_DEVICE::allocate_memory(vk, requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, entry.memory, "texture")) {
        vkDestroyImage(vk.device, entry.image, nullptr);
        return GpuTexture{};
    }
    if (!vk_check(vkBindImageMemory(vk.device, entry.image, entry.memory, 0), "vkBindImageMemory")) {
        destroy_texture_entry(vk, entry);
        return GpuTexture{};
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = entry.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = vk_format;
    view_info.subresourceRange.aspectMask = entry.aspect;
    view_info.subresourceRange.levelCount = desc.mip_levels;
    view_info.subresourceRange.layerCount = 1;
    if (!vk_check(vkCreateImageView(vk.device, &view_info, nullptr, &entry.view), "vkCreateImageView")) {
        entry.view = VK_NULL_HANDLE;
        destroy_texture_entry(vk, entry);
        return GpuTexture{};
    }
    entry.sampled_view = entry.view;
    if (sampled_vk_format != vk_format) {
        view_info.format = sampled_vk_format;
        if (!vk_check(vkCreateImageView(vk.device, &view_info, nullptr, &entry.sampled_view), "vkCreateImageView (sampled)")) {
            entry.sampled_view = entry.view;
            destroy_texture_entry(vk, entry);
            return GpuTexture{};
        }
    }

    GpuTexture texture;
    texture.id = vk.textures.new_element();
    *vk.textures.get_element_alive(texture.id) = entry;
    texture.format = desc.format;
    texture.width = desc.width;
    texture.height = desc.height;
    texture.mip_levels = desc.mip_levels;
    texture.usage = desc.usage;
    return texture;
}

static bool vk_upload_texture(GpuContext* gpu, const GpuTexture texture, const GpuTextureUpload* mips, const u32 count) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    VkTextureEntry* entry = VK_CONTEXT::texture(vk, texture.id);
    if (entry == nullptr) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] upload_texture: invalid texture");
        return false;
    }
    if (count == 0) {
        return true;
    }

    // Lay the levels out in one staging buffer.
    VkDeviceSize total = 0;
    for (u32 i = 0; i < count; ++i) {
        if (mips[i].mip_level >= entry->mip_levels) {
            GPU::log(GPU::LOG_ERROR, "[vulkan] upload_texture: mip %u out of range (texture has %u)", mips[i].mip_level, entry->mip_levels);
            return false;
        }
        if (mips[i].size == 0 || mips[i].pixels == nullptr) {
            GPU::log(GPU::LOG_ERROR, "[vulkan] upload_texture: mip %u has no data", mips[i].mip_level);
            return false;
        }
        total = align_up(total, STAGING_ALIGNMENT) + mips[i].size;
    }

    GpuBufferDesc staging_desc;
    staging_desc.size = total;
    staging_desc.usage = GPU_BUFFER_USAGE_TRANSFER_SRC;
    staging_desc.memory = GPU_MEMORY_HOST_VISIBLE;
    VkBufferEntry staging;
    if (!create_buffer_entry(vk, staging_desc, staging)) {
        return false;
    }

    TemporalAllocator temp = TemporalAllocator::create();
    DynamicArray<VkBufferImageCopy> regions(&temp);
    regions.reserve(count);
    VkDeviceSize cursor = 0;
    for (u32 i = 0; i < count; ++i) {
        cursor = align_up(cursor, STAGING_ALIGNMENT);
        memcpy(static_cast<u8*>(staging.mapped) + cursor, mips[i].pixels, static_cast<usz>(mips[i].size));
        VkBufferImageCopy region{};
        region.bufferOffset = cursor;
        region.imageSubresource.aspectMask = entry->aspect;
        region.imageSubresource.mipLevel = mips[i].mip_level;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {mip_extent(entry->width, mips[i].mip_level), mip_extent(entry->height, mips[i].mip_level), 1};
        regions.push(region);
        cursor += mips[i].size;
    }

    bool ok = begin_upload(vk);
    if (ok) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = entry->image;
        barrier.subresourceRange.aspectMask = entry->aspect;
        barrier.subresourceRange.levelCount = entry->mip_levels;
        barrier.subresourceRange.layerCount = 1;

        // Whole image -> TRANSFER_DST, waiting for whatever last used it.
        const VK_FORMATS::StateInfo from = VK_FORMATS::state_info(entry->state, true);
        barrier.oldLayout = from.layout;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcAccessMask = from.access;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(vk.upload_cmd, from.stage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkCmdCopyBufferToImage(vk.upload_cmd, staging.buffer, entry->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<u32>(regions.count), regions.data);

        // Whole image -> SAMPLED, visible to shaders in later submissions.
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(vk.upload_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        ok = end_upload(vk);
    }
    regions.free();
    destroy_buffer_entry(vk, staging);
    if (ok) {
        entry->state = GPU_STATE_SAMPLED;
    }
    return ok;
}

void VK_RESOURCES::forget_texture_framebuffers(VulkanContext& vk, const SparseId texture) {
    TemporalAllocator temp = TemporalAllocator::create();
    DynamicArray<u64> stale(&temp);
    for (const auto& entry : vk.framebuffers) {
        for (u32 i = 0; i < entry.value.texture_count; ++i) {
            if (entry.value.textures[i] == texture) {
                stale.push(entry.key);
                break;
            }
        }
    }
    for (const u64 key : stale) {
        VkFramebufferEntry* entry = vk.framebuffers.find(key);
        VkPending pending;
        pending.framebuffer = entry->framebuffer;
        VK_FRAME::release(vk, pending);
        vk.framebuffers.remove(key);
    }
    stale.free();
}

static void vk_destroy_texture(GpuContext* gpu, const GpuTexture texture) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    VkTextureEntry* entry = VK_CONTEXT::texture(vk, texture.id);
    if (entry == nullptr) {
        return;
    }
    if (entry->external) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] destroy_texture: the backbuffer belongs to the swapchain");
        return;
    }
    // The framebuffers are queued rather than destroyed, which is still
    // right: the caller guarantees an idle GPU and they go at the latest in
    // shutdown.
    VK_RESOURCES::forget_texture_framebuffers(vk, texture.id);
    destroy_texture_entry(vk, *entry);
    vk.textures.delete_element(texture.id);
}

static void vk_release_texture(GpuContext* gpu, const GpuTexture texture) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    VkTextureEntry* entry = VK_CONTEXT::texture(vk, texture.id);
    if (entry == nullptr) {
        return;
    }
    if (entry->external) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] release_texture: the backbuffer belongs to the swapchain");
        return;
    }
    VK_RESOURCES::forget_texture_framebuffers(vk, texture.id);
    VkPending pending;
    pending.view = entry->view;
    pending.sampled_view = entry->sampled_view != entry->view ? entry->sampled_view : VK_NULL_HANDLE;
    pending.image = entry->image;
    pending.memory = entry->memory;
    VK_FRAME::release(vk, pending);
    vk.textures.delete_element(texture.id);
}

SparseId VK_RESOURCES::register_external_texture(VulkanContext& vk, const VkImage image, const VkImageView view, const VkFormat format, const u32 width, const u32 height) {
    const SparseId id = vk.textures.new_element();
    VkTextureEntry& entry = *vk.textures.get_element_alive(id);
    entry.image = image;
    entry.view = view;
    entry.sampled_view = view;
    entry.vk_format = format;
    entry.format = VK_FORMATS::from_vk(format);
    entry.width = width;
    entry.height = height;
    entry.mip_levels = 1;
    entry.usage = GPU_TEXTURE_USAGE_COLOR_ATTACHMENT | GPU_TEXTURE_USAGE_TRANSFER_DST | GPU_TEXTURE_USAGE_TRANSFER_SRC;
    entry.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    entry.state = GPU_STATE_UNDEFINED;
    entry.external = true;
    return id;
}

void VK_RESOURCES::unregister_external_texture(VulkanContext& vk, const SparseId id) {
    VkTextureEntry* entry = VK_CONTEXT::texture(vk, id);
    if (entry == nullptr) {
        return;
    }
    // Called with the device idle (swapchain recreate / shutdown): the
    // framebuffers can go right away.
    TemporalAllocator temp = TemporalAllocator::create();
    DynamicArray<u64> stale(&temp);
    for (const auto& fb : vk.framebuffers) {
        for (u32 i = 0; i < fb.value.texture_count; ++i) {
            if (fb.value.textures[i] == id) {
                stale.push(fb.key);
                break;
            }
        }
    }
    for (const u64 key : stale) {
        vkDestroyFramebuffer(vk.device, vk.framebuffers.find(key)->framebuffer, nullptr);
        vk.framebuffers.remove(key);
    }
    stale.free();
    if (entry->view != VK_NULL_HANDLE) {
        vkDestroyImageView(vk.device, entry->view, nullptr);
    }
    vk.textures.delete_element(id);
}

namespace VK_RESOURCES_TABLE {
void fill(GpuBackend& table) {
    table.create_buffer = vk_create_buffer;
    table.write_buffer = vk_write_buffer;
    table.upload_buffer = vk_upload_buffer;
    table.destroy_buffer = vk_destroy_buffer;
    table.release_buffer = vk_release_buffer;
    table.create_texture = vk_create_texture;
    table.upload_texture = vk_upload_texture;
    table.destroy_texture = vk_destroy_texture;
    table.release_texture = vk_release_texture;
}
} // namespace VK_RESOURCES_TABLE
