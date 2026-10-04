#include "support/test_support.hpp"

#include "engine/asset/asset_file.hpp"
#include "engine/memory/arena_allocator.hpp"
#include "engine/memory/heap_allocator.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

const AssetGuid WALL_GUID = {0x1111222233334444ull, 0x5555666677778888ull};
const AssetGuid BRICK_GUID = {0xaaaabbbbccccddddull, 0x0000000000000001ull};
const AssetGuid NORMAL_GUID = {0x0000000000000002ull, 0xeeeeffff00001111ull};

bool is_aligned(const void* pointer, const usz alignment) {
    return (reinterpret_cast<uintptr_t>(pointer) & (alignment - 1)) == 0;
}

// A texture asset with a dependency and the common editor chunks, as an
// importer would write it once the payload layouts exist.
void fill_texture(AssetWriter& writer) {
    writer.type = ASSET_TYPE::TEXTURE;
    writer.guid = WALL_GUID;
    writer.content_hash = 0xdeadbeef;
    writer.add_dependency(BRICK_GUID);

    const char name[] = "wall_albedo";
    const u8 settings[3] = {1, 0, 1};
    u8 source[100];
    for (usz i = 0; i < sizeof(source); ++i) {
        source[i] = static_cast<u8>(i);
    }
    const u32 texture_info[4] = {256, 256, 7, 9};
    u8 pixels[300];
    for (usz i = 0; i < sizeof(pixels); ++i) {
        pixels[i] = static_cast<u8>(255 - i % 256);
    }

    writer.add_chunk(CHUNK_TAG::NAME, 1, CHUNK_FLAG::EDITOR_ONLY, name, sizeof(name));
    writer.add_chunk(CHUNK_TAG::IMPORT_SETTINGS, 1, CHUNK_FLAG::EDITOR_ONLY, settings, sizeof(settings));
    writer.add_chunk(CHUNK_TAG::SOURCE, 1, CHUNK_FLAG::EDITOR_ONLY, source, sizeof(source));
    writer.add_chunk(CHUNK_TAG::TEXTURE, 1, 0, texture_info, sizeof(texture_info));
    writer.add_chunk(CHUNK_TAG::PIXELS, 1, 0, pixels, sizeof(pixels));
}

AssetHeader read_header(const u8* bytes) {
    AssetHeader header;
    std::memcpy(&header, bytes, sizeof(AssetHeader));
    return header;
}

void write_header(u8* bytes, const AssetHeader& header) {
    std::memcpy(bytes, &header, sizeof(AssetHeader));
}

usz chunk_entry_offset(const AssetHeader& header, const usz index) {
    return sizeof(AssetHeader) + sizeof(AssetGuid) * header.dependency_count + sizeof(ChunkEntry) * index;
}

ChunkEntry read_chunk(const u8* bytes, const usz index) {
    ChunkEntry chunk;
    std::memcpy(&chunk, bytes + chunk_entry_offset(read_header(bytes), index), sizeof(ChunkEntry));
    return chunk;
}

void write_chunk(u8* bytes, const usz index, const ChunkEntry& chunk) {
    std::memcpy(bytes + chunk_entry_offset(read_header(bytes), index), &chunk, sizeof(ChunkEntry));
}

std::string temp_asset_path(const char* stem) {
    return (std::filesystem::temp_directory_path() / (std::string("luau_engine_") + stem + ASSET_FILE::EXTENSION)).string();
}

} // namespace

TEST_CASE("asset/asset_file: fourcc packs the characters little-endian so they read in file order") {
    const u32 code = ASSET_FILE::fourcc("TEX2");
    u8 bytes[4];
    std::memcpy(bytes, &code, 4);
    CHECK(bytes[0] == 'T');
    CHECK(bytes[1] == 'E');
    CHECK(bytes[2] == 'X');
    CHECK(bytes[3] == '2');

    CHECK(ASSET_TYPE::TEXTURE == ASSET_FILE::fourcc("TEX2"));
    CHECK(CHUNK_TAG::SOURCE == ASSET_FILE::fourcc("SRC "));
    CHECK(ASSET_TYPE::TEXTURE != ASSET_TYPE::MESH);
    CHECK(CHUNK_TAG::TEXTURE == ASSET_TYPE::TEXTURE); // same code, different namespaces by design

    u8 magic[4];
    std::memcpy(magic, &ASSET_FILE::MAGIC, 4);
    CHECK(std::memcmp(magic, "LUAS", 4) == 0);
}

