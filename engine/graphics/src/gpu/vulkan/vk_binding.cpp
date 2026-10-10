#include "engine/utils/hash.hpp"
#include "gpu/vulkan/vk_check.hpp"
#include "gpu/vulkan/vk_context.hpp"

#include <cstring>

// --- Descriptor pools ----------------------------------------------------------

static VkDescriptorPool create_pool(VulkanContext& vk, const VkDescriptorPools& pools) {
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.flags = pools.flags;
    info.maxSets = pools.max_sets;
    info.poolSizeCount = pools.size_count;
    info.pPoolSizes = pools.sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (!vk_check(vkCreateDescriptorPool(vk.device, &info, nullptr, &pool), "vkCreateDescriptorPool")) {
        return VK_NULL_HANDLE;
    }
    return pool;
}

bool VK_BINDING::pools_init(VulkanContext& vk, VkDescriptorPools& pools, const VkDescriptorPoolSize* sizes, const u32 size_count, const u32 max_sets, const VkDescriptorPoolCreateFlags flags) {
    if (size_count == 0 || size_count > VK_MAX_DESCRIPTOR_POOL_SIZES || max_sets == 0) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] descriptor pools need 1..%u sizes and a set budget", VK_MAX_DESCRIPTOR_POOL_SIZES);
        return false;
    }
    memcpy(pools.sizes, sizes, size_count * sizeof(VkDescriptorPoolSize));
    pools.size_count = size_count;
    pools.max_sets = max_sets;
    pools.flags = flags;
    pools.current = 0;
    const VkDescriptorPool first = create_pool(vk, pools);
    if (first == VK_NULL_HANDLE) {
        return false;
    }
    pools.pools.push(first);
    return true;
}

void VK_BINDING::pools_shutdown(VulkanContext& vk, VkDescriptorPools& pools) {
    for (const VkDescriptorPool pool : pools.pools) {
        vkDestroyDescriptorPool(vk.device, pool, nullptr);
    }
    pools.pools.free();
    pools.current = 0;
}

VkDescriptorSet VK_BINDING::pools_allocate(VulkanContext& vk, VkDescriptorPools& pools, const VkDescriptorSetLayout layout, u32* pool_index) {
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;

    // The current pool first, then every other one, then a new one.
    for (usz attempt = 0; attempt < pools.pools.count; ++attempt) {
        const u32 index = static_cast<u32>((pools.current + attempt) % pools.pools.count);
        info.descriptorPool = pools.pools[index];
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult result = vkAllocateDescriptorSets(vk.device, &info, &set);
        if (result == VK_SUCCESS) {
            pools.current = index;
            *pool_index = index;
            return set;
        }
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) {
            vk_check(result, "vkAllocateDescriptorSets");
            return VK_NULL_HANDLE;
        }
    }
    const VkDescriptorPool fresh = create_pool(vk, pools);
    if (fresh == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    pools.pools.push(fresh);
    pools.current = static_cast<u32>(pools.pools.count - 1);
    info.descriptorPool = fresh;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!vk_check(vkAllocateDescriptorSets(vk.device, &info, &set), "vkAllocateDescriptorSets (fresh pool)")) {
        return VK_NULL_HANDLE;
    }
    *pool_index = pools.current;
    return set;
}

void VK_BINDING::pools_reset(VulkanContext& vk, VkDescriptorPools& pools) {
    for (const VkDescriptorPool pool : pools.pools) {
        vkResetDescriptorPool(vk.device, pool, 0);
    }
    pools.current = 0;
}

// --- Lifetime -------------------------------------------------------------------

bool VK_BINDING::init(VulkanContext& vk) {
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 256},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64},
    };
    return pools_init(vk, vk.persistent_sets, sizes, 5, 512, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);
}

