#include "support/test_support.hpp"

#include "engine/asset/asset_view.hpp"
#include "engine/asset/asset_writer.hpp"
#include "engine/asset/asset_reader.hpp"
#include "engine/asset/asset_types/texture_asset.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"

#include <cstring>
#include <filesystem>
#include <string>

namespace {

const AssetGuid TEXTURE_GUID = {0x1234, 0x5678};

std::string temp_asset_path(const char* stem) {
    return (std::filesystem::temp_directory_path() / (std::string("luau_engine_texture_") + stem + ASSET_FILE::EXTENSION)).string();
}

// Builds a texture the way an importer will: a desc with a mip chain laid
// out tightly (16-byte aligned) in PIXL, pixel bytes filled with a
// recognisable pattern. Tests mutate the desc and mips before writing.
struct TextureBuilder {
    TextureDesc desc;
    DynamicArray<TextureMip> mips;
    DynamicArray<u8> pixels;

    void init(const u32 width, const u32 height, const u32 depth, const u32 layers, const u32 format, const u32 dimension, const u32 mip_count) {
        this->desc.width = width;
        this->desc.height = height;
        this->desc.depth = depth;
        this->desc.layers = layers;
        this->desc.format = format;
        this->desc.dimension = dimension;
        this->desc.mip_count = mip_count;

        u64 offset = 0;
        for (u32 level = 0; level < mip_count; ++level) {
            TextureMip mip;
            mip.width = TEXTURE_ASSET::mip_extent(width, level);
            mip.height = TEXTURE_ASSET::mip_extent(height, level);
            mip.depth = TEXTURE_ASSET::mip_extent(depth, level);
            mip.row_pitch = TEXTURE_FORMAT::row_pitch(format, mip.width);
            mip.size = TEXTURE_FORMAT::layer_size(format, mip.width, mip.height, mip.depth) * layers;
            mip.offset = offset;
            offset = ASSET_FILE::align_up(offset + mip.size, TEXTURE_ASSET::MIP_ALIGNMENT);
            this->desc.decoded_size += mip.size;
            this->mips.push(mip);
        }
        const usz last_end = this->mips[mip_count - 1].offset + this->mips[mip_count - 1].size;
        for (usz i = 0; i < last_end; ++i) {
            this->pixels.push(static_cast<u8>(i * 7 + 3));
        }
    }

    // The TEX2 payload: desc + mip table, in a fresh aligned heap buffer the
    // caller frees. This is what read_chunk hands a loader.
    u8* build_payload(usz* out_size) const {
        const usz size = sizeof(TextureDesc) + sizeof(TextureMip) * this->mips.count;
        u8* payload = static_cast<u8*>(MEMORY::heap_allocator()->allocate(size, ASSET_FILE::PAYLOAD_ALIGNMENT));
        std::memcpy(payload, &this->desc, sizeof(TextureDesc));
        if (this->mips.count > 0) {
            std::memcpy(payload + sizeof(TextureDesc), this->mips.data, sizeof(TextureMip) * this->mips.count);
        }
        *out_size = size;
        return payload;
    }

    void write(AssetWriter& writer, const u32 version = TEXTURE_ASSET::VERSION) const {
        writer.type = ASSET_TYPE::TEXTURE;
        writer.guid = TEXTURE_GUID;
        usz payload_size = 0;
        u8* payload = this->build_payload(&payload_size);
        writer.add_chunk(CHUNK_TYPE::TEXTURE, version, 0, payload, payload_size);
        writer.add_chunk(CHUNK_TYPE::PIXELS, version, 0, this->pixels.data, this->pixels.count);
        MEMORY::heap_allocator()->free(payload);
    }

