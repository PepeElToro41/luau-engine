#include "engine/gpu/descriptor.hpp"

#include "engine/utils/hash.hpp"
#include "gpu/vk_check.hpp"

#include <cstdio>
#include <cstring>

// --- DescriptorLayoutDesc ----------------------------------------------------

bool DescriptorLayoutDesc::add(const u32 binding, const VkDescriptorType type, const u32 descriptor_count, const VkShaderStageFlags stages) {
    if (this->count >= DESCRIPTOR_LAYOUT_MAX_BINDINGS) {
        fprintf(stderr, "[descriptor] more than %u bindings in one layout\n", DESCRIPTOR_LAYOUT_MAX_BINDINGS);
        return false;
    }
    VkDescriptorSetLayoutBinding& entry = this->bindings[this->count++];
    entry = VkDescriptorSetLayoutBinding{};
    entry.binding = binding;
    entry.descriptorType = type;
    entry.descriptorCount = descriptor_count;
    entry.stageFlags = stages;
    return true;
}

u64 DescriptorLayoutDesc::hash() const {
    // Sort a copy by binding index so the hash is order-independent.
    u32 order[DESCRIPTOR_LAYOUT_MAX_BINDINGS];
    for (u32 i = 0; i < this->count; ++i) {
        order[i] = i;
    }
    for (u32 i = 1; i < this->count; ++i) {
        const u32 value = order[i];
        u32 j = i;
        while (j > 0 && this->bindings[order[j - 1]].binding > this->bindings[value].binding) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = value;
    }
    u64 hash = HASH::fnv1a(&this->count, sizeof(this->count));
    for (u32 i = 0; i < this->count; ++i) {
        const VkDescriptorSetLayoutBinding& b = this->bindings[order[i]];
        const u32 words[4] = {b.binding, static_cast<u32>(b.descriptorType), b.descriptorCount, static_cast<u32>(b.stageFlags)};
        hash = HASH::fnv1a_append(hash, words, sizeof(words));
    }
    return hash;
}

// --- DescriptorLayoutCache ---------------------------------------------------

bool DescriptorLayoutCache::init(GpuDevice* gpu) {
    this->gpu = gpu;
    return true;
}

void DescriptorLayoutCache::shutdown() {
    if (this->gpu != nullptr) {
        for (const auto& entry : this->layouts) {
            vkDestroyDescriptorSetLayout(this->gpu->device, entry.value, nullptr);
        }
    }
    this->layouts.free();
    this->gpu = nullptr;
}

VkDescriptorSetLayout DescriptorLayoutCache::get(const DescriptorLayoutDesc& desc) {
    const u64 key = desc.hash();
    if (VkDescriptorSetLayout* found = this->layouts.find(key)) {
        return *found;
    }
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = desc.count;
    info.pBindings = desc.bindings;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (!vk_check(vkCreateDescriptorSetLayout(this->gpu->device, &info, nullptr, &layout), "vkCreateDescriptorSetLayout")) {
        return VK_NULL_HANDLE;
    }
    this->layouts.insert(key, layout);
    return layout;
}

VkDescriptorSetLayout DescriptorLayoutCache::empty_layout() {
    return this->get(DescriptorLayoutDesc{});
}

// --- DescriptorAllocator -----------------------------------------------------

bool DescriptorAllocator::init(GpuDevice* gpu, const VkDescriptorPoolSize* sizes, const u32 size_count, const u32 max_sets, const VkDescriptorPoolCreateFlags flags) {
    if (size_count == 0 || size_count > DESCRIPTOR_ALLOCATOR_MAX_SIZES || max_sets == 0) {
        fprintf(stderr, "[descriptor] allocator needs 1..%u pool sizes and a set budget\n", DESCRIPTOR_ALLOCATOR_MAX_SIZES);
        return false;
    }
    this->gpu = gpu;
    memcpy(this->sizes, sizes, size_count * sizeof(VkDescriptorPoolSize));
    this->size_count = size_count;
    this->max_sets = max_sets;
    this->flags = flags;
    this->current = 0;
    const VkDescriptorPool first = this->create_pool();
    if (first == VK_NULL_HANDLE) {
        return false;
    }
    this->pools.push(first);
    return true;
}

void DescriptorAllocator::shutdown() {
    if (this->gpu != nullptr) {
        for (const VkDescriptorPool pool : this->pools) {
            vkDestroyDescriptorPool(this->gpu->device, pool, nullptr);
        }
    }
    this->pools.free();
    this->gpu = nullptr;
    this->current = 0;
}

VkDescriptorPool DescriptorAllocator::create_pool() {
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.flags = this->flags;
    info.maxSets = this->max_sets;
    info.poolSizeCount = this->size_count;
    info.pPoolSizes = this->sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (!vk_check(vkCreateDescriptorPool(this->gpu->device, &info, nullptr, &pool), "vkCreateDescriptorPool")) {
        return VK_NULL_HANDLE;
    }
    return pool;
}

