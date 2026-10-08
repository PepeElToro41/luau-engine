#include "engine/gpu/resource_manager.hpp"

#include "engine/gpu/format_utils.hpp"

#include "engine/memory/temporal_allocator.hpp"
#include "gpu/vk_check.hpp"

#include <cstdio>
#include <cstring>

// Staging offsets for image copies must be a multiple of 4 and of the texel
// block size; 16 covers every uncompressed format and every block-compressed
// one.
static constexpr VkDeviceSize STAGING_ALIGNMENT = 16;

static VkDeviceSize align_up(const VkDeviceSize value, const VkDeviceSize alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static VkImageAspectFlags aspect_for_format(const VkFormat format) {
    return GPU_FORMAT::aspect(format);
}

static u32 mip_extent(const u32 base, const u32 level) {
    const u32 shifted = base >> level;
    return shifted == 0 ? 1 : shifted;
}

// --- Lifetime -------------------------------------------------------------------

bool GpuResourceManager::init(GpuDevice* gpu) {
    this->gpu = gpu;
    this->current_slot = 0;
    VkDevice device = gpu->device;

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = gpu->graphics_queue_family;
    if (!vk_check(vkCreateCommandPool(device, &pool_info, nullptr, &this->upload_pool), "vkCreateCommandPool (upload)")) {
        this->upload_pool = VK_NULL_HANDLE;
        return false;
    }

    VkCommandBufferAllocateInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_info.commandPool = this->upload_pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    if (!vk_check(vkAllocateCommandBuffers(device, &cmd_info, &this->upload_cmd), "vkAllocateCommandBuffers (upload)")) {
        this->upload_cmd = VK_NULL_HANDLE;
        return false;
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (!vk_check(vkCreateFence(device, &fence_info, nullptr, &this->upload_fence), "vkCreateFence (upload)")) {
        this->upload_fence = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void GpuResourceManager::shutdown() {
    if (this->gpu == nullptr) {
        return;
    }
    this->flush();
    for (DynamicArray<Pending>& list : this->pending) {
        list.free();
    }

    VkDevice device = this->gpu->device;
    if (this->upload_fence != VK_NULL_HANDLE) {
        vkDestroyFence(device, this->upload_fence, nullptr);
        this->upload_fence = VK_NULL_HANDLE;
    }
    if (this->upload_pool != VK_NULL_HANDLE) {
        // Frees upload_cmd with it.
        vkDestroyCommandPool(device, this->upload_pool, nullptr);
        this->upload_pool = VK_NULL_HANDLE;
    }
    this->upload_cmd = VK_NULL_HANDLE;
    this->gpu = nullptr;
}

// --- Memory ---------------------------------------------------------------------

bool GpuResourceManager::allocate_memory(const VkMemoryRequirements& requirements, const VkMemoryPropertyFlags properties, VkDeviceMemory& out, const char* what) {
    const u32 memory_type = this->gpu->find_memory_type(requirements.memoryTypeBits, properties);
    if (memory_type == UINT32_MAX) {
        fprintf(stderr, "[vulkan] no memory type with properties 0x%x for %s\n", static_cast<unsigned>(properties), what);
        return false;
    }

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = requirements.size;
    alloc_info.memoryTypeIndex = memory_type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (!vk_check(vkAllocateMemory(this->gpu->device, &alloc_info, nullptr, &memory), "vkAllocateMemory")) {
        return false;
    }
    out = memory;
    return true;
}

// --- Buffers --------------------------------------------------------------------

bool GpuResourceManager::create_buffer(const GpuBufferDesc& desc, GpuBuffer& out) {
    if (desc.size == 0) {
        fprintf(stderr, "[vulkan] create_buffer: size must be non-zero\n");
        return false;
    }
    VkDevice device = this->gpu->device;
    const bool device_local = desc.memory == GPU_MEMORY_DEVICE_LOCAL;

    GpuBuffer buffer;
    buffer.size = desc.size;
    buffer.memory_kind = desc.memory;

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = desc.size;
    buffer_info.usage = desc.usage | (device_local ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : 0);
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!vk_check(vkCreateBuffer(device, &buffer_info, nullptr, &buffer.buffer), "vkCreateBuffer")) {
        return false;
    }

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);
    const VkMemoryPropertyFlags properties = device_local
        ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        : (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!this->allocate_memory(requirements, properties, buffer.memory, "buffer")) {
        vkDestroyBuffer(device, buffer.buffer, nullptr);
        return false;
    }
    if (!vk_check(vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0), "vkBindBufferMemory")) {
        this->destroy_buffer(buffer);
        return false;
    }

    if (!device_local) {
        if (!vk_check(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped), "vkMapMemory")) {
            buffer.mapped = nullptr;
            this->destroy_buffer(buffer);
            return false;
        }
    }

    out = buffer;
    return true;
}