    void free() {
        this->mips.free();
        this->pixels.free();
    }
};

// Writes the file in memory, parses its prelude into `file` and the TEX2
// payload into `texture`, as a loader would after read_prelude and
// read_chunk. The file buffer and the payload are left for the caller to free.
TextureParseError round_trip(const TextureBuilder& builder, AssetView& file, TextureAssetView& texture, u8** bytes, u8** payload) {
    AssetWriter writer;
    builder.write(writer);
    usz size = 0;
    *bytes = writer.write(MEMORY::heap_allocator(), &size);
    writer.free();
    REQUIRE(*bytes != nullptr);
    REQUIRE((file = AssetView::parse(*bytes, ASSET_FILE::prelude_size(0, 2))).is_ok());
    usz payload_size = 0;
    *payload = builder.build_payload(&payload_size);
    return texture.parse(file, *payload, payload_size);
}

void free_round_trip(u8* bytes, u8* payload) {
    MEMORY::heap_allocator()->free(payload);
    MEMORY::heap_allocator()->free(bytes);
}

} // namespace

TEST_CASE("asset/texture_asset: on-disk structs have their documented sizes") {
    CHECK(sizeof(TextureDesc) == 64);
    CHECK(sizeof(TextureMip) == 32);
    CHECK(TEXTURE_ASSET::desc_size(1) == 96);
    CHECK(TEXTURE_ASSET::desc_size(10) == 64 + 320);
}

TEST_CASE("asset/texture_asset: format info gives block layout and sizes") {
    const TextureFormatInfo rgba8 = TEXTURE_FORMAT::info(TEXTURE_FORMAT_RGBA8_UNORM);
    CHECK(rgba8.is_valid());
    CHECK_FALSE(rgba8.is_block_compressed());
    CHECK_FALSE(rgba8.srgb);
    CHECK(rgba8.block_bytes == 4);
    CHECK(TEXTURE_FORMAT::info(TEXTURE_FORMAT_RGBA8_SRGB).srgb);

    const TextureFormatInfo bc7 = TEXTURE_FORMAT::info(TEXTURE_FORMAT_BC7_SRGB);
    CHECK(bc7.is_block_compressed());
    CHECK(bc7.block_width == 4);
    CHECK(bc7.block_height == 4);
    CHECK(bc7.block_bytes == 16);
    CHECK(bc7.srgb);

    CHECK_FALSE(TEXTURE_FORMAT::info(TEXTURE_FORMAT_NONE).is_valid());
    CHECK_FALSE(TEXTURE_FORMAT::info(0xffff).is_valid());
    CHECK(doctest::String(TEXTURE_FORMAT::name(TEXTURE_FORMAT_BC5_UNORM)) == "BC5_UNORM");
    CHECK(doctest::String(TEXTURE_FORMAT::name(0xffff)) == "unknown");

    // Uncompressed: width * bytes per texel.
    CHECK(TEXTURE_FORMAT::row_pitch(TEXTURE_FORMAT_RGBA8_UNORM, 256) == 1024);
    CHECK(TEXTURE_FORMAT::layer_size(TEXTURE_FORMAT_RGBA8_UNORM, 256, 128, 1) == 256 * 128 * 4);
    CHECK(TEXTURE_FORMAT::layer_size(TEXTURE_FORMAT_R32_FLOAT, 4, 4, 4) == 4 * 4 * 4 * 4);
    // Block compressed: rounded up to whole blocks, so a 1x1 mip is one block.
    CHECK(TEXTURE_FORMAT::row_pitch(TEXTURE_FORMAT_BC1_RGB_UNORM, 256) == 64 * 8);
    CHECK(TEXTURE_FORMAT::row_pitch(TEXTURE_FORMAT_BC7_UNORM, 1) == 16);
    CHECK(TEXTURE_FORMAT::layer_size(TEXTURE_FORMAT_BC7_UNORM, 6, 6, 1) == 2 * 2 * 16);
    CHECK(TEXTURE_FORMAT::layer_size(TEXTURE_FORMAT_NONE, 6, 6, 1) == 0);
}

