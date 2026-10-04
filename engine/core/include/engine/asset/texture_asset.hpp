#pragma once

#include "engine/asset/asset_file.hpp"
#include "engine/defines.hpp"

// Payload layout of a texture asset (ASSET_TYPE::TEXTURE): the `TEX2` chunk
// describes the image and holds the mip table, the `PIXL` chunk holds the
// pixel bytes. docs/asset_format.md "Texture payloads" is the specification.
//
//     AssetView view;  ... parsed from the file's prelude ...
//     AssetReader reader;  ... open on the same file ...
//     u8* desc_bytes = reader.read_chunk(view, CHUNK_TAG::TEXTURE, allocator, &desc_chunk);
//     TextureAssetView texture;
//     if (texture.parse(view, desc_bytes, desc_chunk->size) == TEXTURE_PARSE_OK) {
//         u8* pixels = reader.read_chunk(*texture.pixels_chunk, allocator);
//         const u8* mip0 = texture.mip_data(pixels, 0);
//     }
//
// The TEX2 payload is tiny and validated up front; the pixels are read
// afterwards, by the caller, into whatever memory the upload wants. The view
// points into the TEX2 bytes it was given; nothing is copied. Formats and
// dimensions are engine enums, not Vulkan ones: core has no GPU dependency,
// and the graphics module maps TextureFormat to VkFormat.

namespace TEXTURE_ASSET {

// Payload version of the TEX2 and PIXL chunks written by this code.
constexpr u32 VERSION = 1;
// Every mip starts at a multiple of this inside PIXL (the chunk itself is
// 64-byte aligned, so the absolute offset is too). 16 covers every texel
// block size, which is what vkCmdCopyBufferToImage needs for bufferOffset.
constexpr usz MIP_ALIGNMENT = 16;
// A 32768 x 32768 texture has 16 mips; nothing needs more.
constexpr u32 MAX_MIPS = 16;
// Faces of a cube map, in Vulkan order: +X, -X, +Y, -Y, +Z, -Z.
constexpr u32 CUBE_FACES = 6;

} // namespace TEXTURE_ASSET

// Pixel formats. Values are stored on disk: never renumber, only append.
enum TextureFormat : u32 {
    TEXTURE_FORMAT_NONE = 0,

    // Uncompressed, 8 bits per channel.
    TEXTURE_FORMAT_R8_UNORM = 1,
    TEXTURE_FORMAT_RG8_UNORM = 2,
    TEXTURE_FORMAT_RGBA8_UNORM = 3,
    TEXTURE_FORMAT_RGBA8_SRGB = 4,

    // Uncompressed, 16 bits per channel.
    TEXTURE_FORMAT_R16_UNORM = 10,
    TEXTURE_FORMAT_RG16_UNORM = 11,
    TEXTURE_FORMAT_RGBA16_UNORM = 12,
    TEXTURE_FORMAT_R16_FLOAT = 13,
    TEXTURE_FORMAT_RG16_FLOAT = 14,
    TEXTURE_FORMAT_RGBA16_FLOAT = 15,

    // Uncompressed, 32 bits per channel.
    TEXTURE_FORMAT_R32_FLOAT = 20,
    TEXTURE_FORMAT_RG32_FLOAT = 21,
    TEXTURE_FORMAT_RGBA32_FLOAT = 22,

    // Block compressed (desktop). 4x4 texel blocks.
    TEXTURE_FORMAT_BC1_RGB_UNORM = 30,
    TEXTURE_FORMAT_BC1_RGB_SRGB = 31,
    TEXTURE_FORMAT_BC3_UNORM = 32,
    TEXTURE_FORMAT_BC3_SRGB = 33,
    TEXTURE_FORMAT_BC4_UNORM = 34,
    TEXTURE_FORMAT_BC5_UNORM = 35,
    TEXTURE_FORMAT_BC6H_UFLOAT = 36,
    TEXTURE_FORMAT_BC7_UNORM = 37,
    TEXTURE_FORMAT_BC7_SRGB = 38,
};

// What the texture is to a sampler. Array textures are TEXTURE_DIMENSION_2D
// with `layers > 1`.
enum TextureDimension : u32 {
    TEXTURE_DIMENSION_2D = 0,
    TEXTURE_DIMENSION_CUBE = 1, // `layers` is a multiple of 6, one face each
    TEXTURE_DIMENSION_3D = 2,   // `depth > 1`, `layers == 1`
};

// How the bytes in PIXL are encoded. NONE means the mips are the raw texel
// data, ready to upload. Other values are reserved for a future codec applied
// per mip; nothing writes or reads them yet.
enum TextureCompression : u32 {
    TEXTURE_COMPRESSION_NONE = 0,
};

// Per-format layout. Uncompressed formats are 1x1 blocks of `block_bytes`.
struct TextureFormatInfo {
    u32 block_width = 0;
    u32 block_height = 0;
    u32 block_bytes = 0;
    bool srgb = false;

    bool is_valid() const { return this->block_bytes != 0; }
    bool is_block_compressed() const { return this->block_width > 1; }
};

namespace TEXTURE_FORMAT {

// Zeroed info (is_valid() false) for a value that is not a TextureFormat.
TextureFormatInfo info(u32 format);
const char* name(u32 format);

// Blocks per row and rows of blocks for an image of this size.
constexpr u32 blocks_across(const u32 texels, const u32 block_size) {
    return (texels + block_size - 1) / block_size;
}

// Bytes of one row of blocks: what the mip table stores as `row_pitch`.
inline u32 row_pitch(const u32 format, const u32 width) {
    const TextureFormatInfo i = info(format);
    return i.is_valid() ? blocks_across(width, i.block_width) * i.block_bytes : 0;
}

// Bytes of one layer of one mip: `row_pitch * rows of blocks * depth`.
inline u64 layer_size(const u32 format, const u32 width, const u32 height, const u32 depth) {
    const TextureFormatInfo i = info(format);
    if (!i.is_valid()) {
        return 0;
    }
    return static_cast<u64>(row_pitch(format, width)) * blocks_across(height, i.block_height) * depth;
}

} // namespace TEXTURE_FORMAT

