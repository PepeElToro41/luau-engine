#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/device.hpp"
#include "engine/gpu/render_target.hpp"
#include "engine/templates/dynamic_array.hpp"

#include <volk.h>

// Buffers and textures, their device memory, and when it is safe to free them.
//
//     GpuResourceManager resources;
//     resources.init(&gpu);
//
//     GpuBuffer vertices;
//     resources.create_buffer({.size = bytes, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT}, data, bytes, vertices);
//
//     GpuTexture albedo;
//     resources.create_texture({.format = VK_FORMAT_R8G8B8A8_SRGB, .width = w, .height = h, .mip_levels = mips}, albedo);
//     GpuTextureUpload uploads[] = {{mip0, mip0_size, 0}, {mip1, mip1_size, 1}};
//     resources.upload_texture(albedo, uploads, 2);
//
//     // every frame, once the slot's fence has been waited on:
//     resources.begin_frame(frame.slot);
//     ...
//     resources.release_buffer(vertices);   // the GPU may still be reading it; freed later
//
// Two ways to get rid of a resource:
//
//   destroy_*  frees it right now. Only when the caller knows the GPU is done
//              with it (never submitted, or after GpuDevice::wait_idle).
//   release_*  queues it on the current frame slot and frees it the next time
//              that slot comes around, after FrameScheduler has waited on the
//              slot's fence. Any command buffer that could reference the
//              resource was submitted before the release, so by then it has
//              finished. This is the normal path; it never stalls.
//
// begin_frame(slot) is the retire point: it frees what was released the last
// time `slot` was current, then makes `slot` current. The Engine calls it at
// the top of render() with FrameContext::slot, which is the slot whose fence
// begin() just waited on. Releases made outside a frame (between end() and
// the next begin()) go on the last slot and are freed one full ring later,
// which is still after every submission that could have used them.
//
// Uploads are synchronous: a staging buffer, one command buffer on the
// graphics queue, and a fence wait. Good enough for load time; a streaming
// path with its own transfer queue would plug in beside it later.
//
// Both handle structs are plain values owned by whoever holds them: copy them
// freely, but hand exactly one copy to destroy_* / release_*, which null it.
// There is no destructor; shutdown() frees everything still pending and the
// upload objects. The caller guarantees the GPU is idle before shutdown().

// Where a buffer's memory lives.
enum GpuMemoryKind : u32 {
    // VRAM. Filled through a staging copy by upload_buffer(); vertex, index
    // and static uniform data.
    GPU_MEMORY_DEVICE_LOCAL = 0,
    // Host visible and coherent, persistently mapped (GpuBuffer::mapped).
    // Per-frame uniforms and anything the CPU rewrites often.
    GPU_MEMORY_HOST_VISIBLE = 1,
};

struct GpuBufferDesc {
    VkDeviceSize size = 0;
    // VK_BUFFER_USAGE_* bits. TRANSFER_DST is added for device-local buffers
    // so upload_buffer() can fill them.
    VkBufferUsageFlags usage = 0;
    GpuMemoryKind memory = GPU_MEMORY_DEVICE_LOCAL;
};

struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    GpuMemoryKind memory_kind = GPU_MEMORY_DEVICE_LOCAL;
    // Persistent mapping for host-visible buffers, nullptr otherwise.
    void* mapped = nullptr;

    bool is_valid() const { return this->buffer != VK_NULL_HANDLE; }
};

struct GpuTextureDesc {
    VkFormat format = VK_FORMAT_UNDEFINED;
    u32 width = 0;
    u32 height = 0;
    u32 mip_levels = 1;
    // VK_IMAGE_USAGE_* bits. TRANSFER_DST is added so upload_texture() works;
    // add COLOR_ATTACHMENT / DEPTH_STENCIL_ATTACHMENT for render targets.
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
};

struct GpuTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    // View over every mip level, in `format`.
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {0, 0};
    u32 mip_levels = 0;
    // What the image is in right now. UNDEFINED after create_texture(),
    // SHADER_READ_ONLY_OPTIMAL after upload_texture().
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;

    bool is_valid() const { return this->image != VK_NULL_HANDLE; }
};

