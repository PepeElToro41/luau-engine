#include "support/test_support.hpp"

#include "engine/asset/asset_reader.hpp"
#include "engine/asset/asset_view.hpp"
#include "engine/asset/asset_writer.hpp"
#include "engine/asset/source_chunk.hpp"
#include "engine/memory/heap_allocator.hpp"

#include <cstring>
#include <filesystem>
#include <string>

namespace {

const AssetGuid ROCK_GUID = {0x11, 0x22};
const char ROCK_NAME[] = "rock.obj";
const char ROCK_TEXT[] = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
constexpr usz ROCK_SIZE = sizeof(ROCK_TEXT) - 1;

std::string temp_asset_path(const char* stem) {
    return (std::filesystem::temp_directory_path() / (std::string("luau_engine_source_") + stem + ASSET_FILE::EXTENSION)).string();
}

// A mesh-shaped writer with the SOURCE chunk holding the rock text, or the
// placeholder when `keep` is false.
void fill_asset(AssetWriter& writer, const bool keep) {
    writer.type = ASSET_TYPE::MESH;
    writer.guid = ROCK_GUID;
    writer.add_chunk(CHUNK_TYPE::NAME, 0, CHUNK_FLAG::EDITOR_ONLY, nullptr, 0);
    writer.add_chunk(CHUNK_TYPE::IMPORT_SETTINGS, 0, CHUNK_FLAG::EDITOR_ONLY, nullptr, 0);
    if (keep) {
        REQUIRE(SOURCE_CHUNK::add_chunk(writer, ROCK_NAME, ROCK_TEXT, ROCK_SIZE) == 2);
    } else {
        REQUIRE(SOURCE_CHUNK::add_placeholder(writer) == 2);
    }
    const u8 mesh[64] = {};
    writer.add_chunk(CHUNK_TYPE::MESH, 1, 0, mesh, sizeof(mesh));
}

} // namespace

TEST_CASE("asset/source_chunk: layout helpers") {
    CHECK(SOURCE_CHUNK::data_offset(1) == 32);
    CHECK(SOURCE_CHUNK::data_offset(16) == 32);
    CHECK(SOURCE_CHUNK::data_offset(17) == 48);
    CHECK(SOURCE_CHUNK::data_offset(SOURCE_CHUNK::MAX_NAME_SIZE) == SOURCE_CHUNK::PREFIX_SIZE);
    CHECK(SOURCE_CHUNK::payload_size(8, 100) == 132);
    CHECK(SOURCE_CHUNK::PREFIX_SIZE % SOURCE_CHUNK::DATA_ALIGNMENT == 0);
}