bool GpuResourceManager::create_buffer(const GpuBufferDesc& desc, const void* data, const VkDeviceSize size, GpuBuffer& out) {
    GpuBuffer buffer;
    if (!this->create_buffer(desc, buffer)) {
        return false;
    }
    if (!this->upload_buffer(buffer, data, size, 0)) {
        this->destroy_buffer(buffer);
        return false;
    }
    out = buffer;
    return true;
}

bool GpuResourceManager::upload_buffer(GpuBuffer& buffer, const void* data, const VkDeviceSize size, const VkDeviceSize offset) {
    if (!buffer.is_valid()) {
        fprintf(stderr, "[vulkan] upload_buffer: invalid buffer\n");
        return false;
    }
    if (offset > buffer.size || size > buffer.size - offset) {
        fprintf(stderr, "[vulkan] upload_buffer: %llu bytes at %llu do not fit in %llu\n",
            static_cast<unsigned long long>(size), static_cast<unsigned long long>(offset), static_cast<unsigned long long>(buffer.size));
        return false;
    }
    if (size == 0) {
        return true;
    }

    if (buffer.memory_kind == GPU_MEMORY_HOST_VISIBLE) {
        // Coherent memory: the write is visible to the device at the next
        // submission without a flush.
        memcpy(static_cast<u8*>(buffer.mapped) + offset, data, static_cast<usz>(size));
        return true;
    }

    GpuBufferDesc staging_desc;
    staging_desc.size = size;
    staging_desc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    staging_desc.memory = GPU_MEMORY_HOST_VISIBLE;
    GpuBuffer staging;
    if (!this->create_buffer(staging_desc, staging)) {
        return false;
    }
    memcpy(staging.mapped, data, static_cast<usz>(size));

    bool ok = this->begin_upload();
    if (ok) {
        VkBufferCopy region{};
        region.srcOffset = 0;
        region.dstOffset = offset;
        region.size = size;
        vkCmdCopyBuffer(this->upload_cmd, staging.buffer, buffer.buffer, 1, &region);

        // Make the copy visible to whatever reads the buffer in later
        // submissions.
        VkBufferMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer.buffer;
        barrier.offset = offset;
        barrier.size = size;
        vkCmdPipelineBarrier(this->upload_cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 1, &barrier, 0, nullptr);

        ok = this->end_upload();
    }

    // end_upload waited for the copy, so the staging buffer is free to go.
    this->destroy_buffer(staging);
    return ok;
}

void GpuResourceManager::destroy_buffer(GpuBuffer& buffer) {
    Pending pending;
    pending.buffer = buffer.buffer;
    pending.memory = buffer.memory;
    this->destroy(pending);
    buffer = GpuBuffer{};
}

void GpuResourceManager::release_buffer(GpuBuffer& buffer) {
    if (buffer.is_valid()) {
        Pending pending;
        pending.buffer = buffer.buffer;
        pending.memory = buffer.memory;
        this->pending[this->current_slot].push(pending);
    }
    buffer = GpuBuffer{};
}

// --- Textures -------------------------------------------------------------------

bool GpuResourceManager::create_texture(const GpuTextureDesc& desc, GpuTexture& out) {
    if (desc.width == 0 || desc.height == 0) {
        fprintf(stderr, "[vulkan] create_texture: extent must be non-zero\n");
        return false;
    }
    if (desc.format == VK_FORMAT_UNDEFINED) {
        fprintf(stderr, "[vulkan] create_texture: format must be set\n");
        return false;
    }
    if (desc.mip_levels == 0) {
        fprintf(stderr, "[vulkan] create_texture: mip_levels must be at least 1\n");
        return false;
    }
    VkDevice device = this->gpu->device;

    GpuTexture texture;
    texture.format = desc.format;
    texture.extent = {desc.width, desc.height};
    texture.mip_levels = desc.mip_levels;
    texture.layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = desc.format;
    image_info.extent = {desc.width, desc.height, 1};
    image_info.mipLevels = desc.mip_levels;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = desc.usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!vk_check(vkCreateImage(device, &image_info, nullptr, &texture.image), "vkCreateImage")) {
        return false;
    }

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, texture.image, &requirements);
    if (!this->allocate_memory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, texture.memory, "texture")) {
        vkDestroyImage(device, texture.image, nullptr);
        return false;
    }
    if (!vk_check(vkBindImageMemory(device, texture.image, texture.memory, 0), "vkBindImageMemory")) {
        this->destroy_texture(texture);
        return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = texture.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = desc.format;
    view_info.subresourceRange.aspectMask = aspect_for_format(desc.format);
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = desc.mip_levels;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    if (!vk_check(vkCreateImageView(device, &view_info, nullptr, &texture.view), "vkCreateImageView")) {
        texture.view = VK_NULL_HANDLE;
        this->destroy_texture(texture);
        return false;
    }

    out = texture;
    return true;
}

