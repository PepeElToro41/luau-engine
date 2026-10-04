#include "support/test_support.hpp"

#include "engine/asset/asset_file.hpp"
#include "engine/asset/texture_asset.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"

#include <cstring>

namespace {

const AssetGuid TEXTURE_GUID = {0x1234, 0x5678};

// A source with one byte pattern per layer: texel (x, y, z) of layer l holds
// `l * 16 + z * 4 + y * 2 + x` in every channel so filtered values are
// predictable. Call free() when done.
struct SourceImage {
    DynamicArray<u8> bytes;
    TextureSource source;

    void init(const u32 width, const u32 height, const u32 depth, const u32 layers, const u32 format, const u32 dimension) {
        const TextureFormatInfo info = TEXTURE_FORMAT::info(format);
        const usz layer_size = static_cast<usz>(TEXTURE_FORMAT::layer_size(format, width, height, depth));
        this->bytes.resize(layer_size * layers);
        for (u32 layer = 0; layer < layers; ++layer) {
            for (u32 z = 0; z < depth; ++z) {
                for (u32 y = 0; y < height; ++y) {
                    for (u32 x = 0; x < width; ++x) {
                        const usz texel = layer_size * layer + ((static_cast<usz>(z) * height + y) * width + x) * info.block_bytes;
                        for (u32 byte = 0; byte < info.block_bytes; ++byte) {
                            this->bytes[texel + byte] = static_cast<u8>(layer * 16 + z * 4 + y * 2 + x);
                        }
                    }
                }
            }
        }
        this->source.width = width;
        this->source.height = height;
        this->source.depth = depth;
        this->source.layers = layers;
        this->source.format = format;
        this->source.dimension = dimension;
        this->source.pixels = this->bytes.data;
        this->source.pixels_size = this->bytes.count;
    }

    void free() { this->bytes.free(); }
};

// Puts the writer's chunks in a file in memory and parses it back the way a
// loader does; the TEX2 payload is handed to the view straight from the file
// bytes. The file buffer is left for the caller to free.
TextureParseError round_trip(const TextureAssetWriter& texture, AssetView& file, TextureAssetView& view, u8** bytes) {
    AssetWriter writer;
    writer.type = ASSET_TYPE::TEXTURE;
    writer.guid = TEXTURE_GUID;
    texture.add_chunks(writer);
    usz size = 0;
    *bytes = writer.write(MEMORY::heap_allocator(), &size);
    writer.free();
    REQUIRE(*bytes != nullptr);
    REQUIRE(file.parse(*bytes, size) == ASSET_PARSE_OK);
    const ChunkEntry* desc_chunk = file.find_chunk(CHUNK_TAG::TEXTURE);
    REQUIRE(desc_chunk != nullptr);
    return view.parse(file, *bytes + desc_chunk->offset, static_cast<usz>(desc_chunk->size));
}

} // namespace

TEST_CASE("asset/texture_asset_writer: a writer starts empty and adds nothing until built") {
    TextureAssetWriter texture;
    CHECK_FALSE(texture.is_built());
    CHECK(texture.desc_size() == sizeof(TextureDesc));

    AssetWriter writer;
    texture.add_chunks(writer);
    CHECK(writer.chunk_count() == 0);
    writer.free();
    texture.free();
}

