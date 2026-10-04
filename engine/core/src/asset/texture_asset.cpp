#include "engine/asset/texture_asset.hpp"

#include "engine/memory/heap_allocator.hpp"

#include <cmath>
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

// --- Writing ----------------------------------------------------------------------

u32 TEXTURE_FORMAT::srgb_variant(const u32 format) {
    switch (format) {
    case TEXTURE_FORMAT_RGBA8_UNORM: return TEXTURE_FORMAT_RGBA8_SRGB;
    case TEXTURE_FORMAT_BC1_RGB_UNORM: return TEXTURE_FORMAT_BC1_RGB_SRGB;
    case TEXTURE_FORMAT_BC3_UNORM: return TEXTURE_FORMAT_BC3_SRGB;
    case TEXTURE_FORMAT_BC7_UNORM: return TEXTURE_FORMAT_BC7_SRGB;
    default: return format;
    }
}

const char* TEXTURE_ASSET::write_error_name(const TextureWriteError error) {
    switch (error) {
    case TEXTURE_WRITE_OK: return "ok";
    case TEXTURE_WRITE_BAD_SOURCE: return "invalid source dimensions or format";
    case TEXTURE_WRITE_BAD_SIZE: return "source pixel size does not match its dimensions";
    case TEXTURE_WRITE_CANNOT_GENERATE_MIPS: return "mips cannot be generated for a block compressed format";
    }
    return "unknown error";
}

// --- Mip filtering ----------------------------------------------------------------
// The filter works on floats: every channel of every uncompressed format is
// loaded to [0, 1] (or its float value), averaged over the 2x2(x2) source
// texels, and stored back. sRGB channels are decoded to linear first and
// re-encoded after, so a half-grey mip really is half as bright.

namespace {

enum ChannelKind : u32 {
    CHANNEL_UNORM8,
    CHANNEL_UNORM16,
    CHANNEL_FLOAT16,
    CHANNEL_FLOAT32,
};

struct ChannelLayout {
    ChannelKind kind = CHANNEL_UNORM8;
    u32 count = 0; // channels per texel
    u32 bytes = 0; // per channel
    bool srgb = false; // channels 0 to 2 are sRGB encoded; alpha stays linear
};

// False for block compressed formats, which cannot be filtered.
bool channel_layout(const u32 format, ChannelLayout* out) {
    switch (format) {
    case TEXTURE_FORMAT_R8_UNORM: *out = {CHANNEL_UNORM8, 1, 1, false}; return true;
    case TEXTURE_FORMAT_RG8_UNORM: *out = {CHANNEL_UNORM8, 2, 1, false}; return true;
    case TEXTURE_FORMAT_RGBA8_UNORM: *out = {CHANNEL_UNORM8, 4, 1, false}; return true;
    case TEXTURE_FORMAT_RGBA8_SRGB: *out = {CHANNEL_UNORM8, 4, 1, true}; return true;
    case TEXTURE_FORMAT_R16_UNORM: *out = {CHANNEL_UNORM16, 1, 2, false}; return true;
    case TEXTURE_FORMAT_RG16_UNORM: *out = {CHANNEL_UNORM16, 2, 2, false}; return true;
    case TEXTURE_FORMAT_RGBA16_UNORM: *out = {CHANNEL_UNORM16, 4, 2, false}; return true;
    case TEXTURE_FORMAT_R16_FLOAT: *out = {CHANNEL_FLOAT16, 1, 2, false}; return true;
    case TEXTURE_FORMAT_RG16_FLOAT: *out = {CHANNEL_FLOAT16, 2, 2, false}; return true;
    case TEXTURE_FORMAT_RGBA16_FLOAT: *out = {CHANNEL_FLOAT16, 4, 2, false}; return true;
    case TEXTURE_FORMAT_R32_FLOAT: *out = {CHANNEL_FLOAT32, 1, 4, false}; return true;
    case TEXTURE_FORMAT_RG32_FLOAT: *out = {CHANNEL_FLOAT32, 2, 4, false}; return true;
    case TEXTURE_FORMAT_RGBA32_FLOAT: *out = {CHANNEL_FLOAT32, 4, 4, false}; return true;
    default: return false;
    }
}

// IEEE binary16 <-> binary32. Round to nearest even on the way down.
f32 f16_to_f32(const u16 half) {
    const u32 sign = static_cast<u32>(half & 0x8000u) << 16;
    const u32 exponent = (half >> 10) & 0x1fu;
    const u32 mantissa = half & 0x3ffu;
    if (exponent == 0) {
        // Zero or subnormal: mantissa * 2^-24.
        const f32 value = static_cast<f32>(mantissa) * 5.9604644775390625e-08f;
        return sign != 0 ? -value : value;
    }
    u32 bits;
    if (exponent == 31) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    }
    f32 value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

u16 f32_to_f16(const f32 value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const u32 sign = (bits >> 16) & 0x8000u;
    const u32 float_exponent = (bits >> 23) & 0xffu;
    u32 mantissa = bits & 0x7fffffu;
    if (float_exponent == 0xff) {
        // Infinity or NaN; keep NaN a NaN.
        return static_cast<u16>(sign | 0x7c00u | (mantissa != 0 ? 0x200u : 0));
    }
    const i32 exponent = static_cast<i32>(float_exponent) - 127 + 15;
    if (exponent >= 31) {
        return static_cast<u16>(sign | 0x7c00u);
    }
    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<u16>(sign);
        }
        // Subnormal: the implicit one moves into the mantissa and the whole
        // thing shifts down to units of 2^-24.
        mantissa |= 0x800000u;
        const u32 shift = static_cast<u32>(14 - exponent);
        u32 half_mantissa = mantissa >> shift;
        const u32 remainder = mantissa & ((1u << shift) - 1);
        const u32 halfway = 1u << (shift - 1);
        if (remainder > halfway || (remainder == halfway && (half_mantissa & 1) != 0)) {
            half_mantissa += 1; // may carry into the smallest normal, which is right
        }
        return static_cast<u16>(sign | half_mantissa);
    }
    u32 half = sign | (static_cast<u32>(exponent) << 10) | (mantissa >> 13);
    const u32 remainder = mantissa & 0x1fffu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1) != 0)) {
        half += 1; // may carry into infinity, which is the correctly rounded result
    }
    return static_cast<u16>(half);
}