void VK_BINDING::shutdown(VulkanContext& vk) {
    for (usz i = 0; i < vk.samplers.alive_count; ++i) {
        const SparseId id = vk.samplers.get_alive_id(i);
        if (id != GPU_NULL_ID) {
            vkDestroySampler(vk.device, vk.samplers.get_element_alive(id)->sampler, nullptr);
        }
    }
    for (usz i = 0; i < vk.bind_layouts.alive_count; ++i) {
        const SparseId id = vk.bind_layouts.get_alive_id(i);
        if (id != GPU_NULL_ID) {
            vkDestroyDescriptorSetLayout(vk.device, vk.bind_layouts.get_element_alive(id)->layout, nullptr);
        }
    }
    vk.samplers.free();
    vk.bind_layouts.free();
    vk.bind_groups.free();
    vk.sampler_cache.free();
    vk.bind_layout_cache.free();
    // Destroying the pools frees every set still in them.
    pools_shutdown(vk, vk.persistent_sets);
}

void VK_BINDING::begin_slot(VulkanContext& vk, VkFrameSlot& slot) {
    pools_reset(vk, slot.transient_sets);
    for (const SparseId id : slot.transient_groups) {
        vk.bind_groups.delete_element(id);
    }
    slot.transient_groups.clear();
    slot.uniform_cursor = 0;
}

// --- Samplers --------------------------------------------------------------------

static VkFilter to_vk(const GpuFilter filter) {
    return filter == GPU_FILTER_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

static VkSamplerAddressMode to_vk(const GpuAddressMode mode) {
    switch (mode) {
    case GPU_ADDRESS_REPEAT:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case GPU_ADDRESS_MIRROR:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case GPU_ADDRESS_CLAMP:
        break;
    }
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

static GpuSampler vk_sampler(GpuContext* gpu, const GpuSamplerDesc& desc) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    const u64 key = desc.hash();
    GpuSampler sampler;
    if (const SparseId* found = vk.sampler_cache.find(key)) {
        sampler.id = *found;
        return sampler;
    }
    const bool anisotropic = desc.max_anisotropy > 0.0f && gpu->info.limits.max_anisotropy > 1;
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = to_vk(desc.mag_filter);
    info.minFilter = to_vk(desc.min_filter);
    info.mipmapMode = desc.mip_filter == GPU_FILTER_NEAREST ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = to_vk(desc.address_u);
    info.addressModeV = to_vk(desc.address_v);
    info.addressModeW = to_vk(desc.address_w);
    info.anisotropyEnable = anisotropic ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = anisotropic ? desc.max_anisotropy : 1.0f;
    if (info.maxAnisotropy > static_cast<f32>(gpu->info.limits.max_anisotropy)) {
        info.maxAnisotropy = static_cast<f32>(gpu->info.limits.max_anisotropy);
    }
    info.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler handle = VK_NULL_HANDLE;
    if (!vk_check(vkCreateSampler(vk.device, &info, nullptr, &handle), "vkCreateSampler")) {
        return sampler;
    }
    sampler.id = vk.samplers.new_element();
    vk.samplers.get_element_alive(sampler.id)->sampler = handle;
    vk.sampler_cache.insert(key, sampler.id);
    return sampler;
}

// --- Bind layouts -----------------------------------------------------------------

static GpuBindLayout vk_bind_layout(GpuContext* gpu, const GpuBindLayoutDesc& desc) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    const u64 key = desc.hash();
    GpuBindLayout layout;
    if (const SparseId* found = vk.bind_layout_cache.find(key)) {
        layout.id = *found;
        return layout;
    }
    VkDescriptorSetLayoutBinding bindings[GPU_MAX_BIND_SLOTS] = {};
    for (u32 i = 0; i < desc.count; ++i) {
        const GpuBindSlot& slot = desc.slots[i];
        bindings[i].binding = slot.slot;
        bindings[i].descriptorType = VK_FORMATS::to_vk(slot.type);
        bindings[i].descriptorCount = slot.count;
        bindings[i].stageFlags = VK_FORMATS::to_vk_stages(slot.stages);
    }
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = desc.count;
    info.pBindings = bindings;
    VkDescriptorSetLayout handle = VK_NULL_HANDLE;
    if (!vk_check(vkCreateDescriptorSetLayout(vk.device, &info, nullptr, &handle), "vkCreateDescriptorSetLayout")) {
        return layout;
    }
    layout.id = vk.bind_layouts.new_element();
    VkBindLayoutEntry& entry = *vk.bind_layouts.get_element_alive(layout.id);
    entry.layout = handle;
    entry.desc = desc;
    vk.bind_layout_cache.insert(key, layout.id);
    return layout;
}