TEST_CASE("asset/texture_asset_writer: a 2D RGBA8 image gets a full mip chain that parses back") {
    SourceImage image;
    image.init(8, 4, 1, 1, TEXTURE_FORMAT_RGBA8_UNORM, TEXTURE_DIMENSION_2D);
    TextureAssetWriter texture;
    REQUIRE(texture.build(image.source, TextureImportOptions{}) == TEXTURE_WRITE_OK);
    CHECK(texture.is_built());

    // 8x4 -> 4x2 -> 2x1 -> 1x1.
    REQUIRE(texture.desc.mip_count == 4);
    CHECK(texture.mips.count == 4);
    CHECK(texture.desc.format == TEXTURE_FORMAT_RGBA8_UNORM);
    CHECK(texture.desc.compression == TEXTURE_COMPRESSION_NONE);
    CHECK(texture.desc.decoded_size == (32 + 8 + 2 + 1) * 4);
    CHECK(texture.desc_size() == TEXTURE_ASSET::desc_size(4));

    // Mip 0 is the source verbatim; every level starts 16-byte aligned.
    CHECK(texture.mips[0].offset == 0);
    CHECK(texture.mips[0].size == 8 * 4 * 4);
    CHECK(texture.mips[0].row_pitch == 32);
    CHECK(std::memcmp(texture.pixels.data, image.bytes.data, image.bytes.count) == 0);
    CHECK(texture.mips[1].offset == 128);
    CHECK(texture.mips[1].width == 4);
    CHECK(texture.mips[1].height == 2);
    CHECK(texture.mips[2].offset == 160);
    CHECK(texture.mips[3].offset == 176);
    CHECK(texture.mips[3].width == 1);
    CHECK(texture.mips[3].height == 1);
    CHECK(texture.pixels.count == 176 + 4);

    // Mip 1 texel (0, 0) averages source texels (0,0)=0, (1,0)=1, (0,1)=2,
    // (1,1)=3: 1.5, rounded to 2.
    CHECK(texture.pixels[texture.mips[1].offset] == 2);
    // Mip 1 texel (1, 0) averages 2, 3, 4, 5.
    CHECK(texture.pixels[texture.mips[1].offset + 4] == 4);

    AssetView file;
    TextureAssetView view;
    u8* bytes = nullptr;
    CHECK(round_trip(texture, file, view, &bytes) == TEXTURE_PARSE_OK);
    CHECK(view.mip_count() == 4);
    CHECK(view.pixels_size() == texture.pixels.count);
    CHECK(view.mips[1].offset == 128);
    CHECK(view.layer_size(2) == 2 * 4);
    MEMORY::heap_allocator()->free(bytes);

    texture.free();
    image.free();
}

TEST_CASE("asset/texture_asset_writer: generate_mips off writes mip 0 alone") {
    SourceImage image;
    image.init(8, 8, 1, 1, TEXTURE_FORMAT_RG8_UNORM, TEXTURE_DIMENSION_2D);
    TextureImportOptions options;
    options.generate_mips = false;
    TextureAssetWriter texture;
    REQUIRE(texture.build(image.source, options) == TEXTURE_WRITE_OK);
    CHECK(texture.desc.mip_count == 1);
    CHECK(texture.pixels.count == image.bytes.count);
    CHECK(texture.desc.decoded_size == image.bytes.count);

    AssetView file;
    TextureAssetView view;
    u8* bytes = nullptr;
    CHECK(round_trip(texture, file, view, &bytes) == TEXTURE_PARSE_OK);
    MEMORY::heap_allocator()->free(bytes);
    texture.free();
    image.free();
}

TEST_CASE("asset/texture_asset_writer: srgb picks the sRGB format and filters in linear space") {
    CHECK(TEXTURE_FORMAT::srgb_variant(TEXTURE_FORMAT_RGBA8_UNORM) == TEXTURE_FORMAT_RGBA8_SRGB);
    CHECK(TEXTURE_FORMAT::srgb_variant(TEXTURE_FORMAT_RGBA8_SRGB) == TEXTURE_FORMAT_RGBA8_SRGB);
    CHECK(TEXTURE_FORMAT::srgb_variant(TEXTURE_FORMAT_BC7_UNORM) == TEXTURE_FORMAT_BC7_SRGB);
    CHECK(TEXTURE_FORMAT::srgb_variant(TEXTURE_FORMAT_R8_UNORM) == TEXTURE_FORMAT_R8_UNORM);
    CHECK(TEXTURE_FORMAT::srgb_variant(TEXTURE_FORMAT_R32_FLOAT) == TEXTURE_FORMAT_R32_FLOAT);

    // A 2x1 image: one black texel, one white texel, alpha 0 and 255.
    const u8 pixels[8] = {0, 0, 0, 0, 255, 255, 255, 255};
    TextureSource source;
    source.width = 2;
    source.height = 1;
    source.format = TEXTURE_FORMAT_RGBA8_UNORM;
    source.pixels = pixels;
    source.pixels_size = sizeof(pixels);
    TextureImportOptions options;
    options.srgb = true;

    TextureAssetWriter texture;
    REQUIRE(texture.build(source, options) == TEXTURE_WRITE_OK);
    CHECK(texture.desc.format == TEXTURE_FORMAT_RGBA8_SRGB);
    REQUIRE(texture.desc.mip_count == 2);
    // Linear average of 0 and 1 is 0.5, which encodes to 188 in sRGB, not
    // the 128 a byte average would give. Alpha is linear: 128.
    const u8* mip1 = texture.pixels.data + texture.mips[1].offset;
    CHECK(mip1[0] == 188);
    CHECK(mip1[1] == 188);
    CHECK(mip1[2] == 188);
    CHECK(mip1[3] == 128);

    // The same image without srgb averages the bytes.
    options.srgb = false;
    REQUIRE(texture.build(source, options) == TEXTURE_WRITE_OK);
    CHECK(texture.desc.format == TEXTURE_FORMAT_RGBA8_UNORM);
    CHECK(texture.pixels[texture.mips[1].offset] == 128);
    texture.free();
}