TEST_CASE("asset/source_chunk: add_chunk writes name and data, parse reads them back") {
    AssetWriter writer;
    fill_asset(writer, true);

    const ChunkEntry& entry = writer.chunks[2];
    CHECK(entry.tag == CHUNK_TYPE::SOURCE);
    CHECK(entry.version == SOURCE_CHUNK::VERSION);
    CHECK(entry.editor_only());
    CHECK(entry.size == SOURCE_CHUNK::payload_size(sizeof(ROCK_NAME) - 1, ROCK_SIZE));
    CHECK(SOURCE_CHUNK::has_source(&entry));

    // The chunk after it still lands at an aligned offset past the blob.
    CHECK(writer.chunks[3].offset % ASSET_FILE::PAYLOAD_ALIGNMENT == 0);
    CHECK(writer.chunks[3].offset >= entry.offset + entry.size);

    usz size = 0;
    u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
    REQUIRE(bytes != nullptr);
    AssetView view = AssetView::parse(bytes, size);
    REQUIRE(view.is_ok());
    CHECK(SOURCE_CHUNK::has_source(view));
    const ChunkEntry* chunk = view.find_chunk(CHUNK_TYPE::SOURCE);
    REQUIRE(chunk != nullptr);

    SUBCASE("the whole payload") {
        SourceChunkView source = SourceChunkView::parse(*chunk, bytes + chunk->offset, chunk->size);
        REQUIRE(source.is_ok());
        CHECK(source.name_size == sizeof(ROCK_NAME) - 1);
        CHECK(std::memcmp(source.name, ROCK_NAME, source.name_size) == 0);
        CHECK(source.data_offset == SOURCE_CHUNK::data_offset(sizeof(ROCK_NAME) - 1));
        CHECK(source.data_size == ROCK_SIZE);
        REQUIRE(source.has_data());
        CHECK(std::memcmp(source.data, ROCK_TEXT, ROCK_SIZE) == 0);
        // The padding between the name and the data is zero.
        for (usz i = sizeof(SourceChunkHeader) + source.name_size; i < source.data_offset; ++i) {
            CHECK(bytes[chunk->offset + i] == 0);
        }
    }
    SUBCASE("only the prefix") {
        const usz prefix = chunk->size < SOURCE_CHUNK::PREFIX_SIZE ? chunk->size : SOURCE_CHUNK::PREFIX_SIZE;
        SourceChunkView source = SourceChunkView::parse(*chunk, bytes + chunk->offset, sizeof(SourceChunkHeader) + sizeof(ROCK_NAME) - 1);
        REQUIRE(source.is_ok());
        CHECK_FALSE(source.has_data());
        CHECK(source.data_size == ROCK_SIZE);
        CHECK(std::memcmp(bytes + chunk->offset + source.data_offset, ROCK_TEXT, ROCK_SIZE) == 0);
        (void)prefix;
    }
    SUBCASE("too short a prefix is TOO_SMALL") {
        CHECK(SourceChunkView::parse(*chunk, bytes + chunk->offset, sizeof(SourceChunkHeader) + 2).parse_error == SOURCE_PARSE_TOO_SMALL);
        CHECK(SourceChunkView::parse(*chunk, bytes + chunk->offset, 3).parse_error == SOURCE_PARSE_TOO_SMALL);
        CHECK(SourceChunkView::parse(*chunk, nullptr, 0).parse_error == SOURCE_PARSE_TOO_SMALL);
    }
    SUBCASE("an entry of another version is UNSUPPORTED_VERSION") {
        ChunkEntry other = *chunk;
        other.version = 2;
        CHECK(SourceChunkView::parse(other, bytes + chunk->offset, chunk->size).parse_error == SOURCE_PARSE_UNSUPPORTED_VERSION);
        CHECK_FALSE(SOURCE_CHUNK::has_source(&other));
    }
    SUBCASE("an entry whose size disagrees with the header is BAD_SIZE") {
        ChunkEntry other = *chunk;
        other.size += 1;
        CHECK(SourceChunkView::parse(other, bytes + chunk->offset, chunk->size).parse_error == SOURCE_PARSE_BAD_SIZE);
    }
    SUBCASE("a corrupt name size is BAD_NAME") {
        u8 copy[SOURCE_CHUNK::PREFIX_SIZE];
        std::memcpy(copy, bytes + chunk->offset, sizeof(copy));
        SourceChunkHeader header;
        std::memcpy(&header, copy, sizeof(header));
        header.name_size = 0;
        std::memcpy(copy, &header, sizeof(header));
        CHECK(SourceChunkView::parse(*chunk, copy, sizeof(copy)).parse_error == SOURCE_PARSE_BAD_NAME);
        header.name_size = SOURCE_CHUNK::MAX_NAME_SIZE + 1;
        std::memcpy(copy, &header, sizeof(header));
        CHECK(SourceChunkView::parse(*chunk, copy, sizeof(copy)).parse_error == SOURCE_PARSE_BAD_NAME);
    }

    MEMORY::heap_allocator()->free(bytes);
    writer.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("asset/source_chunk: the placeholder is no source") {
    AssetWriter writer;
    fill_asset(writer, false);
    const ChunkEntry& entry = writer.chunks[2];
    CHECK(entry.tag == CHUNK_TYPE::SOURCE);
    CHECK(entry.version == 0);
    CHECK(entry.size == 0);
    CHECK(entry.editor_only());
    CHECK_FALSE(SOURCE_CHUNK::has_source(&entry));
    CHECK_FALSE(SOURCE_CHUNK::has_source(nullptr));
    CHECK(SourceChunkView::parse(entry, nullptr, 0).parse_error == SOURCE_PARSE_NO_SOURCE);

    usz size = 0;
    u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
    REQUIRE(bytes != nullptr);
    AssetView view = AssetView::parse(bytes, size);
    REQUIRE(view.is_ok());
    CHECK_FALSE(SOURCE_CHUNK::has_source(view));

    MEMORY::heap_allocator()->free(bytes);
    writer.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("asset/source_chunk: add_chunk refuses bad names and null data") {
    AssetWriter writer;
    writer.guid = ROCK_GUID;
    CHECK(SOURCE_CHUNK::add_chunk(writer, "", ROCK_TEXT, ROCK_SIZE) == ~0u);
    CHECK(SOURCE_CHUNK::add_chunk(writer, nullptr, ROCK_TEXT, ROCK_SIZE) == ~0u);
    CHECK(SOURCE_CHUNK::add_chunk(writer, ROCK_NAME, nullptr, ROCK_SIZE) == ~0u);
    std::string long_name(SOURCE_CHUNK::MAX_NAME_SIZE + 1, 'a');
    CHECK(SOURCE_CHUNK::add_chunk(writer, long_name.c_str(), ROCK_TEXT, ROCK_SIZE) == ~0u);
    CHECK(writer.chunk_count() == 0);

    // The longest name and an empty file are both fine.
    std::string max_name(SOURCE_CHUNK::MAX_NAME_SIZE, 'b');
    CHECK(SOURCE_CHUNK::add_chunk(writer, max_name.c_str(), nullptr, 0) == 0);
    CHECK(writer.chunks[0].size == SOURCE_CHUNK::PREFIX_SIZE);
    CHECK(SOURCE_CHUNK::has_source(&writer.chunks[0]));
    SourceChunkView source = SourceChunkView::parse(writer.chunks[0], writer.body.data + writer.chunks[0].offset, writer.chunks[0].size);
    REQUIRE(source.is_ok());
    CHECK(source.name_size == SOURCE_CHUNK::MAX_NAME_SIZE);
    CHECK(source.data_size == 0);
    CHECK(source.has_data());

    writer.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("asset/source_chunk: a reader reads the prefix with read_bytes and the blob after it") {
    const std::string path = temp_asset_path("read_bytes");
    AssetWriter writer;
    fill_asset(writer, true);
    usz size = 0;
    u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
    REQUIRE(bytes != nullptr);
    REQUIRE(ASSET_FILE::write_file(path.c_str(), bytes, size));
    MEMORY::heap_allocator()->free(bytes);
    writer.free();

    AssetView view = ASSET_FILE::read_prelude(path.c_str(), MEMORY::heap_allocator());
    REQUIRE(view.is_ok());
    const ChunkEntry* chunk = view.find_chunk(CHUNK_TYPE::SOURCE);
    REQUIRE(chunk != nullptr);
    REQUIRE(SOURCE_CHUNK::has_source(chunk));

    AssetReader reader;
    REQUIRE(reader.open(path.c_str()));
    REQUIRE(reader.matches(view));

    u8 prefix[SOURCE_CHUNK::PREFIX_SIZE];
    const usz prefix_size = chunk->size < sizeof(prefix) ? chunk->size : sizeof(prefix);
    REQUIRE(reader.read_bytes(*chunk, 0, prefix, prefix_size));
    SourceChunkView source = SourceChunkView::parse(*chunk, prefix, prefix_size);
    REQUIRE(source.is_ok());
    CHECK(std::string(source.name, source.name_size) == ROCK_NAME);
    CHECK(source.data_size == ROCK_SIZE);

    u8 data[ROCK_SIZE];
    REQUIRE(reader.read_bytes(*chunk, source.data_offset, data, source.data_size));
    CHECK(std::memcmp(data, ROCK_TEXT, ROCK_SIZE) == 0);

    // Ranges past the chunk are refused; an empty range is not a read.
    CHECK_FALSE(reader.read_bytes(*chunk, source.data_offset, data, source.data_size + 1));
    CHECK_FALSE(reader.read_bytes(*chunk, chunk->size + 1, data, 0));
    CHECK(reader.read_bytes(*chunk, chunk->size, data, 0));
    reader.close();
    CHECK_FALSE(reader.read_bytes(*chunk, 0, prefix, prefix_size));

    ASSET_FILE::free_prelude(&view, MEMORY::heap_allocator());
    std::filesystem::remove(path);
    CHECK_ARENA_CLEAN();
}