bool GpuResourceManager::create_texture(const GpuTextureDesc& desc, const void* pixels, const VkDeviceSize size, GpuTexture& out) {
    GpuTexture texture;
    if (!this->create_texture(desc, texture)) {
        return false;
    }
    if (!this->upload_texture(texture, pixels, size, 0)) {
        this->destroy_texture(texture);
        return false;
    }
    out = texture;
    return true;
}

bool GpuResourceManager::upload_texture(GpuTexture& texture, const void* pixels, const VkDeviceSize size, const u32 mip_level) {
    GpuTextureUpload upload;
    upload.pixels = pixels;
    upload.size = size;
    upload.mip_level = mip_level;
    return this->upload_texture(texture, &upload, 1);
}

bool GpuResourceManager::upload_texture(GpuTexture& texture, const GpuTextureUpload* uploads, const u32 count) {
    if (!texture.is_valid()) {
        fprintf(stderr, "[vulkan] upload_texture: invalid texture\n");
        return false;
    }
    if (count == 0) {
        return true;
    }

    // Lay the levels out in one staging buffer.
    VkDeviceSize total = 0;
    for (u32 i = 0; i < count; ++i) {
        if (uploads[i].mip_level >= texture.mip_levels) {
            fprintf(stderr, "[vulkan] upload_texture: mip %u out of range (texture has %u)\n", uploads[i].mip_level, texture.mip_levels);
            return false;
        }
        if (uploads[i].size == 0 || uploads[i].pixels == nullptr) {
            fprintf(stderr, "[vulkan] upload_texture: mip %u has no data\n", uploads[i].mip_level);
            return false;
        }
        total = align_up(total, STAGING_ALIGNMENT) + uploads[i].size;
    }

    GpuBufferDesc staging_desc;
    staging_desc.size = total;
    staging_desc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    staging_desc.memory = GPU_MEMORY_HOST_VISIBLE;
    GpuBuffer staging;
    if (!this->create_buffer(staging_desc, staging)) {
        return false;
    }

    TemporalAllocator temp = TemporalAllocator::create();
    DynamicArray<VkBufferImageCopy> regions(&temp);
    regions.reserve(count);

    const VkImageAspectFlags aspect = aspect_for_format(texture.format);
    VkDeviceSize cursor = 0;
    for (u32 i = 0; i < count; ++i) {
        cursor = align_up(cursor, STAGING_ALIGNMENT);
        memcpy(static_cast<u8*>(staging.mapped) + cursor, uploads[i].pixels, static_cast<usz>(uploads[i].size));

        VkBufferImageCopy region{};
        region.bufferOffset = cursor;
        region.bufferRowLength = 0;   // tightly packed
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = aspect;
        region.imageSubresource.mipLevel = uploads[i].mip_level;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {mip_extent(texture.extent.width, uploads[i].mip_level), mip_extent(texture.extent.height, uploads[i].mip_level), 1};
        regions.push(region);

        cursor += uploads[i].size;
    }

    bool ok = this->begin_upload();
    if (ok) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = texture.image;
        barrier.subresourceRange.aspectMask = aspect;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = texture.mip_levels;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        // Whole image -> TRANSFER_DST. If it was already sampled from, wait
        // for those reads; fresh images have nothing to wait on.
        const bool fresh = texture.layout == VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.oldLayout = texture.layout;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcAccessMask = fresh ? 0 : VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(this->upload_cmd,
            fresh ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkCmdCopyBufferToImage(this->upload_cmd, staging.buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            static_cast<u32>(regions.count), regions.data);

        // Whole image -> SHADER_READ_ONLY, visible to shaders in later submissions.
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(this->upload_cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        ok = this->end_upload();
    }

    regions.free();
    this->destroy_buffer(staging);
    if (ok) {
        texture.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    return ok;
}

void GpuResourceManager::destroy_texture(GpuTexture& texture) {
    Pending pending;
    pending.image = texture.image;
    pending.view = texture.view;
    pending.memory = texture.memory;
    this->destroy(pending);
    texture = GpuTexture{};
}

void GpuResourceManager::release_texture(GpuTexture& texture) {
    if (texture.is_valid()) {
        Pending pending;
        pending.image = texture.image;
        pending.view = texture.view;
        pending.memory = texture.memory;
        this->pending[this->current_slot].push(pending);
    }
    texture = GpuTexture{};
}

// --- Pipelines ------------------------------------------------------------------

void GpuResourceManager::release_pipeline(const VkPipeline pipeline, const VkPipelineLayout layout) {
    if (pipeline == VK_NULL_HANDLE && layout == VK_NULL_HANDLE) {
        return;
    }
    Pending pending;
    pending.pipeline = pipeline;
    pending.pipeline_layout = layout;
    this->pending[this->current_slot].push(pending);
}

// --- Render passes and framebuffers -----------------------------------------------

void GpuResourceManager::release_render_pass(const VkRenderPass render_pass) {
    if (render_pass == VK_NULL_HANDLE) {
        return;
    }
    Pending pending;
    pending.render_pass = render_pass;
    this->pending[this->current_slot].push(pending);
}

void GpuResourceManager::release_framebuffer(const VkFramebuffer framebuffer) {
    if (framebuffer == VK_NULL_HANDLE) {
        return;
    }
    Pending pending;
    pending.framebuffer = framebuffer;
    this->pending[this->current_slot].push(pending);
}

// --- Frames ---------------------------------------------------------------------

void GpuResourceManager::begin_frame(const u32 slot) {
    ENGINE_ASSERT(slot < FRAMES_IN_FLIGHT, "begin_frame: slot %u out of range", slot);
    DynamicArray<Pending>& list = this->pending[slot];
    for (const Pending& pending : list) {
        this->destroy(pending);
    }
    list.clear();
    this->current_slot = slot;
}

void GpuResourceManager::flush() {
    for (DynamicArray<Pending>& list : this->pending) {
        for (const Pending& pending : list) {
            this->destroy(pending);
        }
        list.clear();
    }
}

usz GpuResourceManager::pending_count() const {
    usz total = 0;
    for (const DynamicArray<Pending>& list : this->pending) {
        total += list.count;
    }
    return total;
}

void GpuResourceManager::destroy(const Pending& pending) {
    VkDevice device = this->gpu->device;
    if (pending.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, pending.view, nullptr);
    }
    if (pending.image != VK_NULL_HANDLE) {
        vkDestroyImage(device, pending.image, nullptr);
    }
    if (pending.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, pending.buffer, nullptr);
    }
    // Mapped memory is unmapped implicitly by vkFreeMemory.
    if (pending.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, pending.memory, nullptr);
    }
    if (pending.pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pending.pipeline, nullptr);
    }
    if (pending.pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, pending.pipeline_layout, nullptr);
    }
    // A framebuffer may outlive its render pass handle, but destroying both
    // in one record is still fine in either order.
    if (pending.framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, pending.framebuffer, nullptr);
    }
    if (pending.render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, pending.render_pass, nullptr);
    }
}

// --- Uploads --------------------------------------------------------------------

bool GpuResourceManager::begin_upload() {
    if (!vk_check(vkResetCommandBuffer(this->upload_cmd, 0), "vkResetCommandBuffer (upload)")) {
        return false;
    }
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vk_check(vkBeginCommandBuffer(this->upload_cmd, &begin_info), "vkBeginCommandBuffer (upload)");
}

bool GpuResourceManager::end_upload() {
    if (!vk_check(vkEndCommandBuffer(this->upload_cmd), "vkEndCommandBuffer (upload)")) {
        return false;
    }

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &this->upload_cmd;
    if (!vk_check(vkQueueSubmit(this->gpu->graphics_queue, 1, &submit_info, this->upload_fence), "vkQueueSubmit (upload)")) {
        return false;
    }

    const bool ok = vk_check(vkWaitForFences(this->gpu->device, 1, &this->upload_fence, VK_TRUE, UINT64_MAX), "vkWaitForFences (upload)");
    vkResetFences(this->gpu->device, 1, &this->upload_fence);
    return ok;
}