TEST_CASE("asset/texture_asset_writer: 16-bit and float formats filter through their encodings") {
    SUBCASE("R16_UNORM") {
        const u16 pixels[4] = {0, 65535, 0, 65535};
        TextureSource source;
        source.width = 2;
        source.height = 2;
        source.format = TEXTURE_FORMAT_R16_UNORM;
        source.pixels = pixels;
        source.pixels_size = sizeof(pixels);
        TextureAssetWriter texture;
        REQUIRE(texture.build(source, TextureImportOptions{}) == TEXTURE_WRITE_OK);
        REQUIRE(texture.desc.mip_count == 2);
        u16 value;
        std::memcpy(&value, texture.pixels.data + texture.mips[1].offset, sizeof(value));
        CHECK(value == 32768);
        texture.free();
    }
    SUBCASE("RG32_FLOAT") {
        const f32 pixels[8] = {1, -2, 3, -4, 5, -6, 7, -8};
        TextureSource source;
        source.width = 2;
        source.height = 2;
        source.format = TEXTURE_FORMAT_RG32_FLOAT;
        source.pixels = pixels;
        source.pixels_size = sizeof(pixels);
        TextureAssetWriter texture;
        REQUIRE(texture.build(source, TextureImportOptions{}) == TEXTURE_WRITE_OK);
        REQUIRE(texture.desc.mip_count == 2);
        f32 value[2];
        std::memcpy(value, texture.pixels.data + texture.mips[1].offset, sizeof(value));
        CHECK(value[0] == doctest::Approx(4.0f));
        CHECK(value[1] == doctest::Approx(-5.0f));
        texture.free();
    }
    SUBCASE("R16_FLOAT") {
        // 1.0 = 0x3c00, 3.0 = 0x4200; their average 2.0 = 0x4000. The last
        // two are 0.5 = 0x3800 and a subnormal (2^-24 = 0x0001), whose
        // average rounds to nearest even.
        const u16 pixels[4] = {0x3c00, 0x4200, 0x3c00, 0x4200};
        TextureSource source;
        source.width = 2;
        source.height = 2;
        source.format = TEXTURE_FORMAT_R16_FLOAT;
        source.pixels = pixels;
        source.pixels_size = sizeof(pixels);
        TextureAssetWriter texture;
        REQUIRE(texture.build(source, TextureImportOptions{}) == TEXTURE_WRITE_OK);
        u16 value;
        std::memcpy(&value, texture.pixels.data + texture.mips[1].offset, sizeof(value));
        CHECK(value == 0x4000);

        const u16 small[4] = {0x3800, 0x0001, 0x3800, 0x0001};
        source.pixels = small;
        REQUIRE(texture.build(source, TextureImportOptions{}) == TEXTURE_WRITE_OK);
        std::memcpy(&value, texture.pixels.data + texture.mips[1].offset, sizeof(value));
        // (0.5 + 2^-24) / 2 = 0.25 + 2^-25, which is 0.25 (0x3400) in half.
        CHECK(value == 0x3400);
        texture.free();
    }
}