// One mip level's worth of texels for upload_texture(). `size` is the tightly
// packed byte count of that level (TEXTURE_ASSET::layer_size for assets).
struct GpuTextureUpload {
    const void* pixels = nullptr;
    VkDeviceSize size = 0;
    u32 mip_level = 0;
};

struct GpuResourceManager {
    bool init(GpuDevice* gpu);
    // Frees every pending resource and the upload objects. The GPU must be
    // idle. Resources still held by callers are theirs to destroy first.
    void shutdown();

    // --- Buffers -----------------------------------------------------------

    // Creates and binds memory for `desc`. Host-visible buffers come back
    // mapped. False (and `out` untouched) on failure.
    bool create_buffer(const GpuBufferDesc& desc, GpuBuffer& out);
    // create_buffer followed by upload_buffer of `size` bytes from `data`.
    bool create_buffer(const GpuBufferDesc& desc, const void* data, VkDeviceSize size, GpuBuffer& out);
    // Writes `size` bytes at `offset`. Host-visible buffers are written
    // through the mapping; device-local ones through a staging copy that
    // blocks until the copy has finished. The range must fit in the buffer.
    bool upload_buffer(GpuBuffer& buffer, const void* data, VkDeviceSize size, VkDeviceSize offset = 0);
    // Frees the buffer now. The GPU must be done with it.
    void destroy_buffer(GpuBuffer& buffer);
    // Frees the buffer once the current frame slot has completed.
    void release_buffer(GpuBuffer& buffer);

    // --- Textures ----------------------------------------------------------

    // Creates a 2D image with `desc.mip_levels` levels, its memory and a view
    // over every level. The image is in UNDEFINED layout: upload_texture()
    // fills it, or a render pass transitions it. False on failure.
    bool create_texture(const GpuTextureDesc& desc, GpuTexture& out);
    // create_texture followed by upload_texture of mip 0.
    bool create_texture(const GpuTextureDesc& desc, const void* pixels, VkDeviceSize size, GpuTexture& out);
    // Copies `count` mip levels into the image through one staging buffer and
    // leaves every level in SHADER_READ_ONLY_OPTIMAL, including levels not
    // uploaded. Blocks until the copies have finished. Each `mip_level` must
    // be below `texture.mip_levels`.
    bool upload_texture(GpuTexture& texture, const GpuTextureUpload* uploads, u32 count);
    // upload_texture with the single level `mip_level`.
    bool upload_texture(GpuTexture& texture, const void* pixels, VkDeviceSize size, u32 mip_level = 0);
    // Frees the texture now. The GPU must be done with it.
    void destroy_texture(GpuTexture& texture);
    // Frees the texture once the current frame slot has completed.
    void release_texture(GpuTexture& texture);

    // --- Frames ------------------------------------------------------------

    // Frees what was released the last time `slot` was current and makes
    // `slot` current. Call once per frame, after the slot's fence has been
    // waited on (FrameScheduler::begin does that), before recording.
    void begin_frame(u32 slot);
    // Frees everything pending on every slot. The GPU must be idle.
    void flush();
    // Resources waiting to be freed, over every slot.
    usz pending_count() const;

    GpuDevice* gpu = nullptr;
    u32 current_slot = 0;

private:
    // Everything destroy_buffer / destroy_texture need, in one record. A
    // buffer leaves `image` and `view` null; a texture leaves `buffer` null.
    struct Pending {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    void destroy(const Pending& pending);
    bool allocate_memory(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags properties, VkDeviceMemory& out, const char* what);
    // The upload command buffer, recording. Nothing else may be recording.
    bool begin_upload();
    // Ends, submits and waits for the upload command buffer.
    bool end_upload();

    DynamicArray<Pending> pending[FRAMES_IN_FLIGHT];
    VkCommandPool upload_pool = VK_NULL_HANDLE;
    VkCommandBuffer upload_cmd = VK_NULL_HANDLE;
    VkFence upload_fence = VK_NULL_HANDLE;
};