// --- On-disk structures ------------------------------------------------------
// Written and read verbatim. Changing a layout means bumping
// TEXTURE_ASSET::VERSION and keeping a reader for the old one.

// TEX2 payload: this struct followed by `mip_count` TextureMip entries.
struct TextureDesc {
    u32 width = 0;  // mip 0, texels
    u32 height = 0; // mip 0, texels
    u32 depth = 1;  // mip 0, texels; 1 unless TEXTURE_DIMENSION_3D
    u32 layers = 1; // array layers; cube maps hold 6 per cube
    u32 mip_count = 0;
    u32 format = TEXTURE_FORMAT_NONE;              // TextureFormat
    u32 dimension = TEXTURE_DIMENSION_2D;          // TextureDimension
    u32 compression = TEXTURE_COMPRESSION_NONE;    // TextureCompression, how PIXL is encoded
    u64 decoded_size = 0;                          // bytes of raw texel data over every mip and layer
    u32 flags = 0;                                 // no bits defined in version 1, zero
    u32 reserved[5] = {0, 0, 0, 0, 0};
};

// One mip level, all layers. Layers are stored back to back inside the mip,
// each `layer_size` bytes when uncompressed.
struct TextureMip {
    u64 offset = 0;    // from the start of PIXL; multiple of TEXTURE_ASSET::MIP_ALIGNMENT
    u64 size = 0;      // bytes in PIXL for this mip (every layer); the decoded size when compression is NONE
    u32 width = 0;     // texels
    u32 height = 0;    // texels
    u32 depth = 0;     // texels
    u32 row_pitch = 0; // bytes per row of blocks of the decoded data
};

static_assert(sizeof(TextureDesc) == 64, "TextureDesc must be 64 bytes on disk");
static_assert(sizeof(TextureMip) == 32, "TextureMip must be 32 bytes on disk");
static_assert(alignof(TextureDesc) <= 8 && alignof(TextureMip) <= 8, "payloads are placed at 64-byte aligned offsets");

namespace TEXTURE_ASSET {

// Size of the TEX2 payload for a texture with this many mips.
constexpr usz desc_size(const u32 mip_count) {
    return sizeof(TextureDesc) + sizeof(TextureMip) * mip_count;
}

// Mip dimensions follow from mip 0: each level halves and clamps to 1.
constexpr u32 mip_extent(const u32 extent, const u32 mip) {
    const u32 shifted = mip < 32 ? extent >> mip : 0;
    return shifted > 0 ? shifted : 1;
}

// How many mips a full chain down to 1x1(x1) has.
u32 full_mip_count(u32 width, u32 height, u32 depth);

} // namespace TEXTURE_ASSET

// --- Reading ------------------------------------------------------------------

enum TextureParseError {
    TEXTURE_PARSE_OK = 0,
    TEXTURE_PARSE_NOT_A_TEXTURE,        // the file's type is not ASSET_TYPE::TEXTURE
    TEXTURE_PARSE_MISSING_CHUNK,        // no TEX2 or no PIXL chunk
    TEXTURE_PARSE_UNSUPPORTED_VERSION,  // TEX2 or PIXL has a version this build does not read
    TEXTURE_PARSE_BAD_DESC,             // TextureDesc is inconsistent (see docs/asset_format.md)
    TEXTURE_PARSE_BAD_MIP,              // a mip entry has wrong dimensions, pitch, size or offset
};

namespace TEXTURE_ASSET {

const char* parse_error_name(TextureParseError error);

}

// Non-owning view over a TEX2 payload. parse() validates it against the
// file's chunk table (the PIXL entry gives the pixel size without reading a
// pixel) and points into the payload bytes, which must outlive the view.
struct TextureAssetView {
    const TextureDesc* desc = nullptr;
    const TextureMip* mips = nullptr; // desc->mip_count entries
    // The PIXL entry in `file`'s chunk table: read it with AssetReader to get
    // the bytes that mip_data and layer_data index into.
    const ChunkEntry* pixels_chunk = nullptr;

    // `file` is the parsed prelude; `payload` is the TEX2 chunk's bytes
    // (`size` of them, 8-byte aligned, as read_chunk returns them). On any
    // error the view is reset to empty.
    TextureParseError parse(const AssetView& file, const void* payload, usz size);
    void reset();

    bool is_parsed() const { return this->desc != nullptr; }
    u32 mip_count() const { return this->desc != nullptr ? this->desc->mip_count : 0; }
    // Bytes of the PIXL chunk: what to allocate before reading it.
    u64 pixels_size() const { return this->pixels_chunk != nullptr ? this->pixels_chunk->size : 0; }

    // Start of mip `mip` (layer 0) inside `pixels`, the PIXL payload, or
    // nullptr when `mip` is out of range.
    const u8* mip_data(const u8* pixels, u32 mip) const;
    // Start of one layer of one mip inside `pixels`. Only meaningful for
    // TEXTURE_COMPRESSION_NONE, where layers are at fixed strides.
    const u8* layer_data(const u8* pixels, u32 mip, u32 layer) const;
    // Bytes of one decoded layer of `mip`.
    u64 layer_size(u32 mip) const;
};
