#include "engine/asset/texture_asset.hpp"

#include <cstdint>
#include <cstring>

// --- Formats --------------------------------------------------------------------

TextureFormatInfo TEXTURE_FORMAT::info(const u32 format) {
    switch (format) {
    case TEXTURE_FORMAT_R8_UNORM: return {1, 1, 1, false};
    case TEXTURE_FORMAT_RG8_UNORM: return {1, 1, 2, false};
    case TEXTURE_FORMAT_RGBA8_UNORM: return {1, 1, 4, false};
    case TEXTURE_FORMAT_RGBA8_SRGB: return {1, 1, 4, true};

    case TEXTURE_FORMAT_R16_UNORM: return {1, 1, 2, false};
    case TEXTURE_FORMAT_RG16_UNORM: return {1, 1, 4, false};
    case TEXTURE_FORMAT_RGBA16_UNORM: return {1, 1, 8, false};
    case TEXTURE_FORMAT_R16_FLOAT: return {1, 1, 2, false};
    case TEXTURE_FORMAT_RG16_FLOAT: return {1, 1, 4, false};
    case TEXTURE_FORMAT_RGBA16_FLOAT: return {1, 1, 8, false};

    case TEXTURE_FORMAT_R32_FLOAT: return {1, 1, 4, false};
    case TEXTURE_FORMAT_RG32_FLOAT: return {1, 1, 8, false};
    case TEXTURE_FORMAT_RGBA32_FLOAT: return {1, 1, 16, false};

    case TEXTURE_FORMAT_BC1_RGB_UNORM: return {4, 4, 8, false};
    case TEXTURE_FORMAT_BC1_RGB_SRGB: return {4, 4, 8, true};
    case TEXTURE_FORMAT_BC3_UNORM: return {4, 4, 16, false};
    case TEXTURE_FORMAT_BC3_SRGB: return {4, 4, 16, true};
    case TEXTURE_FORMAT_BC4_UNORM: return {4, 4, 8, false};
    case TEXTURE_FORMAT_BC5_UNORM: return {4, 4, 16, false};
    case TEXTURE_FORMAT_BC6H_UFLOAT: return {4, 4, 16, false};
    case TEXTURE_FORMAT_BC7_UNORM: return {4, 4, 16, false};
    case TEXTURE_FORMAT_BC7_SRGB: return {4, 4, 16, true};
    default: return {};
    }
}

const char* TEXTURE_FORMAT::name(const u32 format) {
    switch (format) {
    case TEXTURE_FORMAT_NONE: return "none";
    case TEXTURE_FORMAT_R8_UNORM: return "R8_UNORM";
    case TEXTURE_FORMAT_RG8_UNORM: return "RG8_UNORM";
    case TEXTURE_FORMAT_RGBA8_UNORM: return "RGBA8_UNORM";
    case TEXTURE_FORMAT_RGBA8_SRGB: return "RGBA8_SRGB";
    case TEXTURE_FORMAT_R16_UNORM: return "R16_UNORM";
    case TEXTURE_FORMAT_RG16_UNORM: return "RG16_UNORM";
    case TEXTURE_FORMAT_RGBA16_UNORM: return "RGBA16_UNORM";
    case TEXTURE_FORMAT_R16_FLOAT: return "R16_FLOAT";
    case TEXTURE_FORMAT_RG16_FLOAT: return "RG16_FLOAT";
    case TEXTURE_FORMAT_RGBA16_FLOAT: return "RGBA16_FLOAT";
    case TEXTURE_FORMAT_R32_FLOAT: return "R32_FLOAT";
    case TEXTURE_FORMAT_RG32_FLOAT: return "RG32_FLOAT";
    case TEXTURE_FORMAT_RGBA32_FLOAT: return "RGBA32_FLOAT";
    case TEXTURE_FORMAT_BC1_RGB_UNORM: return "BC1_RGB_UNORM";
    case TEXTURE_FORMAT_BC1_RGB_SRGB: return "BC1_RGB_SRGB";
    case TEXTURE_FORMAT_BC3_UNORM: return "BC3_UNORM";
    case TEXTURE_FORMAT_BC3_SRGB: return "BC3_SRGB";
    case TEXTURE_FORMAT_BC4_UNORM: return "BC4_UNORM";
    case TEXTURE_FORMAT_BC5_UNORM: return "BC5_UNORM";
    case TEXTURE_FORMAT_BC6H_UFLOAT: return "BC6H_UFLOAT";
    case TEXTURE_FORMAT_BC7_UNORM: return "BC7_UNORM";
    case TEXTURE_FORMAT_BC7_SRGB: return "BC7_SRGB";
    default: return "unknown";
    }
}

