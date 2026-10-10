#include "engine/gpu/gpu_types.hpp"

#include "engine/asset/asset_types/mesh_asset.hpp"
#include "engine/asset/asset_types/texture_asset.hpp"
#include "engine/utils/hash.hpp"
#include "gpu/backend.hpp"

#include <cstdint>

// The API-neutral side of gpu_types.hpp: format facts and the small desc
// helpers. Nothing here touches a backend.

// --- Formats -------------------------------------------------------------------

namespace {

struct FormatInfo {
    const char* name;
    u32 size; // bytes per texel, or per 4x4 block when compressed
    bool depth;
    bool stencil;
    bool srgb;
    bool compressed;
    bool integer;
};

// Indexed by GpuFormat.
const FormatInfo FORMAT_INFO[GPU_FORMAT_COUNT] = {
    {"UNDEFINED", 0, false, false, false, false, false},

    {"R8_UNORM", 1, false, false, false, false, false},
    {"RG8_UNORM", 2, false, false, false, false, false},
    {"RGBA8_UNORM", 4, false, false, false, false, false},
    {"RGBA8_SRGB", 4, false, false, true, false, false},
    {"RGBA8_SNORM", 4, false, false, false, false, false},
    {"RGBA8_UINT", 4, false, false, false, false, true},
    {"BGRA8_UNORM", 4, false, false, false, false, false},
    {"BGRA8_SRGB", 4, false, false, true, false, false},

    {"R16_UNORM", 2, false, false, false, false, false},
    {"RG16_UNORM", 4, false, false, false, false, false},
    {"RGBA16_UNORM", 8, false, false, false, false, false},
    {"RG16_SNORM", 4, false, false, false, false, false},
    {"RGBA16_SNORM", 8, false, false, false, false, false},
    {"RGBA16_UINT", 8, false, false, false, false, true},
    {"R16_FLOAT", 2, false, false, false, false, false},
    {"RG16_FLOAT", 4, false, false, false, false, false},
    {"RGBA16_FLOAT", 8, false, false, false, false, false},

    {"R32_UINT", 4, false, false, false, false, true},
    {"R32_FLOAT", 4, false, false, false, false, false},
    {"RG32_FLOAT", 8, false, false, false, false, false},
    {"RGB32_FLOAT", 12, false, false, false, false, false},
    {"RGBA32_FLOAT", 16, false, false, false, false, false},

    {"BC1_RGB_UNORM", 8, false, false, false, true, false},
    {"BC1_RGB_SRGB", 8, false, false, true, true, false},
    {"BC3_UNORM", 16, false, false, false, true, false},
    {"BC3_SRGB", 16, false, false, true, true, false},
    {"BC4_UNORM", 8, false, false, false, true, false},
    {"BC5_UNORM", 16, false, false, false, true, false},
    {"BC6H_UFLOAT", 16, false, false, false, true, false},
    {"BC7_UNORM", 16, false, false, false, true, false},
    {"BC7_SRGB", 16, false, false, true, true, false},

    {"D16_UNORM", 2, true, false, false, false, false},
    {"D32_FLOAT", 4, true, false, false, false, false},
    {"D24_UNORM_S8_UINT", 4, true, true, false, false, false},
    {"D32_FLOAT_S8_UINT", 8, true, true, false, false, false},
};

const FormatInfo& info_of(const GpuFormat format) {
    const u32 index = static_cast<u32>(format) < GPU_FORMAT_COUNT ? static_cast<u32>(format) : 0;
    return FORMAT_INFO[index];
}

} // namespace

u32 GPU_FORMAT::size(const GpuFormat format) { return info_of(format).size; }
bool GPU_FORMAT::is_depth(const GpuFormat format) { return info_of(format).depth; }
bool GPU_FORMAT::has_stencil(const GpuFormat format) { return info_of(format).stencil; }
bool GPU_FORMAT::is_srgb(const GpuFormat format) { return info_of(format).srgb; }
bool GPU_FORMAT::is_compressed(const GpuFormat format) { return info_of(format).compressed; }
bool GPU_FORMAT::is_integer(const GpuFormat format) { return info_of(format).integer; }
const char* GPU_FORMAT::name(const GpuFormat format) { return info_of(format).name; }