// --- Bind groups --------------------------------------------------------------------

static const GpuBindSlot* find_slot(const GpuBindLayoutDesc& desc, const u32 slot) {
    for (u32 i = 0; i < desc.count; ++i) {
        if (desc.slots[i].slot == slot) {
            return &desc.slots[i];
        }
    }
    return nullptr;
}

static bool write_group(VulkanContext& vk, const VkBindLayoutEntry& layout, const VkDescriptorSet set, const GpuBindGroupDesc& desc) {
    VkWriteDescriptorSet writes[GPU_MAX_BIND_SLOTS] = {};
    VkDescriptorBufferInfo buffers[GPU_MAX_BIND_SLOTS] = {};
    VkDescriptorImageInfo images[GPU_MAX_BIND_SLOTS] = {};
    u32 count = 0;

    for (u32 i = 0; i < desc.count; ++i) {
        const GpuBindEntry& entry = desc.entries[i];
        const GpuBindSlot* slot = find_slot(layout.desc, entry.slot);
        if (slot == nullptr) {
            GPU::log(GPU::LOG_ERROR, "[vulkan] bind group: the layout has no slot %u", entry.slot);
            return false;
        }
        VkWriteDescriptorSet& write = writes[count];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = entry.slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_FORMATS::to_vk(slot->type);

        switch (slot->type) {
        case GPU_BINDING_UNIFORM_BUFFER:
        case GPU_BINDING_STORAGE_BUFFER: {
            const VkBufferEntry* buffer = VK_CONTEXT::buffer(vk, entry.buffer.id);
            if (buffer == nullptr) {
                GPU::log(GPU::LOG_ERROR, "[vulkan] bind group: slot %u has no valid buffer", entry.slot);
                return false;
            }
            buffers[count].buffer = buffer->buffer;
            buffers[count].offset = entry.offset;
            buffers[count].range = entry.range != 0 ? entry.range : VK_WHOLE_SIZE;
            write.pBufferInfo = &buffers[count];
            break;
        }
        case GPU_BINDING_TEXTURE:
        case GPU_BINDING_STORAGE_TEXTURE: {
            const VkTextureEntry* texture = VK_CONTEXT::texture(vk, entry.texture.id);
            if (texture == nullptr) {
                GPU::log(GPU::LOG_ERROR, "[vulkan] bind group: slot %u has no valid texture", entry.slot);
                return false;
            }
            images[count].imageView = slot->type == GPU_BINDING_STORAGE_TEXTURE ? texture->view : texture->sampled_view;
            images[count].imageLayout = slot->type == GPU_BINDING_STORAGE_TEXTURE ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            write.pImageInfo = &images[count];
            break;
        }
        case GPU_BINDING_SAMPLER: {
            const VkSamplerEntry* sampler = VK_CONTEXT::sampler(vk, entry.sampler.id);
            if (sampler == nullptr) {
                GPU::log(GPU::LOG_ERROR, "[vulkan] bind group: slot %u has no valid sampler", entry.slot);
                return false;
            }
            images[count].sampler = sampler->sampler;
            write.pImageInfo = &images[count];
            break;
        }
        }
        count += 1;
    }
    if (count > 0) {
        vkUpdateDescriptorSets(vk.device, count, writes, 0, nullptr);
    }
    return true;
}