u32 TEXTURE_ASSET::full_mip_count(const u32 width, const u32 height, const u32 depth) {
    u32 largest = width > height ? width : height;
    largest = largest > depth ? largest : depth;
    u32 count = 1;
    while (largest > 1) {
        largest >>= 1;
        ++count;
    }
    return count;
}

// --- Errors -----------------------------------------------------------------------

const char* TEXTURE_ASSET::parse_error_name(const TextureParseError error) {
    switch (error) {
    case TEXTURE_PARSE_OK: return "ok";
    case TEXTURE_PARSE_NOT_A_TEXTURE: return "not a texture asset";
    case TEXTURE_PARSE_MISSING_CHUNK: return "missing TEX2 or PIXL chunk";
    case TEXTURE_PARSE_UNSUPPORTED_VERSION: return "unsupported texture payload version";
    case TEXTURE_PARSE_BAD_DESC: return "invalid texture description";
    case TEXTURE_PARSE_BAD_MIP: return "invalid mip table";
    }
    return "unknown error";
}

// --- TextureAssetView -----------------------------------------------------------

void TextureAssetView::reset() {
    this->desc = nullptr;
    this->mips = nullptr;
    this->pixels_chunk = nullptr;
}

namespace {

bool desc_is_valid(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0 || desc.depth == 0 || desc.layers == 0) {
        return false;
    }
    if (desc.mip_count == 0 || desc.mip_count > TEXTURE_ASSET::MAX_MIPS) {
        return false;
    }
    if (desc.mip_count > TEXTURE_ASSET::full_mip_count(desc.width, desc.height, desc.depth)) {
        return false;
    }
    if (!TEXTURE_FORMAT::info(desc.format).is_valid()) {
        return false;
    }
    if (desc.compression != TEXTURE_COMPRESSION_NONE) {
        return false;
    }
    if (desc.flags != 0 || desc.reserved[0] != 0 || desc.reserved[1] != 0 || desc.reserved[2] != 0 || desc.reserved[3] != 0 ||
        desc.reserved[4] != 0) {
        return false;
    }
    switch (desc.dimension) {
    case TEXTURE_DIMENSION_2D: return desc.depth == 1;
    case TEXTURE_DIMENSION_CUBE: return desc.depth == 1 && desc.layers % TEXTURE_ASSET::CUBE_FACES == 0;
    case TEXTURE_DIMENSION_3D: return desc.layers == 1;
    default: return false;
    }
}

// Checks one mip against what the desc implies for it and against the PIXL
// size; `previous_end` is where the previous mip ended inside PIXL.
bool mip_is_valid(const TextureDesc& desc, const TextureMip& mip, const u32 level, const u64 pixels_size, const u64 previous_end) {
    if (mip.width != TEXTURE_ASSET::mip_extent(desc.width, level) || mip.height != TEXTURE_ASSET::mip_extent(desc.height, level) ||
        mip.depth != TEXTURE_ASSET::mip_extent(desc.depth, level)) {
        return false;
    }
    if (mip.row_pitch != TEXTURE_FORMAT::row_pitch(desc.format, mip.width)) {
        return false;
    }
    // Version 1 only knows uncompressed data, so the stored size is the
    // decoded size.
    const u64 expected = TEXTURE_FORMAT::layer_size(desc.format, mip.width, mip.height, mip.depth) * desc.layers;
    if (mip.size != expected) {
        return false;
    }
    if (mip.offset % TEXTURE_ASSET::MIP_ALIGNMENT != 0) {
        return false;
    }
    if (mip.offset < previous_end || mip.offset > pixels_size || mip.size > pixels_size - mip.offset) {
        return false;
    }
    return true;
}

} // namespace