f32 srgb_to_linear(const f32 value) {
    return value <= 0.04045f ? value / 12.92f : powf((value + 0.055f) / 1.055f, 2.4f);
}

f32 linear_to_srgb(const f32 value) {
    return value <= 0.0031308f ? value * 12.92f : 1.055f * powf(value, 1.0f / 2.4f) - 0.055f;
}

// Decoded sRGB for every byte, since 8-bit sRGB is the common case and powf
// per sample would dominate the import.
const f32* srgb8_to_linear_table() {
    static f32 table[256];
    static bool built = false;
    if (!built) {
        for (u32 i = 0; i < 256; ++i) {
            table[i] = srgb_to_linear(static_cast<f32>(i) / 255.0f);
        }
        built = true;
    }
    return table;
}

f32 load_channel(const u8* texel, const ChannelLayout& layout, const u32 channel) {
    switch (layout.kind) {
    case CHANNEL_UNORM8: {
        const u8 value = texel[channel];
        if (layout.srgb && channel < 3) {
            return srgb8_to_linear_table()[value];
        }
        return static_cast<f32>(value) / 255.0f;
    }
    case CHANNEL_UNORM16: {
        u16 value;
        std::memcpy(&value, texel + channel * 2, sizeof(value));
        return static_cast<f32>(value) / 65535.0f;
    }
    case CHANNEL_FLOAT16: {
        u16 value;
        std::memcpy(&value, texel + channel * 2, sizeof(value));
        return f16_to_f32(value);
    }
    case CHANNEL_FLOAT32: {
        f32 value;
        std::memcpy(&value, texel + channel * 4, sizeof(value));
        return value;
    }
    }
    return 0;
}

f32 clamp_unit(const f32 value) {
    return value < 0 ? 0 : value > 1 ? 1 : value;
}

void store_channel(u8* texel, const ChannelLayout& layout, const u32 channel, const f32 value) {
    switch (layout.kind) {
    case CHANNEL_UNORM8: {
        const f32 encoded = layout.srgb && channel < 3 ? linear_to_srgb(clamp_unit(value)) : clamp_unit(value);
        texel[channel] = static_cast<u8>(encoded * 255.0f + 0.5f);
        return;
    }
    case CHANNEL_UNORM16: {
        const u16 encoded = static_cast<u16>(clamp_unit(value) * 65535.0f + 0.5f);
        std::memcpy(texel + channel * 2, &encoded, sizeof(encoded));
        return;
    }
    case CHANNEL_FLOAT16: {
        const u16 encoded = f32_to_f16(value);
        std::memcpy(texel + channel * 2, &encoded, sizeof(encoded));
        return;
    }
    case CHANNEL_FLOAT32: std::memcpy(texel + channel * 4, &value, sizeof(value)); return;
    }
}

// Linear UNORM channels are averaged as integers: exact, so a .5 rounds the
// same way everywhere, and faster than the float path for the common case.
bool channel_is_integer(const ChannelLayout& layout, const u32 channel) {
    const bool unorm = layout.kind == CHANNEL_UNORM8 || layout.kind == CHANNEL_UNORM16;
    return unorm && !(layout.srgb && channel < 3);
}