TEST_CASE("asset/texture_asset: mip extents halve and clamp at one") {
    CHECK(TEXTURE_ASSET::mip_extent(256, 0) == 256);
    CHECK(TEXTURE_ASSET::mip_extent(256, 1) == 128);
    CHECK(TEXTURE_ASSET::mip_extent(256, 8) == 1);
    CHECK(TEXTURE_ASSET::mip_extent(256, 9) == 1);
    CHECK(TEXTURE_ASSET::mip_extent(5, 1) == 2);
    CHECK(TEXTURE_ASSET::mip_extent(5, 2) == 1);
    CHECK(TEXTURE_ASSET::mip_extent(1, 40) == 1);

    CHECK(TEXTURE_ASSET::full_mip_count(1, 1, 1) == 1);
    CHECK(TEXTURE_ASSET::full_mip_count(256, 256, 1) == 9);
    CHECK(TEXTURE_ASSET::full_mip_count(256, 16, 1) == 9);
    CHECK(TEXTURE_ASSET::full_mip_count(5, 3, 1) == 3);
    CHECK(TEXTURE_ASSET::full_mip_count(1, 1, 64) == 7);
    CHECK(TEXTURE_ASSET::full_mip_count(32768, 32768, 1) == TEXTURE_ASSET::MAX_MIPS);
}

TEST_CASE("asset/texture_asset: a view starts empty and rejects files of other types") {
    TextureAssetView texture;
    CHECK_FALSE(texture.is_parsed());
    CHECK(texture.mip_count() == 0);
    CHECK(texture.pixels_size() == 0);
    CHECK(texture.mip_data(nullptr, 0) == nullptr);
    CHECK(texture.layer_size(0) == 0);

    AssetView empty;
    TextureDesc desc;
    CHECK(texture.parse(empty, &desc, sizeof(desc)) == TEXTURE_PARSE_NOT_A_TEXTURE);

    AssetWriter writer;
    writer.type = ASSET_TYPE::MESH;
    writer.guid = TEXTURE_GUID;
    usz size = 0;
    u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
    writer.free();
    REQUIRE(bytes != nullptr);
    AssetView file;
    REQUIRE((file = AssetView::parse(bytes, size)).is_ok());
    CHECK(texture.parse(file, &desc, sizeof(desc)) == TEXTURE_PARSE_NOT_A_TEXTURE);
    MEMORY::heap_allocator()->free(bytes);

    CHECK(doctest::String(TEXTURE_ASSET::parse_error_name(TEXTURE_PARSE_OK)) == "ok");
    CHECK(doctest::String(TEXTURE_ASSET::parse_error_name(TEXTURE_PARSE_BAD_MIP)) != "ok");
}

TEST_CASE("asset/texture_asset: a 2D texture with a full mip chain parses from its prelude and TEX2 payload") {
    TextureBuilder builder;
    builder.init(8, 4, 1, 1, TEXTURE_FORMAT_RGBA8_SRGB, TEXTURE_DIMENSION_2D, 4);
    CHECK(builder.mips.count == 4);

    AssetView file;
    TextureAssetView texture;
    u8* bytes = nullptr;
    u8* payload = nullptr;
    REQUIRE(round_trip(builder, file, texture, &bytes, &payload) == TEXTURE_PARSE_OK);

    CHECK(texture.is_parsed());
    CHECK(texture.mip_count() == 4);
    CHECK(texture.desc == reinterpret_cast<const TextureDesc*>(payload)); // points into the payload
    CHECK(texture.desc->width == 8);
    CHECK(texture.desc->height == 4);
    CHECK(texture.desc->format == TEXTURE_FORMAT_RGBA8_SRGB);
    CHECK(texture.desc->compression == TEXTURE_COMPRESSION_NONE);
    CHECK(texture.pixels_chunk == file.find_chunk(CHUNK_TYPE::PIXELS));
    CHECK(texture.pixels_size() == builder.pixels.count);

    // 8x4, 4x2, 2x1, 1x1 at 16-byte aligned offsets.
    CHECK(texture.mips[0].size == 128);
    CHECK(texture.mips[0].offset == 0);
    CHECK(texture.mips[1].size == 32);
    CHECK(texture.mips[1].offset == 128);
    CHECK(texture.mips[2].size == 8);
    CHECK(texture.mips[2].offset == 160);
    CHECK(texture.mips[3].size == 4);
    CHECK(texture.mips[3].offset == 176);
    CHECK(texture.mips[3].width == 1);
    CHECK(texture.mips[3].height == 1);
    CHECK(texture.mips[3].row_pitch == 4);
    CHECK(texture.desc->decoded_size == 128 + 32 + 8 + 4);
    CHECK(texture.layer_size(0) == 128);
    CHECK(texture.layer_size(3) == 4);
    CHECK(texture.layer_size(4) == 0);

    // Given the PIXL bytes (here straight from the builder), mip_data indexes them.
    const u8* pixels = builder.pixels.data;
    const u8* mip1 = texture.mip_data(pixels, 1);
    REQUIRE(mip1 != nullptr);
    CHECK(mip1 == pixels + 128);
    CHECK(mip1[0] == static_cast<u8>(128 * 7 + 3));
    CHECK(texture.mip_data(pixels, 4) == nullptr);
    CHECK(texture.mip_data(nullptr, 0) == nullptr);
    CHECK(texture.layer_data(pixels, 0, 0) == pixels);
    CHECK(texture.layer_data(pixels, 0, 1) == nullptr);

    free_round_trip(bytes, payload);
    builder.free();
}