TEST_CASE("asset/asset_file: prelude and payload start follow from the table counts") {
    CHECK(ASSET_FILE::prelude_size(0, 0) == 64);
    CHECK(ASSET_FILE::prelude_size(1, 0) == 80);
    CHECK(ASSET_FILE::prelude_size(0, 1) == 96);
    CHECK(ASSET_FILE::prelude_size(2, 3) == 64 + 32 + 96);

    CHECK(ASSET_FILE::payload_start(0, 0) == 64);
    CHECK(ASSET_FILE::payload_start(1, 0) == 128);
    CHECK(ASSET_FILE::payload_start(0, 2) == 128);
    CHECK(ASSET_FILE::payload_start(2, 3) == 192);
    CHECK(ASSET_FILE::align_up(0, 64) == 0);
    CHECK(ASSET_FILE::align_up(1, 64) == 64);
    CHECK(ASSET_FILE::align_up(64, 64) == 64);
    CHECK(ASSET_FILE::align_up(65, 64) == 128);
}

TEST_CASE("asset/asset_file: a view starts empty and parse rejects garbage") {
    AssetView view;
    CHECK_FALSE(view.is_parsed());
    CHECK_FALSE(view.is_cooked());
    CHECK(view.chunk_count() == 0);
    CHECK(view.dependency_count() == 0);
    CHECK(view.find_chunk(CHUNK_TAG::NAME) == nullptr);
    CHECK_FALSE(view.has_editor_chunks());

    CHECK(view.parse(nullptr, 0) == ASSET_PARSE_TOO_SMALL);

    alignas(64) u8 small[16] = {};
    CHECK(view.parse(small, sizeof(small)) == ASSET_PARSE_TOO_SMALL);

    alignas(64) u8 zeros[128] = {};
    CHECK(view.parse(zeros, sizeof(zeros)) == ASSET_PARSE_BAD_MAGIC);
    CHECK_FALSE(view.is_parsed());

    CHECK(doctest::String(ASSET_FILE::parse_error_name(ASSET_PARSE_OK)) == "ok");
    CHECK(doctest::String(ASSET_FILE::parse_error_name(ASSET_PARSE_BAD_MAGIC)) != "ok");
}

TEST_CASE("asset/asset_file: a writer with no chunks produces a prelude-only file") {
    AssetWriter writer;
    writer.type = ASSET_TYPE::MESH;
    writer.guid = WALL_GUID;
    CHECK(writer.file_size() == 64);

    usz size = 0;
    u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
    REQUIRE(bytes != nullptr);
    CHECK(size == 64);
    CHECK(is_aligned(bytes, ASSET_FILE::PAYLOAD_ALIGNMENT));

    AssetView view;
    REQUIRE(view.parse(bytes, size) == ASSET_PARSE_OK);
    CHECK(view.is_parsed());
    CHECK_FALSE(view.is_cooked());
    CHECK(view.header->magic == ASSET_FILE::MAGIC);
    CHECK(view.header->format_version == ASSET_FILE::FORMAT_VERSION);
    CHECK(view.header->type == ASSET_TYPE::MESH);
    CHECK(view.header->guid == WALL_GUID);
    CHECK(view.header->file_size == 64);
    CHECK(view.header->reserved == 0);
    CHECK(view.chunk_count() == 0);
    CHECK(view.dependency_count() == 0);
    CHECK(view.find_chunk(CHUNK_TAG::MESH) == nullptr);

    MEMORY::heap_allocator()->free(bytes);
    writer.free();
}