u32 load_unorm(const u8* texel, const ChannelLayout& layout, const u32 channel) {
    if (layout.kind == CHANNEL_UNORM8) {
        return texel[channel];
    }
    u16 value;
    std::memcpy(&value, texel + channel * 2, sizeof(value));
    return value;
}

void store_unorm(u8* texel, const ChannelLayout& layout, const u32 channel, const u32 value) {
    if (layout.kind == CHANNEL_UNORM8) {
        texel[channel] = static_cast<u8>(value);
        return;
    }
    const u16 encoded = static_cast<u16>(value);
    std::memcpy(texel + channel * 2, &encoded, sizeof(encoded));
}

// Box filters one layer: every target texel averages the 2x2x2 source texels
// it covers (2x2 for 2D). A source extent that is already 1 contributes the
// same texel twice, which keeps the weights uniform; the last row or column
// of an odd extent is dropped, as the halve-and-clamp mip rule implies.
void downsample_layer(const ChannelLayout& layout, const u8* source, const u32 source_width, const u32 source_height,
                      const u32 source_depth, u8* target, const u32 target_width, const u32 target_height, const u32 target_depth) {
    const usz texel_bytes = layout.count * layout.bytes;
    const usz source_row = texel_bytes * source_width;
    const usz source_slice = source_row * source_height;
    u8* out = target;
    for (u32 z = 0; z < target_depth; ++z) {
        const u32 z0 = 2 * z < source_depth ? 2 * z : source_depth - 1;
        const u32 z1 = 2 * z + 1 < source_depth ? 2 * z + 1 : source_depth - 1;
        for (u32 y = 0; y < target_height; ++y) {
            const u32 y0 = 2 * y < source_height ? 2 * y : source_height - 1;
            const u32 y1 = 2 * y + 1 < source_height ? 2 * y + 1 : source_height - 1;
            for (u32 x = 0; x < target_width; ++x) {
                const u32 x0 = 2 * x < source_width ? 2 * x : source_width - 1;
                const u32 x1 = 2 * x + 1 < source_width ? 2 * x + 1 : source_width - 1;
                const u8* taps[8] = {
                    source + z0 * source_slice + y0 * source_row + x0 * texel_bytes,
                    source + z0 * source_slice + y0 * source_row + x1 * texel_bytes,
                    source + z0 * source_slice + y1 * source_row + x0 * texel_bytes,
                    source + z0 * source_slice + y1 * source_row + x1 * texel_bytes,
                    source + z1 * source_slice + y0 * source_row + x0 * texel_bytes,
                    source + z1 * source_slice + y0 * source_row + x1 * texel_bytes,
                    source + z1 * source_slice + y1 * source_row + x0 * texel_bytes,
                    source + z1 * source_slice + y1 * source_row + x1 * texel_bytes,
                };
                for (u32 channel = 0; channel < layout.count; ++channel) {
                    if (channel_is_integer(layout, channel)) {
                        u32 sum = 0; // at most 8 * 65535, fits
                        for (u32 tap = 0; tap < 8; ++tap) {
                            sum += load_unorm(taps[tap], layout, channel);
                        }
                        store_unorm(out, layout, channel, (sum + 4) >> 3); // round half up
                    } else {
                        f32 sum = 0;
                        for (u32 tap = 0; tap < 8; ++tap) {
                            sum += load_channel(taps[tap], layout, channel);
                        }
                        store_channel(out, layout, channel, sum * 0.125f);
                    }
                }
                out += texel_bytes;
            }
        }
    }
}

bool source_is_valid(const TextureSource& source, const u32 format) {
    if (source.width == 0 || source.height == 0 || source.depth == 0 || source.layers == 0) {
        return false;
    }
    if (!TEXTURE_FORMAT::info(format).is_valid()) {
        return false;
    }
    switch (source.dimension) {
    case TEXTURE_DIMENSION_2D: return source.depth == 1;
    case TEXTURE_DIMENSION_CUBE: return source.depth == 1 && source.layers % TEXTURE_ASSET::CUBE_FACES == 0;
    case TEXTURE_DIMENSION_3D: return source.layers == 1;
    default: return false;
    }
}

} // namespace

// --- TextureAssetWriter -----------------------------------------------------------

TextureAssetWriter::TextureAssetWriter() : TextureAssetWriter(MEMORY::heap_allocator()) {}

TextureAssetWriter::TextureAssetWriter(BaseAllocator* allocator) : mips(allocator), pixels(allocator) {}