TEST_CASE("asset/texture_asset: loading through a file reads only the chunks asked for") {
    const std::string path = temp_asset_path("load");
    TextureBuilder builder;
    builder.init(4, 4, 1, 1, TEXTURE_FORMAT_RG8_UNORM, TEXTURE_DIMENSION_2D, 3);

    AssetWriter writer;
    builder.write(writer);
    usz size = 0;
    u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
    writer.free();
    REQUIRE(bytes != nullptr);
    REQUIRE(ASSET_FILE::write_file(path.c_str(), bytes, size));
    MEMORY::heap_allocator()->free(bytes);

    // The scan: just the prelude.
    AssetView file = ASSET_FILE::read_prelude(path.c_str(), MEMORY::heap_allocator());
    REQUIRE(file.is_ok());

    // The load: TEX2 first, then PIXL into a buffer sized from the view.
    AssetReader reader;
    REQUIRE(reader.open(path.c_str()));
    ReadChunk desc_chunk = reader.read_chunk(file, CHUNK_TYPE::TEXTURE, MEMORY::heap_allocator());
    REQUIRE(desc_chunk.is_ok());
    REQUIRE(desc_chunk.chunk_data != nullptr);
    CHECK(desc_chunk.entry.tag == CHUNK_TYPE::TEXTURE);

    TextureAssetView texture;
    REQUIRE(texture.parse(file, desc_chunk) == TEXTURE_PARSE_OK);
    CHECK(texture.mip_count() == 3);
    CHECK(texture.pixels_size() == builder.pixels.count);

    u8* pixels = static_cast<u8*>(MEMORY::heap_allocator()->allocate(texture.pixels_size(), ASSET_FILE::PAYLOAD_ALIGNMENT));
    REQUIRE(reader.read_chunk(*texture.pixels_chunk, pixels));
    reader.close();
    CHECK(std::memcmp(pixels, builder.pixels.data, builder.pixels.count) == 0);
    CHECK(texture.mip_data(pixels, 2) == pixels + texture.mips[2].offset);
    CHECK(texture.mip_data(pixels, 2)[0] == static_cast<u8>(texture.mips[2].offset * 7 + 3));

    MEMORY::heap_allocator()->free(pixels);
    MEMORY::heap_allocator()->free(desc_chunk.chunk_data);
    ASSET_FILE::free_prelude(&file, MEMORY::heap_allocator());
    std::filesystem::remove(path);
    builder.free();
}