GpuFormat GPU_FORMAT::from_vertex(const u32 vertex_format) {
    switch (vertex_format) {
    case VERTEX_FORMAT_F32:
        return GPU_FORMAT_R32_FLOAT;
    case VERTEX_FORMAT_F32x2:
        return GPU_FORMAT_RG32_FLOAT;
    case VERTEX_FORMAT_F32x3:
        return GPU_FORMAT_RGB32_FLOAT;
    case VERTEX_FORMAT_F32x4:
        return GPU_FORMAT_RGBA32_FLOAT;
    case VERTEX_FORMAT_F16x2:
        return GPU_FORMAT_RG16_FLOAT;
    case VERTEX_FORMAT_F16x4:
        return GPU_FORMAT_RGBA16_FLOAT;
    case VERTEX_FORMAT_UNORM8x4:
        return GPU_FORMAT_RGBA8_UNORM;
    case VERTEX_FORMAT_SNORM8x4:
        return GPU_FORMAT_RGBA8_SNORM;
    case VERTEX_FORMAT_UNORM16x2:
        return GPU_FORMAT_RG16_UNORM;
    case VERTEX_FORMAT_UNORM16x4:
        return GPU_FORMAT_RGBA16_UNORM;
    case VERTEX_FORMAT_SNORM16x2:
        return GPU_FORMAT_RG16_SNORM;
    case VERTEX_FORMAT_SNORM16x4:
        return GPU_FORMAT_RGBA16_SNORM;
    case VERTEX_FORMAT_UINT8x4:
        return GPU_FORMAT_RGBA8_UINT;
    case VERTEX_FORMAT_UINT16x4:
        return GPU_FORMAT_RGBA16_UINT;
    case VERTEX_FORMAT_UINT32:
        return GPU_FORMAT_R32_UINT;
    default:
        return GPU_FORMAT_UNDEFINED;
    }
}

GpuFormat GPU_FORMAT::from_texture(const u32 texture_format) {
    switch (texture_format) {
    case TEXTURE_FORMAT_R8_UNORM:
        return GPU_FORMAT_R8_UNORM;
    case TEXTURE_FORMAT_RG8_UNORM:
        return GPU_FORMAT_RG8_UNORM;
    case TEXTURE_FORMAT_RGBA8_UNORM:
        return GPU_FORMAT_RGBA8_UNORM;
    case TEXTURE_FORMAT_RGBA8_SRGB:
        return GPU_FORMAT_RGBA8_SRGB;
    case TEXTURE_FORMAT_R16_UNORM:
        return GPU_FORMAT_R16_UNORM;
    case TEXTURE_FORMAT_RG16_UNORM:
        return GPU_FORMAT_RG16_UNORM;
    case TEXTURE_FORMAT_RGBA16_UNORM:
        return GPU_FORMAT_RGBA16_UNORM;
    case TEXTURE_FORMAT_R16_FLOAT:
        return GPU_FORMAT_R16_FLOAT;
    case TEXTURE_FORMAT_RG16_FLOAT:
        return GPU_FORMAT_RG16_FLOAT;
    case TEXTURE_FORMAT_RGBA16_FLOAT:
        return GPU_FORMAT_RGBA16_FLOAT;
    case TEXTURE_FORMAT_R32_FLOAT:
        return GPU_FORMAT_R32_FLOAT;
    case TEXTURE_FORMAT_RG32_FLOAT:
        return GPU_FORMAT_RG32_FLOAT;
    case TEXTURE_FORMAT_RGBA32_FLOAT:
        return GPU_FORMAT_RGBA32_FLOAT;
    case TEXTURE_FORMAT_BC1_RGB_UNORM:
        return GPU_FORMAT_BC1_RGB_UNORM;
    case TEXTURE_FORMAT_BC1_RGB_SRGB:
        return GPU_FORMAT_BC1_RGB_SRGB;
    case TEXTURE_FORMAT_BC3_UNORM:
        return GPU_FORMAT_BC3_UNORM;
    case TEXTURE_FORMAT_BC3_SRGB:
        return GPU_FORMAT_BC3_SRGB;
    case TEXTURE_FORMAT_BC4_UNORM:
        return GPU_FORMAT_BC4_UNORM;
    case TEXTURE_FORMAT_BC5_UNORM:
        return GPU_FORMAT_BC5_UNORM;
    case TEXTURE_FORMAT_BC6H_UFLOAT:
        return GPU_FORMAT_BC6H_UFLOAT;
    case TEXTURE_FORMAT_BC7_UNORM:
        return GPU_FORMAT_BC7_UNORM;
    case TEXTURE_FORMAT_BC7_SRGB:
        return GPU_FORMAT_BC7_SRGB;
    default:
        return GPU_FORMAT_UNDEFINED;
    }
}

// --- Samplers ------------------------------------------------------------------

u64 GpuSamplerDesc::hash() const {
    const u32 words[6] = {this->min_filter, this->mag_filter, this->mip_filter, this->address_u, this->address_v, this->address_w};
    const u64 hash = HASH::fnv1a(words, sizeof(words));
    return HASH::fnv1a_append(hash, &this->max_anisotropy, sizeof(this->max_anisotropy));
}

// --- Binding -------------------------------------------------------------------

