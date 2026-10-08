#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/device.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "engine/templates/hash_map.hpp"

#include <volk.h>

// Descriptor sets, in four small pieces:
//
//   DescriptorLayoutCache   bindings -> VkDescriptorSetLayout, deduplicated,
//                           so every shader with the same interface shares
//                           one layout (and pipelines stay compatible)
//   DescriptorAllocator     a growing list of pools to allocate sets from;
//                           either long-lived sets freed one by one, or
//                           transient sets thrown away by reset() per frame
//   DescriptorWriter        a batch of writes flushed with one
//                           vkUpdateDescriptorSets
//   SamplerCache            SamplerDesc -> VkSampler, deduplicated
//
// Updating a descriptor set that a submitted, unfinished command buffer may
// still read is invalid: write sets only before recording the frame that
// binds them (the renderer does it in begin_frame, on the frame slot whose
// fence was just waited on).

// --- Layouts ----------------------------------------------------------------

static constexpr u32 DESCRIPTOR_LAYOUT_MAX_BINDINGS = 24;

struct DescriptorLayoutDesc {
    VkDescriptorSetLayoutBinding bindings[DESCRIPTOR_LAYOUT_MAX_BINDINGS] = {};
    u32 count = 0;

    // False when full. Bindings may be added in any order.
    bool add(u32 binding, VkDescriptorType type, u32 descriptor_count, VkShaderStageFlags stages);
    // Over the bindings sorted by index, so the order of add() calls does
    // not matter.
    u64 hash() const;
};

struct DescriptorLayoutCache {
    bool init(GpuDevice* gpu);
    // Destroys every layout; pipelines and sets built on them must be gone.
    void shutdown();

    // The layout for `desc`, created on first request. VK_NULL_HANDLE if
    // Vulkan refuses it.
    VkDescriptorSetLayout get(const DescriptorLayoutDesc& desc);
    // The layout with no bindings, for set indices a pipeline layout must
    // cover but the shader does not use.
    VkDescriptorSetLayout empty_layout();

    GpuDevice* gpu = nullptr;
    HashMap<u64, VkDescriptorSetLayout> layouts;
};

// --- Allocation -------------------------------------------------------------

struct DescriptorSetHandle {
    VkDescriptorSet set = VK_NULL_HANDLE;
    // Index of the pool it came from, for free().
    u32 pool = 0xffffffffu;

    bool is_valid() const { 
    	return this->set != VK_NULL_HANDLE;
    }
};

static constexpr u32 DESCRIPTOR_ALLOCATOR_MAX_SIZES = 8;

struct DescriptorAllocator {
    // `sizes` and `max_sets` are the budget of each pool; a fresh pool with
    // the same budget is added when allocation fails. Pass
    // VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT in `flags` for sets
    // released one by one with free(); leave it out for sets dropped
    // together with reset().
    bool init(GpuDevice* gpu, const VkDescriptorPoolSize* sizes, u32 size_count, u32 max_sets, VkDescriptorPoolCreateFlags flags);
    // Destroys every pool and with it every set. The GPU must be idle.
    void shutdown();

    DescriptorSetHandle allocate(VkDescriptorSetLayout layout);
    // Returns the set to its pool. Only with the FREE flag.
    void free(DescriptorSetHandle handle);
    // Returns every set of every pool at once. Nothing allocated from this
    // allocator may still be in use.
    void reset();

    GpuDevice* gpu = nullptr;
    DynamicArray<VkDescriptorPool> pools;
    VkDescriptorPoolSize sizes[DESCRIPTOR_ALLOCATOR_MAX_SIZES] = {};
    u32 size_count = 0;
    u32 max_sets = 0;
    VkDescriptorPoolCreateFlags flags = 0;
    // The pool allocate() tries first.
    u32 current = 0;

private:
    VkDescriptorPool create_pool();
};

// --- Writing ----------------------------------------------------------------

struct DescriptorWriter {
    static constexpr u32 MAX_WRITES = 16;

    void write_buffer(VkDescriptorSet set, u32 binding, VkDescriptorType type, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range);
    void write_image(VkDescriptorSet set, u32 binding, VkDescriptorType type, VkImageView view, VkSampler sampler, VkImageLayout layout);
    // Flushes the writes and empties the writer. Do not copy a writer
    // between a write and its update: the writes point into it.
    void update(GpuDevice* gpu);

    VkWriteDescriptorSet writes[MAX_WRITES] = {};
    VkDescriptorBufferInfo buffers[MAX_WRITES] = {};
    VkDescriptorImageInfo images[MAX_WRITES] = {};
    u32 count = 0;
};

// --- Samplers ---------------------------------------------------------------

struct SamplerDesc {
    VkFilter mag_filter = VK_FILTER_LINEAR;
    VkFilter min_filter = VK_FILTER_LINEAR;
    VkSamplerMipmapMode mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VkSamplerAddressMode address_u = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode address_v = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode address_w = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    // 0 disables anisotropy. Stays 0 until the device enables the feature.
    f32 max_anisotropy = 0.0f;

    u64 hash() const;
    bool equals(const SamplerDesc& other) const;
    bool operator==(const SamplerDesc& other) const { return this->equals(other); }
    bool operator!=(const SamplerDesc& other) const { return !this->equals(other); }
};

// Lets SamplerDesc key a HashMap with the default Hash / Equal parameters.
template <>
struct std::hash<SamplerDesc> {
    size_t operator()(const SamplerDesc& desc) const { return static_cast<size_t>(desc.hash()); }
};

struct SamplerCache {
    bool init(GpuDevice* gpu);
    void shutdown();

    // The sampler for `desc`, created on first request. VK_NULL_HANDLE if
    // Vulkan refuses it.
    VkSampler get(const SamplerDesc& desc);

    GpuDevice* gpu = nullptr;
    // Keyed by the full description, so a hash collision cannot alias two samplers.
    HashMap<SamplerDesc, VkSampler> samplers;
};