TEST_CASE("asset/asset_file: chunks round-trip in order, aligned, with their payloads") {
    AssetWriter writer;
    fill_texture(writer);
    CHECK(writer.chunk_count() == 5);
    CHECK(writer.dependency_count() == 1);

    usz size = 0;
    u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
    REQUIRE(bytes != nullptr);
    CHECK(size == writer.file_size());

    AssetView view;
    REQUIRE(view.parse(bytes, size) == ASSET_PARSE_OK);
    CHECK(view.header->type == ASSET_TYPE::TEXTURE);
    CHECK(view.header->content_hash == 0xdeadbeef);
    CHECK(view.chunk_count() == 5);
    CHECK(view.dependency_count() == 1);
    CHECK(view.dependencies[0] == BRICK_GUID);

    // Table order is payload order and every payload is aligned.
    const usz payload_start = ASSET_FILE::payload_start(1, 5);
    CHECK(view.chunks[0].offset == payload_start);
    u64 previous_end = payload_start;
    for (usz i = 0; i < view.chunk_count(); ++i) {
        const ChunkEntry& chunk = view.chunks[i];
        CHECK(chunk.offset % ASSET_FILE::PAYLOAD_ALIGNMENT == 0);
        CHECK(chunk.offset >= previous_end);
        CHECK(chunk.offset + chunk.size <= view.header->file_size);
        CHECK(chunk.reserved == 0);
        previous_end = chunk.offset + chunk.size;
    }
    // The file ends exactly where the last payload ends.
    CHECK(previous_end == view.header->file_size);

    SUBCASE("find_chunk locates tags and the entries point at the copied bytes") {
        const ChunkEntry* name = view.find_chunk(CHUNK_TAG::NAME);
        REQUIRE(name != nullptr);
        CHECK(name->version == 1);
        CHECK(name->is_editor_only());
        CHECK(name->size == sizeof("wall_albedo"));
        CHECK(doctest::String(reinterpret_cast<const char*>(bytes + name->offset)) == "wall_albedo");

        const ChunkEntry* pixels = view.find_chunk(CHUNK_TAG::PIXELS);
        REQUIRE(pixels != nullptr);
        CHECK_FALSE(pixels->is_editor_only());
        CHECK(pixels->size == 300);
        const u8* data = bytes + pixels->offset;
        CHECK(is_aligned(data, ASSET_FILE::PAYLOAD_ALIGNMENT));
        bool intact = true;
        for (usz i = 0; i < 300; ++i) {
            intact = intact && data[i] == static_cast<u8>(255 - i % 256);
        }
        CHECK(intact);

        const ChunkEntry* texture = view.find_chunk(CHUNK_TAG::TEXTURE);
        REQUIRE(texture != nullptr);
        u32 info[4];
        std::memcpy(info, bytes + texture->offset, sizeof(info));
        CHECK(info[0] == 256);
        CHECK(info[3] == 9);

        CHECK(view.find_chunk(CHUNK_TAG::MESH) == nullptr);
        CHECK(view.find_chunk(CHUNK_TAG::VERTICES) == nullptr);
    }

    SUBCASE("editor chunks are reported and the file is not cooked") {
        CHECK(view.has_editor_chunks());
        CHECK_FALSE(view.is_cooked());
    }

    SUBCASE("padding between payloads is zero") {
        const ChunkEntry* settings = view.find_chunk(CHUNK_TAG::IMPORT_SETTINGS);
        REQUIRE(settings != nullptr);
        CHECK(settings->size == 3);
        const u8* after = bytes + settings->offset + settings->size;
        const u8* next = bytes + ASSET_FILE::align_up(settings->offset + settings->size, ASSET_FILE::PAYLOAD_ALIGNMENT);
        bool zero = true;
        for (const u8* p = after; p < next; ++p) {
            zero = zero && *p == 0;
        }
        CHECK(zero);
    }

    MEMORY::heap_allocator()->free(bytes);
    writer.free();
}