bool GpuBindLayoutDesc::add(const u32 slot, const GpuBindingType type, const u8 stages, const u32 count) {
    if (this->count >= GPU_MAX_BIND_SLOTS) {
        GPU::log(GPU::LOG_ERROR, "bind layout: more than %u slots", GPU_MAX_BIND_SLOTS);
        return false;
    }
    GpuBindSlot& entry = this->slots[this->count++];
    entry.slot = slot;
    entry.type = type;
    entry.count = count;
    entry.stages = stages;
    return true;
}

u64 GpuBindLayoutDesc::hash() const {
    // Sort a copy of the indices by slot so the hash is order-independent.
    u32 order[GPU_MAX_BIND_SLOTS];
    for (u32 i = 0; i < this->count; ++i) {
        order[i] = i;
    }
    for (u32 i = 1; i < this->count; ++i) {
        const u32 value = order[i];
        u32 j = i;
        while (j > 0 && this->slots[order[j - 1]].slot > this->slots[value].slot) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = value;
    }
    u64 hash = HASH::fnv1a(&this->count, sizeof(this->count));
    for (u32 i = 0; i < this->count; ++i) {
        const GpuBindSlot& s = this->slots[order[i]];
        const u32 words[4] = {s.slot, s.type, s.count, s.stages};
        hash = HASH::fnv1a_append(hash, words, sizeof(words));
    }
    return hash;
}

static GpuBindEntry* next_entry(GpuBindGroupDesc& desc, const u32 slot) {
    if (desc.count >= GPU_MAX_BIND_SLOTS) {
        GPU::log(GPU::LOG_ERROR, "bind group: more than %u entries", GPU_MAX_BIND_SLOTS);
        return nullptr;
    }
    GpuBindEntry& entry = desc.entries[desc.count++];
    entry = GpuBindEntry{};
    entry.slot = slot;
    return &entry;
}

bool GpuBindGroupDesc::bind_buffer(const u32 slot, const GpuBuffer buffer, const u64 offset, const u64 range) {
    GpuBindEntry* entry = next_entry(*this, slot);
    if (entry == nullptr) {
        return false;
    }
    entry->buffer = buffer;
    entry->offset = offset;
    entry->range = range;
    return true;
}

bool GpuBindGroupDesc::bind_texture(const u32 slot, const GpuTexture texture) {
    GpuBindEntry* entry = next_entry(*this, slot);
    if (entry == nullptr) {
        return false;
    }
    entry->texture = texture;
    return true;
}

bool GpuBindGroupDesc::bind_sampler(const u32 slot, const GpuSampler sampler) {
    GpuBindEntry* entry = next_entry(*this, slot);
    if (entry == nullptr) {
        return false;
    }
    entry->sampler = sampler;
    return true;
}

// --- Pipelines -----------------------------------------------------------------

u32 GpuVertexInput::add_binding(const u32 stride, const GpuVertexRate rate) {
    if (this->binding_count >= GPU_MAX_VERTEX_BINDINGS) {
        GPU::log(GPU::LOG_ERROR, "vertex input: more than %u bindings", GPU_MAX_VERTEX_BINDINGS);
        return UINT32_MAX;
    }
    GpuVertexBinding& binding = this->bindings[this->binding_count];
    binding.stride = stride;
    binding.rate = rate;
    return this->binding_count++;
}

bool GpuVertexInput::add_attribute(const u32 location, const GpuFormat format, const u32 offset, const u32 binding) {
    if (this->attribute_count >= GPU_MAX_VERTEX_ATTRIBUTES) {
        GPU::log(GPU::LOG_ERROR, "vertex input: more than %u attributes", GPU_MAX_VERTEX_ATTRIBUTES);
        return false;
    }
    GpuVertexAttribute& attribute = this->attributes[this->attribute_count++];
    attribute.location = location;
    attribute.binding = binding;
    attribute.format = format;
    attribute.offset = offset;
    return true;
}

u64 GpuTargetFormats::hash() const {
    u64 hash = HASH::fnv1a(&this->color_count, sizeof(this->color_count));
    for (u32 i = 0; i < this->color_count; ++i) {
        const u32 format = this->color[i];
        hash = HASH::fnv1a_append(hash, &format, sizeof(format));
    }
    const u32 tail[2] = {this->depth, this->samples};
    return HASH::fnv1a_append(hash, tail, sizeof(tail));
}

// --- Render passes -------------------------------------------------------------

GpuTargetFormats GpuRenderPassDesc::formats() const {
    GpuTargetFormats out;
    for (u32 i = 0; i < this->color_count; ++i) {
        out.color[i] = this->color[i].texture.format;
    }
    out.color_count = this->color_count;
    out.depth = this->has_depth ? this->depth.texture.format : GPU_FORMAT_UNDEFINED;
    out.samples = 1;
    return out;
}