static GpuBindGroup vk_create_bind_group(GpuContext* gpu, const GpuBindLayout layout, const GpuBindGroupDesc& desc) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    GpuBindGroup group;
    const VkBindLayoutEntry* layout_entry = VK_CONTEXT::bind_layout(vk, layout.id);
    if (layout_entry == nullptr) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] create_bind_group: invalid layout");
        return group;
    }
    u32 pool = 0;
    const VkDescriptorSet set = VK_BINDING::pools_allocate(vk, vk.persistent_sets, layout_entry->layout, &pool);
    if (set == VK_NULL_HANDLE) {
        return group;
    }
    if (!write_group(vk, *layout_entry, set, desc)) {
        vkFreeDescriptorSets(vk.device, vk.persistent_sets.pools[pool], 1, &set);
        return group;
    }
    group.id = vk.bind_groups.new_element();
    VkBindGroupEntry& entry = *vk.bind_groups.get_element_alive(group.id);
    entry.set = set;
    entry.pool = pool;
    entry.transient = false;
    return group;
}

static void vk_release_bind_group(GpuContext* gpu, const GpuBindGroup group) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    VkBindGroupEntry* entry = VK_CONTEXT::bind_group(vk, group.id);
    if (entry == nullptr) {
        return;
    }
    if (!entry->transient) {
        VkPending pending;
        pending.set = entry->set;
        pending.set_pool = vk.persistent_sets.pools[entry->pool];
        VK_FRAME::release(vk, pending);
    }
    vk.bind_groups.delete_element(group.id);
}

static GpuBindGroup vk_transient_bind_group(GpuContext* gpu, const GpuBindLayout layout, const GpuBindGroupDesc& desc) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    GpuBindGroup group;
    if (!vk.frame_open) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] transient_bind_group: no frame is open");
        return group;
    }
    const VkBindLayoutEntry* layout_entry = VK_CONTEXT::bind_layout(vk, layout.id);
    if (layout_entry == nullptr) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] transient_bind_group: invalid layout");
        return group;
    }
    VkFrameSlot& slot = vk.slots[vk.current_slot];
    u32 pool = 0;
    const VkDescriptorSet set = VK_BINDING::pools_allocate(vk, slot.transient_sets, layout_entry->layout, &pool);
    if (set == VK_NULL_HANDLE) {
        return group;
    }
    if (!write_group(vk, *layout_entry, set, desc)) {
        // The pool reset at the slot's next frame reclaims it.
        return group;
    }
    group.id = vk.bind_groups.new_element();
    VkBindGroupEntry& entry = *vk.bind_groups.get_element_alive(group.id);
    entry.set = set;
    entry.pool = pool;
    entry.transient = true;
    slot.transient_groups.push(group.id);
    return group;
}

// --- Uniform ring --------------------------------------------------------------------

static GpuUniformRange vk_push_uniforms(GpuContext* gpu, const void* data, const u32 size) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    GpuUniformRange range;
    if (!vk.frame_open) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] push_uniforms: no frame is open");
        return range;
    }
    VkFrameSlot& slot = vk.slots[vk.current_slot];
    const VkBufferEntry* ring = VK_CONTEXT::buffer(vk, slot.uniform_ring.id);
    if (ring == nullptr) {
        return range;
    }
    u64 alignment = gpu->info.limits.uniform_buffer_alignment;
    if (alignment < 16) {
        alignment = 16;
    }
    const u64 offset = (slot.uniform_cursor + alignment - 1) & ~(alignment - 1);
    if (offset + size > ring->size) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] push_uniforms: the frame's uniform ring (%u bytes) is full", VK_UNIFORM_RING_SIZE);
        return range;
    }
    memcpy(static_cast<u8*>(ring->mapped) + offset, data, size);
    slot.uniform_cursor = offset + size;
    range.buffer = slot.uniform_ring;
    range.offset = offset;
    range.size = size;
    return range;
}

namespace VK_BINDING_TABLE {
void fill(GpuBackend& table) {
    table.sampler = vk_sampler;
    table.bind_layout = vk_bind_layout;
    table.create_bind_group = vk_create_bind_group;
    table.release_bind_group = vk_release_bind_group;
    table.transient_bind_group = vk_transient_bind_group;
    table.push_uniforms = vk_push_uniforms;
}
} // namespace VK_BINDING_TABLE