TEST_CASE("asset/texture_asset_writer: array, cube and 3D sources filter every layer and slice") {
    SUBCASE("2D array keeps layers apart") {
        SourceImage image;
        image.init(2, 2, 1, 3, TEXTURE_FORMAT_R8_UNORM, TEXTURE_DIMENSION_2D);
        TextureAssetWriter texture;
        REQUIRE(texture.build(image.source, TextureImportOptions{}) == TEXTURE_WRITE_OK);
        REQUIRE(texture.desc.mip_count == 2);
        CHECK(texture.desc.layers == 3);
        CHECK(texture.mips[1].size == 3);
        // Layer l's 2x2 holds l*16 + {0, 1, 2, 3}; the 1x1 mip averages to
        // l*16 + 1.5, rounded to l*16 + 2.
        const u8* mip1 = texture.pixels.data + texture.mips[1].offset;
        CHECK(mip1[0] == 2);
        CHECK(mip1[1] == 18);
        CHECK(mip1[2] == 34);

        AssetView file;
        TextureAssetView view;
        u8* bytes = nullptr;
        CHECK(round_trip(texture, file, view, &bytes) == TEXTURE_PARSE_OK);
        CHECK(view.desc->layers == 3);
        MEMORY::heap_allocator()->free(bytes);
        texture.free();
        image.free();
    }
    SUBCASE("cube map") {
        SourceImage image;
        image.init(4, 4, 1, 6, TEXTURE_FORMAT_RGBA8_UNORM, TEXTURE_DIMENSION_CUBE);
        TextureAssetWriter texture;
        REQUIRE(texture.build(image.source, TextureImportOptions{}) == TEXTURE_WRITE_OK);
        CHECK(texture.desc.dimension == TEXTURE_DIMENSION_CUBE);
        CHECK(texture.desc.mip_count == 3);
        CHECK(texture.mips[2].size == 6 * 4);

        AssetView file;
        TextureAssetView view;
        u8* bytes = nullptr;
        CHECK(round_trip(texture, file, view, &bytes) == TEXTURE_PARSE_OK);
        MEMORY::heap_allocator()->free(bytes);
        texture.free();
        image.free();
    }
    SUBCASE("3D texture halves its depth too") {
        SourceImage image;
        image.init(2, 2, 2, 1, TEXTURE_FORMAT_R8_UNORM, TEXTURE_DIMENSION_3D);
        TextureAssetWriter texture;
        REQUIRE(texture.build(image.source, TextureImportOptions{}) == TEXTURE_WRITE_OK);
        REQUIRE(texture.desc.mip_count == 2);
        CHECK(texture.mips[1].depth == 1);
        CHECK(texture.mips[1].size == 1);
        // Eight texels: {0, 1, 2, 3} and {4, 5, 6, 7}; mean 3.5 rounds to 4.
        CHECK(texture.pixels[texture.mips[1].offset] == 4);

        AssetView file;
        TextureAssetView view;
        u8* bytes = nullptr;
        CHECK(round_trip(texture, file, view, &bytes) == TEXTURE_PARSE_OK);
        MEMORY::heap_allocator()->free(bytes);
        texture.free();
        image.free();
    }
}

TEST_CASE("asset/texture_asset_writer: odd extents and a 1 wide image follow the halve-and-clamp rule") {
    SourceImage image;
    image.init(5, 1, 1, 1, TEXTURE_FORMAT_R8_UNORM, TEXTURE_DIMENSION_2D);
    TextureAssetWriter texture;
    REQUIRE(texture.build(image.source, TextureImportOptions{}) == TEXTURE_WRITE_OK);
    // 5 -> 2 -> 1.
    REQUIRE(texture.desc.mip_count == 3);
    CHECK(texture.mips[1].width == 2);
    CHECK(texture.mips[1].height == 1);
    // Texels 0..4 hold 0..4; mip 1 is {avg(0,1), avg(2,3)} = {1 (0.5 rounds up), 3 (2.5 rounds up)}; texel 4 is dropped.
    const u8* mip1 = texture.pixels.data + texture.mips[1].offset;
    CHECK(mip1[0] == 1);
    CHECK(mip1[1] == 3);
    CHECK(texture.pixels[texture.mips[2].offset] == 2);

    AssetView file;
    TextureAssetView view;
    u8* bytes = nullptr;
    CHECK(round_trip(texture, file, view, &bytes) == TEXTURE_PARSE_OK);
    MEMORY::heap_allocator()->free(bytes);
    texture.free();
    image.free();
}