TextureParseError TextureAssetView::parse(const AssetView& file, const void* payload, const usz size) {
    this->reset();

    if (!file.is_parsed() || file.header->type != ASSET_TYPE::TEXTURE) {
        return TEXTURE_PARSE_NOT_A_TEXTURE;
    }
    const ChunkEntry* desc_chunk = file.find_chunk(CHUNK_TAG::TEXTURE);
    const ChunkEntry* pixel_chunk = file.find_chunk(CHUNK_TAG::PIXELS);
    if (desc_chunk == nullptr || pixel_chunk == nullptr) {
        return TEXTURE_PARSE_MISSING_CHUNK;
    }
    if (desc_chunk->version != TEXTURE_ASSET::VERSION || pixel_chunk->version != TEXTURE_ASSET::VERSION) {
        return TEXTURE_PARSE_UNSUPPORTED_VERSION;
    }

    // The payload must be the TEX2 chunk's bytes; copy the desc out so its
    // fields are checked before the rest is trusted.
    if (payload == nullptr || size != desc_chunk->size || size < sizeof(TextureDesc)) {
        return TEXTURE_PARSE_BAD_DESC;
    }
    ENGINE_ASSERT(reinterpret_cast<uintptr_t>(payload) % alignof(TextureDesc) == 0,
                  "TextureAssetView::parse: the payload must be %zu-byte aligned (any allocator gives this)", alignof(TextureDesc));
    const u8* desc_bytes = static_cast<const u8*>(payload);
    TextureDesc desc;
    std::memcpy(&desc, desc_bytes, sizeof(TextureDesc));
    if (!desc_is_valid(desc)) {
        return TEXTURE_PARSE_BAD_DESC;
    }
    if (desc_chunk->size != TEXTURE_ASSET::desc_size(desc.mip_count)) {
        return TEXTURE_PARSE_BAD_DESC;
    }

    const TextureMip* mips = reinterpret_cast<const TextureMip*>(desc_bytes + sizeof(TextureDesc));
    u64 previous_end = 0;
    u64 decoded_size = 0;
    for (u32 level = 0; level < desc.mip_count; ++level) {
        TextureMip mip;
        std::memcpy(&mip, mips + level, sizeof(TextureMip));
        if (!mip_is_valid(desc, mip, level, pixel_chunk->size, previous_end)) {
            return TEXTURE_PARSE_BAD_MIP;
        }
        previous_end = mip.offset + mip.size;
        decoded_size += mip.size;
    }
    if (desc.decoded_size != decoded_size) {
        return TEXTURE_PARSE_BAD_DESC;
    }

    this->desc = reinterpret_cast<const TextureDesc*>(desc_bytes);
    this->mips = mips;
    this->pixels_chunk = pixel_chunk;
    return TEXTURE_PARSE_OK;
}

const u8* TextureAssetView::mip_data(const u8* pixels, const u32 mip) const {
    if (pixels == nullptr || this->desc == nullptr || mip >= this->desc->mip_count) {
        return nullptr;
    }
    return pixels + this->mips[mip].offset;
}

const u8* TextureAssetView::layer_data(const u8* pixels, const u32 mip, const u32 layer) const {
    const u8* base = this->mip_data(pixels, mip);
    if (base == nullptr || layer >= this->desc->layers) {
        return nullptr;
    }
    return base + this->layer_size(mip) * layer;
}

u64 TextureAssetView::layer_size(const u32 mip) const {
    if (this->desc == nullptr || mip >= this->desc->mip_count) {
        return 0;
    }
    const TextureMip& entry = this->mips[mip];
    return TEXTURE_FORMAT::layer_size(this->desc->format, entry.width, entry.height, entry.depth);
}