TEST_CASE("asset/texture_asset: block compressed, cube and 3D textures validate") {
    SUBCASE("BC7 with a partial chain down to a single block") {
        TextureBuilder builder;
        builder.init(12, 12, 1, 1, TEXTURE_FORMAT_BC7_UNORM, TEXTURE_DIMENSION_2D, 3);
        AssetView file;
        TextureAssetView texture;
        u8* bytes = nullptr;
        u8* payload = nullptr;
        CHECK(round_trip(builder, file, texture, &bytes, &payload) == TEXTURE_PARSE_OK);
        CHECK(texture.mips[0].size == 3 * 3 * 16); // 12x12 is 3x3 blocks
        CHECK(texture.mips[1].size == 2 * 2 * 16); // 6x6 rounds up to 2x2 blocks
        CHECK(texture.mips[2].size == 16);         // 3x3 is one block
        CHECK(texture.mips[2].offset % TEXTURE_ASSET::MIP_ALIGNMENT == 0);
        free_round_trip(bytes, payload);
        builder.free();
    }

    SUBCASE("cube map: six layers per mip, layers at fixed strides") {
        TextureBuilder builder;
        builder.init(4, 4, 1, 6, TEXTURE_FORMAT_R8_UNORM, TEXTURE_DIMENSION_CUBE, 2);
        AssetView file;
        TextureAssetView texture;
        u8* bytes = nullptr;
        u8* payload = nullptr;
        CHECK(round_trip(builder, file, texture, &bytes, &payload) == TEXTURE_PARSE_OK);
        const u8* pixels = builder.pixels.data;
        CHECK(texture.mips[0].size == 16 * 6);
        CHECK(texture.layer_size(0) == 16);
        CHECK(texture.layer_data(pixels, 0, 5) == pixels + 16 * 5);
        CHECK(texture.layer_data(pixels, 0, 6) == nullptr);
        CHECK(texture.layer_data(pixels, 1, 1) == pixels + texture.mips[1].offset + 4);
        free_round_trip(bytes, payload);
        builder.free();
    }

    SUBCASE("3D: depth halves with the other extents") {
        TextureBuilder builder;
        builder.init(4, 2, 8, 1, TEXTURE_FORMAT_R16_FLOAT, TEXTURE_DIMENSION_3D, 4);
        AssetView file;
        TextureAssetView texture;
        u8* bytes = nullptr;
        u8* payload = nullptr;
        CHECK(round_trip(builder, file, texture, &bytes, &payload) == TEXTURE_PARSE_OK);
        CHECK(texture.mips[0].depth == 8);
        CHECK(texture.mips[1].depth == 4);
        CHECK(texture.mips[3].depth == 1);
        CHECK(texture.mips[0].size == 4 * 2 * 8 * 2);
        free_round_trip(bytes, payload);
        builder.free();
    }
}

TEST_CASE("asset/texture_asset: parse rejects missing chunks, unknown versions and a payload of the wrong size") {
    TextureBuilder builder;
    builder.init(4, 4, 1, 1, TEXTURE_FORMAT_RGBA8_UNORM, TEXTURE_DIMENSION_2D, 1);
    usz payload_size = 0;
    u8* payload = builder.build_payload(&payload_size);

    SUBCASE("no PIXL chunk") {
        AssetWriter writer;
        writer.type = ASSET_TYPE::TEXTURE;
        writer.guid = TEXTURE_GUID;
        writer.add_chunk(CHUNK_TYPE::TEXTURE, TEXTURE_ASSET::VERSION, 0, payload, payload_size);
        usz size = 0;
        u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
        writer.free();
        AssetView file;
        REQUIRE((file = AssetView::parse(bytes, size)).is_ok());
        TextureAssetView texture;
        CHECK(texture.parse(file, payload, payload_size) == TEXTURE_PARSE_MISSING_CHUNK);
        MEMORY::heap_allocator()->free(bytes);
    }

    SUBCASE("placeholder chunks from before the layout existed (version 0, size 0)") {
        AssetWriter writer;
        writer.type = ASSET_TYPE::TEXTURE;
        writer.guid = TEXTURE_GUID;
        writer.add_chunk(CHUNK_TYPE::TEXTURE, 0, 0, nullptr, 0);
        writer.add_chunk(CHUNK_TYPE::PIXELS, 0, 0, nullptr, 0);
        usz size = 0;
        u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
        writer.free();
        AssetView file;
        REQUIRE((file = AssetView::parse(bytes, size)).is_ok());
        TextureAssetView texture;
        CHECK(texture.parse(file, nullptr, 0) == TEXTURE_PARSE_UNSUPPORTED_VERSION);
        MEMORY::heap_allocator()->free(bytes);
    }

    SUBCASE("a future version") {
        AssetWriter writer;
        builder.write(writer, TEXTURE_ASSET::VERSION + 1);
        usz size = 0;
        u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
        writer.free();
        AssetView file;
        REQUIRE((file = AssetView::parse(bytes, size)).is_ok());
        TextureAssetView texture;
        CHECK(texture.parse(file, payload, payload_size) == TEXTURE_PARSE_UNSUPPORTED_VERSION);
        MEMORY::heap_allocator()->free(bytes);
    }

    SUBCASE("a payload that is not the TEX2 chunk's size, or missing") {
        AssetWriter writer;
        builder.write(writer);
        usz size = 0;
        u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
        writer.free();
        AssetView file;
        REQUIRE((file = AssetView::parse(bytes, size)).is_ok());
        TextureAssetView texture;
        CHECK(texture.parse(file, payload, payload_size - 1) == TEXTURE_PARSE_BAD_DESC);
        CHECK(texture.parse(file, nullptr, payload_size) == TEXTURE_PARSE_BAD_DESC);
        CHECK(texture.parse(file, payload, payload_size) == TEXTURE_PARSE_OK);
        MEMORY::heap_allocator()->free(bytes);
    }

    MEMORY::heap_allocator()->free(payload);
    builder.free();
}