TEST_CASE("asset/texture_asset_writer: block compressed sources are written as given, mip 0 only") {
    // A 12x12 BC7 image is 3x3 blocks of 16 bytes.
    u8 blocks[9 * 16];
    for (u32 i = 0; i < sizeof(blocks); ++i) {
        blocks[i] = static_cast<u8>(i * 3);
    }
    TextureSource source;
    source.width = 12;
    source.height = 12;
    source.format = TEXTURE_FORMAT_BC7_UNORM;
    source.pixels = blocks;
    source.pixels_size = sizeof(blocks);
    REQUIRE(source.pixels_size == TEXTURE_FORMAT::layer_size(TEXTURE_FORMAT_BC7_UNORM, 12, 12, 1));

    TextureImportOptions options;
    TextureAssetWriter texture;
    CHECK(texture.build(source, options) == TEXTURE_WRITE_CANNOT_GENERATE_MIPS);
    CHECK_FALSE(texture.is_built());

    options.generate_mips = false;
    options.srgb = true;
    REQUIRE(texture.build(source, options) == TEXTURE_WRITE_OK);
    CHECK(texture.desc.format == TEXTURE_FORMAT_BC7_SRGB);
    CHECK(texture.desc.mip_count == 1);
    CHECK(texture.mips[0].row_pitch == 3 * 16);
    CHECK(texture.pixels.count == 9 * 16);
    CHECK(std::memcmp(texture.pixels.data, blocks, sizeof(blocks)) == 0);

    AssetView file;
    TextureAssetView view;
    u8* bytes = nullptr;
    CHECK(round_trip(texture, file, view, &bytes) == TEXTURE_PARSE_OK);
    MEMORY::heap_allocator()->free(bytes);
    texture.free();
}

TEST_CASE("asset/texture_asset_writer: build rejects a bad source and leaves nothing behind") {
    SourceImage image;
    image.init(4, 4, 1, 1, TEXTURE_FORMAT_RGBA8_UNORM, TEXTURE_DIMENSION_2D);
    TextureAssetWriter texture;
    REQUIRE(texture.build(image.source, TextureImportOptions{}) == TEXTURE_WRITE_OK);

    SUBCASE("zero extent") {
        TextureSource source = image.source;
        source.height = 0;
        CHECK(texture.build(source, TextureImportOptions{}) == TEXTURE_WRITE_BAD_SOURCE);
    }
    SUBCASE("unknown format") {
        TextureSource source = image.source;
        source.format = 0xffff;
        CHECK(texture.build(source, TextureImportOptions{}) == TEXTURE_WRITE_BAD_SOURCE);
    }
    SUBCASE("dimension rules") {
        TextureSource source = image.source;
        source.dimension = TEXTURE_DIMENSION_CUBE; // layers == 1
        CHECK(texture.build(source, TextureImportOptions{}) == TEXTURE_WRITE_BAD_SOURCE);
        source.dimension = TEXTURE_DIMENSION_2D;
        source.depth = 2;
        source.pixels_size *= 2;
        CHECK(texture.build(source, TextureImportOptions{}) == TEXTURE_WRITE_BAD_SOURCE);
        source.dimension = 0xff;
        CHECK(texture.build(source, TextureImportOptions{}) == TEXTURE_WRITE_BAD_SOURCE);
    }
    SUBCASE("pixel size") {
        TextureSource source = image.source;
        source.pixels_size -= 1;
        CHECK(texture.build(source, TextureImportOptions{}) == TEXTURE_WRITE_BAD_SIZE);
        source.pixels_size = image.source.pixels_size;
        source.pixels = nullptr;
        CHECK(texture.build(source, TextureImportOptions{}) == TEXTURE_WRITE_BAD_SIZE);
    }

    // A failed build clears the previous result.
    CHECK_FALSE(texture.is_built());
    CHECK(texture.mips.count == 0);
    CHECK(texture.pixels.count == 0);
    AssetWriter writer;
    texture.add_chunks(writer);
    CHECK(writer.chunk_count() == 0);
    writer.free();

    CHECK(doctest::String(TEXTURE_ASSET::write_error_name(TEXTURE_WRITE_BAD_SIZE)) != "unknown error");
    texture.free();
    image.free();
}