TextureWriteError TextureAssetWriter::build(const TextureSource& source, const TextureImportOptions& options) {
    this->clear();

    const u32 format = options.srgb ? TEXTURE_FORMAT::srgb_variant(source.format) : source.format;
    if (!source_is_valid(source, format)) {
        return TEXTURE_WRITE_BAD_SOURCE;
    }
    const u64 layer_bytes = TEXTURE_FORMAT::layer_size(format, source.width, source.height, source.depth);
    if (source.pixels == nullptr || source.pixels_size != layer_bytes * source.layers) {
        return TEXTURE_WRITE_BAD_SIZE;
    }
    const TextureFormatInfo info = TEXTURE_FORMAT::info(format);
    if (options.generate_mips && info.is_block_compressed()) {
        return TEXTURE_WRITE_CANNOT_GENERATE_MIPS;
    }

    u32 mip_count = 1;
    if (options.generate_mips) {
        mip_count = TEXTURE_ASSET::full_mip_count(source.width, source.height, source.depth);
        mip_count = mip_count < TEXTURE_ASSET::MAX_MIPS ? mip_count : TEXTURE_ASSET::MAX_MIPS;
    }

    // Lay out the chain: level order, each mip on a MIP_ALIGNMENT boundary.
    u64 offset = 0;
    u64 decoded_size = 0;
    for (u32 level = 0; level < mip_count; ++level) {
        TextureMip mip;
        mip.width = TEXTURE_ASSET::mip_extent(source.width, level);
        mip.height = TEXTURE_ASSET::mip_extent(source.height, level);
        mip.depth = TEXTURE_ASSET::mip_extent(source.depth, level);
        mip.row_pitch = TEXTURE_FORMAT::row_pitch(format, mip.width);
        mip.size = TEXTURE_FORMAT::layer_size(format, mip.width, mip.height, mip.depth) * source.layers;
        mip.offset = offset;
        offset = ASSET_FILE::align_up(static_cast<usz>(offset + mip.size), TEXTURE_ASSET::MIP_ALIGNMENT);
        decoded_size += mip.size;
        this->mips.push(mip);
    }
    const TextureMip& last = this->mips[mip_count - 1];
    this->pixels.resize(static_cast<usz>(last.offset + last.size)); // zeroed, so the alignment gaps are too
    std::memcpy(this->pixels.data, source.pixels, source.pixels_size);

    if (mip_count > 1) {
        ChannelLayout layout;
        const bool filterable = channel_layout(format, &layout);
        ENGINE_ASSERT(filterable, "TextureAssetWriter::build: every uncompressed format has a channel layout");
        (void)filterable;
        for (u32 level = 1; level < mip_count; ++level) {
            const TextureMip& from = this->mips[level - 1];
            const TextureMip& to = this->mips[level];
            const usz from_layer = static_cast<usz>(TEXTURE_FORMAT::layer_size(format, from.width, from.height, from.depth));
            const usz to_layer = static_cast<usz>(TEXTURE_FORMAT::layer_size(format, to.width, to.height, to.depth));
            for (u32 layer = 0; layer < source.layers; ++layer) {
                downsample_layer(layout, this->pixels.data + from.offset + from_layer * layer, from.width, from.height, from.depth,
                                 this->pixels.data + to.offset + to_layer * layer, to.width, to.height, to.depth);
            }
        }
    }

    this->desc.width = source.width;
    this->desc.height = source.height;
    this->desc.depth = source.depth;
    this->desc.layers = source.layers;
    this->desc.mip_count = mip_count;
    this->desc.format = format;
    this->desc.dimension = source.dimension;
    this->desc.compression = TEXTURE_COMPRESSION_NONE;
    this->desc.decoded_size = decoded_size;
    return TEXTURE_WRITE_OK;
}

void TextureAssetWriter::write_desc(void* out) const {
    u8* cursor = static_cast<u8*>(out);
    std::memcpy(cursor, &this->desc, sizeof(TextureDesc));
    if (this->mips.count > 0) {
        std::memcpy(cursor + sizeof(TextureDesc), this->mips.data, sizeof(TextureMip) * this->mips.count);
    }
}

void TextureAssetWriter::add_chunks(AssetWriter& file) const {
    if (!this->is_built()) {
        return;
    }
    alignas(8) u8 payload[TEXTURE_ASSET::desc_size(TEXTURE_ASSET::MAX_MIPS)];
    this->write_desc(payload);
    file.add_chunk(CHUNK_TAG::TEXTURE, TEXTURE_ASSET::VERSION, 0, payload, this->desc_size());
    file.add_chunk(CHUNK_TAG::PIXELS, TEXTURE_ASSET::VERSION, 0, this->pixels.data, this->pixels.count);
}

void TextureAssetWriter::clear() {
    this->desc = TextureDesc{};
    this->mips.clear();
    this->pixels.clear();
}

void TextureAssetWriter::free() {
    this->desc = TextureDesc{};
    this->mips.free();
    this->pixels.free();
}