TEST_CASE("asset/texture_asset: parse rejects inconsistent descs") {
    TextureBuilder builder;
    builder.init(8, 8, 1, 1, TEXTURE_FORMAT_RGBA8_UNORM, TEXTURE_DIMENSION_2D, 2);

    SUBCASE("zero width") { builder.desc.width = 0; }
    SUBCASE("zero layers") { builder.desc.layers = 0; }
    SUBCASE("zero mips") { builder.desc.mip_count = 0; }
    SUBCASE("more mips than the chain has") { builder.desc.mip_count = 5; }
    SUBCASE("unknown format") { builder.desc.format = 0xbeef; }
    SUBCASE("unknown dimension") { builder.desc.dimension = 7; }
    SUBCASE("compression is reserved") { builder.desc.compression = 1; }
    SUBCASE("flags must be zero") { builder.desc.flags = 1; }
    SUBCASE("reserved must be zero") { builder.desc.reserved[4] = 1; }
    SUBCASE("2D with depth") { builder.desc.depth = 2; }
    SUBCASE("cube needs six layers per cube") { builder.desc.dimension = TEXTURE_DIMENSION_CUBE; }
    SUBCASE("3D with layers") {
        builder.desc.dimension = TEXTURE_DIMENSION_3D;
        builder.desc.layers = 2;
    }
    SUBCASE("decoded_size disagrees with the mips") { builder.desc.decoded_size += 1; }
    SUBCASE("mip table shorter than mip_count") {
        builder.mips.count -= 1; // the chunk is written with one entry fewer
    }

    AssetView file;
    TextureAssetView texture;
    u8* bytes = nullptr;
    u8* payload = nullptr;
    CHECK(round_trip(builder, file, texture, &bytes, &payload) == TEXTURE_PARSE_BAD_DESC);
    CHECK_FALSE(texture.is_parsed());

    free_round_trip(bytes, payload);
    builder.mips.count = 2;
    builder.free();
}

TEST_CASE("asset/texture_asset: parse rejects inconsistent mip entries") {
    TextureBuilder builder;
    builder.init(8, 8, 1, 2, TEXTURE_FORMAT_BC1_RGB_UNORM, TEXTURE_DIMENSION_2D, 3);

    SUBCASE("wrong extent") { builder.mips[1].width = 8; }
    SUBCASE("wrong pitch") { builder.mips[0].row_pitch += 8; }
    SUBCASE("size not the decoded size for every layer") { builder.mips[0].size -= 8; }
    SUBCASE("misaligned offset") {
        builder.mips[1].offset += 8;
        builder.mips[2].offset += 16;
        builder.pixels.push(0);
        builder.pixels.push(0);
    }
    SUBCASE("overlapping the previous mip") { builder.mips[1].offset = 0; }
    SUBCASE("past the end of PIXL") { builder.mips[2].offset += 64; }

    AssetView file;
    TextureAssetView texture;
    u8* bytes = nullptr;
    u8* payload = nullptr;
    CHECK(round_trip(builder, file, texture, &bytes, &payload) == TEXTURE_PARSE_BAD_MIP);
    CHECK_FALSE(texture.is_parsed());

    free_round_trip(bytes, payload);
    builder.free();
}