DescriptorSetHandle DescriptorAllocator::allocate(const VkDescriptorSetLayout layout) {
    DescriptorSetHandle handle;
    if (layout == VK_NULL_HANDLE || this->pools.count == 0) {
        return handle;
    }
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;

    // The current pool first, then every other one, then a new one.
    for (usz attempt = 0; attempt < this->pools.count; ++attempt) {
        const u32 index = static_cast<u32>((this->current + attempt) % this->pools.count);
        info.descriptorPool = this->pools[index];
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult result = vkAllocateDescriptorSets(this->gpu->device, &info, &set);
        if (result == VK_SUCCESS) {
            this->current = index;
            handle.set = set;
            handle.pool = index;
            return handle;
        }
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) {
            vk_check(result, "vkAllocateDescriptorSets");
            return handle;
        }
    }
    const VkDescriptorPool fresh = this->create_pool();
    if (fresh == VK_NULL_HANDLE) {
        return handle;
    }
    this->pools.push(fresh);
    this->current = static_cast<u32>(this->pools.count - 1);
    info.descriptorPool = fresh;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!vk_check(vkAllocateDescriptorSets(this->gpu->device, &info, &set), "vkAllocateDescriptorSets (fresh pool)")) {
        return handle;
    }
    handle.set = set;
    handle.pool = this->current;
    return handle;
}

void DescriptorAllocator::free(const DescriptorSetHandle handle) {
    if (!handle.is_valid() || handle.pool >= this->pools.count) {
        return;
    }
    if ((this->flags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT) == 0) {
        fprintf(stderr, "[descriptor] free() on an allocator without the FREE flag\n");
        return;
    }
    vkFreeDescriptorSets(this->gpu->device, this->pools[handle.pool], 1, &handle.set);
}

void DescriptorAllocator::reset() {
    for (const VkDescriptorPool pool : this->pools) {
        vkResetDescriptorPool(this->gpu->device, pool, 0);
    }
    this->current = 0;
}

// --- DescriptorWriter --------------------------------------------------------

void DescriptorWriter::write_buffer(const VkDescriptorSet set, const u32 binding, const VkDescriptorType type, const VkBuffer buffer, const VkDeviceSize offset, const VkDeviceSize range) {
    if (this->count >= MAX_WRITES) {
        fprintf(stderr, "[descriptor] more than %u writes in one batch\n", MAX_WRITES);
        return;
    }
    VkDescriptorBufferInfo& info = this->buffers[this->count];
    info.buffer = buffer;
    info.offset = offset;
    info.range = range;
    VkWriteDescriptorSet& write = this->writes[this->count];
    write = VkWriteDescriptorSet{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pBufferInfo = &info;
    this->count += 1;
}

void DescriptorWriter::write_image(
	const VkDescriptorSet set, 
	const u32 binding, 
	const VkDescriptorType type, 
	const VkImageView view, 
	const VkSampler sampler, 
	const VkImageLayout layout
) {
    if (this->count >= MAX_WRITES) {
        fprintf(stderr, "[descriptor] more than %u writes in one batch\n", MAX_WRITES);
        return;
    }
    VkDescriptorImageInfo& info = this->images[this->count];
    info.sampler = sampler;
    info.imageView = view;
    info.imageLayout = layout;
    VkWriteDescriptorSet& write = this->writes[this->count];
    write = VkWriteDescriptorSet{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pImageInfo = &info;
    this->count += 1;
}

void DescriptorWriter::update(GpuDevice* gpu) {
    if (this->count > 0) {
        vkUpdateDescriptorSets(gpu->device, this->count, this->writes, 0, nullptr);
    }
    this->count = 0;
}

// --- Samplers ----------------------------------------------------------------

u64 SamplerDesc::hash() const {
    const u32 words[6] = {
    	static_cast<u32>(this->mag_filter), static_cast<u32>(this->min_filter), static_cast<u32>(this->mipmap_mode),
        static_cast<u32>(this->address_u), static_cast<u32>(this->address_v), static_cast<u32>(this->address_w)
    };
    
    u64 hash = HASH::fnv1a(words, sizeof(words));
    return HASH::fnv1a_append(hash, &this->max_anisotropy, sizeof(this->max_anisotropy));
}

bool SamplerDesc::equals(const SamplerDesc& other) const {
    return this->mag_filter == other.mag_filter && this->min_filter == other.min_filter && this->mipmap_mode == other.mipmap_mode &&
        this->address_u == other.address_u && this->address_v == other.address_v && this->address_w == other.address_w &&
        this->max_anisotropy == other.max_anisotropy;
}

bool SamplerCache::init(GpuDevice* gpu) {
    this->gpu = gpu;
    return true;
}

void SamplerCache::shutdown() {
    if (this->gpu != nullptr) {
        for (const auto& entry : this->samplers) {
            vkDestroySampler(this->gpu->device, entry.value, nullptr);
        }
    }
    this->samplers.free();
    this->gpu = nullptr;
}

VkSampler SamplerCache::get(const SamplerDesc& desc) {
    if (VkSampler* found = this->samplers.find(desc)) {
        return *found;
    }
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = desc.mag_filter;
    info.minFilter = desc.min_filter;
    info.mipmapMode = desc.mipmap_mode;
    info.addressModeU = desc.address_u;
    info.addressModeV = desc.address_v;
    info.addressModeW = desc.address_w;
    info.anisotropyEnable = desc.max_anisotropy > 0.0f ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = desc.max_anisotropy > 0.0f ? desc.max_anisotropy : 1.0f;
    info.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler sampler = VK_NULL_HANDLE;
    if (!vk_check(vkCreateSampler(this->gpu->device, &info, nullptr, &sampler), "vkCreateSampler")) {
        return VK_NULL_HANDLE;
    }
    this->samplers.insert(desc, sampler);
    return sampler;
}