TEST_CASE("asset/asset_file: placeholder chunks with no payload are valid, even as the last chunk") {
    // What the texture and mesh importers write until their payload layouts
    // exist: the right tags, version 0, size 0.
    AssetWriter writer;
    writer.type = ASSET_TYPE::MESH;
    writer.guid = WALL_GUID;
    writer.add_chunk(CHUNK_TAG::MESH, 0, 0, nullptr, 0);
    writer.add_chunk(CHUNK_TAG::VERTICES, 0, 0, nullptr, 0);
    writer.add_chunk(CHUNK_TAG::INDICES, 0, 0, nullptr, 0);
    writer.add_chunk(CHUNK_TAG::BOUNDS, 0, 0, nullptr, 0);
    CHECK(writer.file_size() == ASSET_FILE::payload_start(0, 4));

    usz size = 0;
    u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
    REQUIRE(bytes != nullptr);

    AssetView view;
    REQUIRE(view.parse(bytes, size) == ASSET_PARSE_OK);
    CHECK(view.chunk_count() == 4);
    for (usz i = 0; i < 4; ++i) {
        CHECK(view.chunks[i].size == 0);
        CHECK(view.chunks[i].version == 0);
        // Zero-size payloads still have an aligned, in-bounds offset.
        CHECK(view.chunks[i].offset == view.header->file_size);
    }
    CHECK(view.find_chunk(CHUNK_TAG::BOUNDS) == &view.chunks[3]);

    SUBCASE("an empty chunk after a real one sits at the next aligned offset") {
        writer.clear();
        const u8 payload[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        writer.add_chunk(CHUNK_TAG::MESH, 1, 0, payload, sizeof(payload));
        writer.add_chunk(CHUNK_TAG::VERTICES, 0, 0, nullptr, 0);
        MEMORY::heap_allocator()->free(bytes);
        bytes = writer.write(MEMORY::heap_allocator(), &size);
        REQUIRE(bytes != nullptr);
        REQUIRE(view.parse(bytes, size) == ASSET_PARSE_OK);
        CHECK(view.chunks[0].offset == ASSET_FILE::payload_start(0, 2));
        CHECK(view.chunks[1].offset == ASSET_FILE::payload_start(0, 2) + 64);
        CHECK(view.chunks[1].offset == view.header->file_size);
    }

    MEMORY::heap_allocator()->free(bytes);
    writer.free();
}

TEST_CASE("asset/asset_file: the same tag may repeat and find_chunk walks the matches in order") {
    AssetWriter writer;
    writer.type = ASSET_TYPE::MESH;
    writer.guid = WALL_GUID;
    const u8 positions[12] = {};
    const u8 normals[12] = {};
    writer.add_chunk(CHUNK_TAG::MESH, 1, 0, nullptr, 0);
    writer.add_chunk(CHUNK_TAG::VERTICES, 1, 0, positions, sizeof(positions));
    writer.add_chunk(CHUNK_TAG::INDICES, 1, 0, nullptr, 0);
    writer.add_chunk(CHUNK_TAG::VERTICES, 2, 0, normals, sizeof(normals));

    usz size = 0;
    u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
    REQUIRE(bytes != nullptr);
    AssetView view;
    REQUIRE(view.parse(bytes, size) == ASSET_PARSE_OK);

    const ChunkEntry* first = view.find_chunk(CHUNK_TAG::VERTICES);
    REQUIRE(first != nullptr);
    CHECK(first == &view.chunks[1]);
    CHECK(first->version == 1);
    const ChunkEntry* second = view.find_chunk(CHUNK_TAG::VERTICES, first);
    REQUIRE(second != nullptr);
    CHECK(second == &view.chunks[3]);
    CHECK(second->version == 2);
    CHECK(view.find_chunk(CHUNK_TAG::VERTICES, second) == nullptr);
    CHECK(view.find_chunk(CHUNK_TAG::MESH, first) == nullptr); // only looks after `after`

    MEMORY::heap_allocator()->free(bytes);
    writer.free();
}

TEST_CASE("asset/asset_file: dependencies are deduplicated, indexed and never null") {
    AssetWriter writer;
    writer.type = ASSET_TYPE::TEXTURE;
    writer.guid = WALL_GUID;

    CHECK(writer.add_dependency(BRICK_GUID) == 0);
    CHECK(writer.add_dependency(NORMAL_GUID) == 1);
    CHECK(writer.add_dependency(BRICK_GUID) == 0); // already there
    CHECK(writer.add_dependency(AssetGuid{}) == ~0u);
    CHECK(writer.dependency_count() == 2);

    usz size = 0;
    u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
    REQUIRE(bytes != nullptr);
    AssetView view;
    REQUIRE(view.parse(bytes, size) == ASSET_PARSE_OK);
    CHECK(view.dependency_count() == 2);
    CHECK(view.dependencies[0] == BRICK_GUID);
    CHECK(view.dependencies[1] == NORMAL_GUID);
    CHECK(view.find_dependency(BRICK_GUID) == 0);
    CHECK(view.find_dependency(NORMAL_GUID) == 1);
    CHECK(view.find_dependency(WALL_GUID) == 2); // absent: == dependency_count()
    CHECK(view.find_dependency(AssetGuid{}) == 2);

    MEMORY::heap_allocator()->free(bytes);
    writer.free();
}

TEST_CASE("asset/asset_file: write refuses a null guid") {
    AssetWriter writer;
    writer.type = ASSET_TYPE::TEXTURE;
    CHECK(writer.guid.is_null());

    alignas(64) u8 out[64];
    std::memset(out, 0xab, sizeof(out));
    CHECK_FALSE(writer.write(out));
    CHECK(out[0] == 0xab); // nothing written

    usz size = 123;
    CHECK(writer.write(MEMORY::heap_allocator(), &size) == nullptr);
    CHECK(size == 123);

    writer.free();
}

TEST_CASE("asset/asset_file: parse rejects corrupted headers") {
    AssetWriter writer;
    fill_texture(writer);
    usz size = 0;
    u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
    REQUIRE(bytes != nullptr);
    const AssetHeader good = read_header(bytes);
    AssetView view;

    SUBCASE("wrong magic") {
        AssetHeader header = good;
        header.magic = ASSET_FILE::fourcc("RIFF");
        write_header(bytes, header);
        CHECK(view.parse(bytes, size) == ASSET_PARSE_BAD_MAGIC);
    }
    SUBCASE("unsupported version") {
        AssetHeader header = good;
        header.format_version = ASSET_FILE::FORMAT_VERSION + 1;
        write_header(bytes, header);
        CHECK(view.parse(bytes, size) == ASSET_PARSE_UNSUPPORTED_VERSION);
        header.format_version = 0;
        write_header(bytes, header);
        CHECK(view.parse(bytes, size) == ASSET_PARSE_UNSUPPORTED_VERSION);
    }
    SUBCASE("null guid") {
        AssetHeader header = good;
        header.guid = AssetGuid{};
        write_header(bytes, header);
        CHECK(view.parse(bytes, size) == ASSET_PARSE_BAD_HEADER);
    }
    SUBCASE("file_size smaller than the prelude") {
        AssetHeader header = good;
        header.file_size = sizeof(AssetHeader);
        write_header(bytes, header);
        CHECK(view.parse(bytes, size) == ASSET_PARSE_BAD_HEADER);
    }
    SUBCASE("buffer longer than the declared file") {
        AssetHeader header = good;
        header.file_size = size - 1;
        write_header(bytes, header);
        CHECK(view.parse(bytes, size) == ASSET_PARSE_BAD_HEADER);
    }
    SUBCASE("chunk count claims a table the buffer does not hold") {
        AssetHeader header = good;
        header.chunk_count = 1000;
        header.file_size = ASSET_FILE::payload_start(1, 1000) + 1;
        write_header(bytes, header);
        CHECK(view.parse(bytes, size) == ASSET_PARSE_TOO_SMALL);
    }
    SUBCASE("the untouched file still parses") {
        CHECK(view.parse(bytes, size) == ASSET_PARSE_OK);
    }

    // Every failure leaves the view empty.
    if (!view.is_parsed()) {
        CHECK(view.header == nullptr);
        CHECK(view.chunks == nullptr);
        CHECK(view.dependencies == nullptr);
        CHECK(view.data == nullptr);
        CHECK(view.size == 0);
    }

    MEMORY::heap_allocator()->free(bytes);
    writer.free();
}

TEST_CASE("asset/asset_file: parse rejects corrupted chunk tables") {
    AssetWriter writer;
    fill_texture(writer);
    usz size = 0;
    u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
    REQUIRE(bytes != nullptr);
    AssetView view;
    REQUIRE(view.parse(bytes, size) == ASSET_PARSE_OK);
    view.reset();

    SUBCASE("misaligned offset") {
        ChunkEntry chunk = read_chunk(bytes, 1);
        chunk.offset += 8;
        write_chunk(bytes, 1, chunk);
        CHECK(view.parse(bytes, size) == ASSET_PARSE_BAD_CHUNK);
    }
    SUBCASE("offset before the payload area") {
        ChunkEntry chunk = read_chunk(bytes, 0);
        chunk.offset = 64;
        write_chunk(bytes, 0, chunk);
        CHECK(view.parse(bytes, size) == ASSET_PARSE_BAD_CHUNK);
    }
    SUBCASE("payload past the end of the file") {
        ChunkEntry chunk = read_chunk(bytes, 4);
        chunk.size += 1;
        write_chunk(bytes, 4, chunk);
        CHECK(view.parse(bytes, size) == ASSET_PARSE_BAD_CHUNK);
    }
    SUBCASE("offset past the end of the file") {
        ChunkEntry chunk = read_chunk(bytes, 4);
        chunk.offset = ASSET_FILE::align_up(size, 64) + 64;
        chunk.size = 0;
        write_chunk(bytes, 4, chunk);
        CHECK(view.parse(bytes, size) == ASSET_PARSE_BAD_CHUNK);
    }
    SUBCASE("size that wraps around") {
        ChunkEntry chunk = read_chunk(bytes, 2);
        chunk.size = ~0ull;
        write_chunk(bytes, 2, chunk);
        CHECK(view.parse(bytes, size) == ASSET_PARSE_BAD_CHUNK);
    }
    SUBCASE("overlapping payloads") {
        ChunkEntry chunk = read_chunk(bytes, 3);
        chunk.offset = read_chunk(bytes, 2).offset; // same start as the previous chunk
        write_chunk(bytes, 3, chunk);
        CHECK(view.parse(bytes, size) == ASSET_PARSE_BAD_CHUNK);
    }
    SUBCASE("out-of-order table") {
        const ChunkEntry a = read_chunk(bytes, 3);
        const ChunkEntry b = read_chunk(bytes, 4);
        write_chunk(bytes, 3, b);
        write_chunk(bytes, 4, a);
        CHECK(view.parse(bytes, size) == ASSET_PARSE_BAD_CHUNK);
    }
    SUBCASE("unknown tags and flags are not errors") {
        ChunkEntry chunk = read_chunk(bytes, 1);
        chunk.tag = ASSET_FILE::fourcc("WHAT");
        chunk.flags = 0xffffffffu;
        chunk.version = 99;
        write_chunk(bytes, 1, chunk);
        CHECK(view.parse(bytes, size) == ASSET_PARSE_OK);
        CHECK(view.find_chunk(ASSET_FILE::fourcc("WHAT")) == &view.chunks[1]);
    }

    MEMORY::heap_allocator()->free(bytes);
    writer.free();
}

TEST_CASE("asset/asset_file: the prelude alone parses the same as the whole file") {
    AssetWriter writer;
    fill_texture(writer);
    usz size = 0;
    u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
    REQUIRE(bytes != nullptr);

    const usz prelude = ASSET_FILE::prelude_size(1, 5);
    REQUIRE(prelude < size);

    AssetView view;
    SUBCASE("exactly the prelude") {
        REQUIRE(view.parse(bytes, prelude) == ASSET_PARSE_OK);
    }
    SUBCASE("the prelude plus some payload bytes") {
        REQUIRE(view.parse(bytes, size - 1) == ASSET_PARSE_OK);
    }
    SUBCASE("the whole file") {
        REQUIRE(view.parse(bytes, size) == ASSET_PARSE_OK);
    }
    SUBCASE("one byte short of the prelude") {
        CHECK(view.parse(bytes, prelude - 1) == ASSET_PARSE_TOO_SMALL);
        CHECK_FALSE(view.is_parsed());
    }

    if (view.is_parsed()) {
        CHECK(view.header->type == ASSET_TYPE::TEXTURE);
        CHECK(view.header->guid == WALL_GUID);
        CHECK(view.dependency_count() == 1);
        CHECK(view.dependencies[0] == BRICK_GUID);
        CHECK(view.chunk_count() == 5);
        CHECK(view.has_editor_chunks());
        const ChunkEntry* pixels = view.find_chunk(CHUNK_TAG::PIXELS);
        REQUIRE(pixels != nullptr);
        CHECK(pixels->size == 300);
        CHECK(pixels->offset + pixels->size == view.header->file_size);
    }

    MEMORY::heap_allocator()->free(bytes);
    writer.free();
}

TEST_CASE("asset/asset_file: a cooked file carries the flag and no editor chunks") {
    AssetWriter editor;
    fill_texture(editor);
    usz size = 0;
    u8* bytes = editor.write(MEMORY::heap_allocator(), &size);
    REQUIRE(bytes != nullptr);
    AssetView source;
    REQUIRE(source.parse(bytes, size) == ASSET_PARSE_OK);

    // Cooking: copy everything but the EDITOR_ONLY chunks, set COOKED. Done
    // here from the in-memory file; asset_reader_tests does it from disk.
    AssetWriter cooked;
    cooked.type = source.header->type;
    cooked.flags = source.header->flags | ASSET_FLAG::COOKED;
    cooked.guid = source.header->guid;
    cooked.content_hash = source.header->content_hash;
    for (usz i = 0; i < source.dependency_count(); ++i) {
        cooked.add_dependency(source.dependencies[i]);
    }
    for (usz i = 0; i < source.chunk_count(); ++i) {
        const ChunkEntry& chunk = source.chunks[i];
        if (!chunk.is_editor_only()) {
            cooked.add_chunk(chunk.tag, chunk.version, chunk.flags, bytes + chunk.offset, chunk.size);
        }
    }

    usz cooked_size = 0;
    u8* cooked_bytes = cooked.write(MEMORY::heap_allocator(), &cooked_size);
    REQUIRE(cooked_bytes != nullptr);
    CHECK(cooked_size < size);

    AssetView view;
    REQUIRE(view.parse(cooked_bytes, cooked_size) == ASSET_PARSE_OK);
    CHECK(view.is_cooked());
    CHECK_FALSE(view.has_editor_chunks());
    CHECK(view.chunk_count() == 2);
    CHECK(view.header->guid == WALL_GUID);
    CHECK(view.header->content_hash == 0xdeadbeef);
    CHECK(view.dependency_count() == 1);
    CHECK(view.find_chunk(CHUNK_TAG::SOURCE) == nullptr);
    const ChunkEntry* pixels = view.find_chunk(CHUNK_TAG::PIXELS);
    REQUIRE(pixels != nullptr);
    CHECK(pixels->size == 300);
    CHECK(std::memcmp(cooked_bytes + pixels->offset, bytes + source.find_chunk(CHUNK_TAG::PIXELS)->offset, 300) == 0);

    MEMORY::heap_allocator()->free(cooked_bytes);
    MEMORY::heap_allocator()->free(bytes);
    cooked.free();
    editor.free();
}

TEST_CASE("asset/asset_file: a writer on an arena puts everything there") {
    ArenaAllocator arena(256 * MEMORY::KB);
    AssetWriter writer(&arena);
    fill_texture(writer);
    CHECK(arena.offset > 0);

    const usz before = arena.offset;
    usz size = 0;
    u8* bytes = writer.write(&arena, &size);
    REQUIRE(bytes != nullptr);
    CHECK(arena.offset >= before + size);
    CHECK(bytes >= reinterpret_cast<u8*>(arena.data));
    CHECK(bytes + size <= reinterpret_cast<u8*>(arena.data) + arena.arena_size);
    CHECK(is_aligned(bytes, ASSET_FILE::PAYLOAD_ALIGNMENT));

    AssetView view;
    CHECK(view.parse(bytes, size) == ASSET_PARSE_OK);

    writer.free();
    arena.release();
}

TEST_CASE("asset/asset_file: files round-trip through write_file and read_prelude") {
    const std::string path = temp_asset_path("roundtrip");
    AssetWriter writer;
    fill_texture(writer);
    usz size = 0;
    u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
    REQUIRE(bytes != nullptr);
    REQUIRE(ASSET_FILE::write_file(path.c_str(), bytes, size));

    SUBCASE("read_prelude returns just the tables") {
        usz read_size = 0;
        u8* read = ASSET_FILE::read_prelude(path.c_str(), MEMORY::heap_allocator(), &read_size);
        REQUIRE(read != nullptr);
        CHECK(read_size == ASSET_FILE::prelude_size(1, 5));
        CHECK(std::memcmp(read, bytes, read_size) == 0);
        CHECK(is_aligned(read, ASSET_FILE::PAYLOAD_ALIGNMENT));

        AssetView view;
        REQUIRE(view.parse(read, read_size) == ASSET_PARSE_OK);
        CHECK(view.header->guid == WALL_GUID);
        CHECK(view.chunk_count() == 5);
        CHECK(view.find_chunk(CHUNK_TAG::PIXELS) != nullptr);
        CHECK(view.find_chunk(CHUNK_TAG::PIXELS)->size == 300);
        MEMORY::heap_allocator()->free(read);
    }

    SUBCASE("read_prelude of a file with no payloads is the complete file") {
        AssetWriter empty;
        empty.type = ASSET_TYPE::MESH;
        empty.guid = NORMAL_GUID;
        usz empty_size = 0;
        u8* empty_bytes = empty.write(MEMORY::heap_allocator(), &empty_size);
        REQUIRE(empty_bytes != nullptr);
        REQUIRE(ASSET_FILE::write_file(path.c_str(), empty_bytes, empty_size));

        usz read_size = 0;
        u8* read = ASSET_FILE::read_prelude(path.c_str(), MEMORY::heap_allocator(), &read_size);
        REQUIRE(read != nullptr);
        CHECK(read_size == 64);
        AssetView view;
        CHECK(view.parse(read, read_size) == ASSET_PARSE_OK);
        CHECK(view.chunk_count() == 0);

        MEMORY::heap_allocator()->free(read);
        MEMORY::heap_allocator()->free(empty_bytes);
        empty.free();
    }

    SUBCASE("read_prelude rejects a file that is not an asset") {
        const u8 junk[100] = {'n', 'o', 't', ' ', 'a', 'n', ' ', 'a', 's', 's', 'e', 't'};
        REQUIRE(ASSET_FILE::write_file(path.c_str(), junk, sizeof(junk)));
        usz read_size = 7;
        CHECK(ASSET_FILE::read_prelude(path.c_str(), MEMORY::heap_allocator(), &read_size) == nullptr);
        CHECK(read_size == 7);
    }

    SUBCASE("a truncated file fails to read its prelude") {
        REQUIRE(ASSET_FILE::write_file(path.c_str(), bytes, 70)); // header plus part of the dependency table
        usz read_size = 7;
        CHECK(ASSET_FILE::read_prelude(path.c_str(), MEMORY::heap_allocator(), &read_size) == nullptr);
        CHECK(read_size == 7);
    }

    std::filesystem::remove(path);
    MEMORY::heap_allocator()->free(bytes);
    writer.free();
}

TEST_CASE("asset/asset_file: missing files are reported as nullptr / false") {
    const std::string path = temp_asset_path("does_not_exist/nested/missing");
    usz size = 7;
    CHECK(ASSET_FILE::read_prelude(path.c_str(), MEMORY::heap_allocator(), &size) == nullptr);
    CHECK(size == 7);
    const u8 byte = 0;
    CHECK_FALSE(ASSET_FILE::write_file(path.c_str(), &byte, 1));
}
