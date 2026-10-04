#include "support/test_support.hpp"

#include "engine/asset/asset_file.hpp"
#include "engine/asset/asset_reader.hpp"
#include "engine/memory/arena_allocator.hpp"
#include "engine/memory/heap_allocator.hpp"

#include <cstring>
#include <filesystem>
#include <string>

namespace {

const AssetGuid ROCK_GUID = {0x10, 0x20};
const AssetGuid MATERIAL_GUID = {0x30, 0x40};

bool is_aligned(const void* pointer, const usz alignment) {
    return (reinterpret_cast<uintptr_t>(pointer) & (alignment - 1)) == 0;
}

std::string temp_asset_path(const char* stem) {
    return (std::filesystem::temp_directory_path() / (std::string("luau_engine_reader_") + stem + ASSET_FILE::EXTENSION)).string();
}

// A mesh-shaped file: editor chunks, a MESH placeholder, two VERT streams
// with distinct patterns, an empty INDX and a 48-byte BBOX.
void fill_mesh(AssetWriter& writer) {
    writer.type = ASSET_TYPE::MESH;
    writer.guid = ROCK_GUID;
    writer.content_hash = 0x1234;
    writer.add_dependency(MATERIAL_GUID);

    const char name[] = "rock";
    u8 positions[120];
    u8 normals[200];
    for (usz i = 0; i < sizeof(positions); ++i) {
        positions[i] = static_cast<u8>(i);
    }
    for (usz i = 0; i < sizeof(normals); ++i) {
        normals[i] = static_cast<u8>(200 - i);
    }
    f32 bounds[12] = {-1, -2, -3, 1, 2, 3, 0, 0, 0, 3.75f, 0, 0};

    writer.add_chunk(CHUNK_TAG::NAME, 1, CHUNK_FLAG::EDITOR_ONLY, name, sizeof(name));
    writer.add_chunk(CHUNK_TAG::MESH, 1, 0, nullptr, 0);
    writer.add_chunk(CHUNK_TAG::VERTICES, 1, 0, positions, sizeof(positions));
    writer.add_chunk(CHUNK_TAG::VERTICES, 1, 0, normals, sizeof(normals));
    writer.add_chunk(CHUNK_TAG::INDICES, 1, 0, nullptr, 0);
    writer.add_chunk(CHUNK_TAG::BOUNDS, 1, 0, bounds, sizeof(bounds));
}

// Writes the mesh file to `path` and returns its bytes for comparison.
u8* write_mesh_file(const std::string& path, usz* out_size) {
    AssetWriter writer;
    fill_mesh(writer);
    u8* bytes = writer.write(MEMORY::heap_allocator(), out_size);
    writer.free();
    REQUIRE(bytes != nullptr);
    REQUIRE(ASSET_FILE::write_file(path.c_str(), bytes, *out_size));
    return bytes;
}

} // namespace

TEST_CASE("asset/asset_reader: a reader starts closed and refuses to read") {
    AssetReader reader;
    CHECK_FALSE(reader.is_open());
    AssetView empty;
    CHECK_FALSE(reader.matches(empty));

    ChunkEntry chunk;
    chunk.offset = 64;
    chunk.size = 0;
    CHECK_FALSE(reader.read_chunk(chunk, static_cast<void*>(nullptr)));
    CHECK(reader.read_chunk(chunk, MEMORY::heap_allocator()) == nullptr);
    CHECK(reader.read_chunk(empty, CHUNK_TAG::MESH, MEMORY::heap_allocator()) == nullptr);
    usz size = 7;
    CHECK(reader.read_prelude(MEMORY::heap_allocator(), &size) == nullptr);
    CHECK(size == 7);

    reader.close(); // closing a closed reader is fine
    CHECK_FALSE(reader.is_open());
}

TEST_CASE("asset/asset_reader: open validates the header against the file on disk") {
    const std::string path = temp_asset_path("open");
    usz size = 0;
    u8* bytes = write_mesh_file(path, &size);

    SUBCASE("a good file opens and exposes its header") {
        AssetReader reader;
        REQUIRE(reader.open(path.c_str()));
        CHECK(reader.is_open());
        CHECK(reader.header.guid == ROCK_GUID);
        CHECK(reader.header.type == ASSET_TYPE::MESH);
        CHECK(reader.header.file_size == size);
        CHECK(reader.header.chunk_count == 6);
        reader.close();
        CHECK_FALSE(reader.is_open());
        CHECK(reader.header.guid.is_null());
    }

    SUBCASE("open on an open reader closes the previous file first") {
        AssetReader reader;
        REQUIRE(reader.open(path.c_str()));
        REQUIRE(reader.open(path.c_str()));
        CHECK(reader.is_open());
        reader.close();
    }

    SUBCASE("a missing file") {
        AssetReader reader;
        CHECK_FALSE(reader.open(temp_asset_path("does_not_exist/nested/missing").c_str()));
        CHECK_FALSE(reader.is_open());
    }

    SUBCASE("not an asset") {
        const u8 junk[100] = {'n', 'o', 'p', 'e'};
        REQUIRE(ASSET_FILE::write_file(path.c_str(), junk, sizeof(junk)));
        AssetReader reader;
        CHECK_FALSE(reader.open(path.c_str()));
        CHECK_FALSE(reader.is_open());
    }

    SUBCASE("shorter than a header") {
        REQUIRE(ASSET_FILE::write_file(path.c_str(), bytes, 30));
        AssetReader reader;
        CHECK_FALSE(reader.open(path.c_str()));
    }

    SUBCASE("truncated after the header: the size on disk disagrees") {
        REQUIRE(ASSET_FILE::write_file(path.c_str(), bytes, size - 1));
        AssetReader reader;
        CHECK_FALSE(reader.open(path.c_str()));
        CHECK_FALSE(reader.is_open());
    }

    SUBCASE("trailing garbage: the size on disk disagrees") {
        u8* longer = static_cast<u8*>(MEMORY::heap_allocator()->allocate(size + 1, 8));
        std::memcpy(longer, bytes, size);
        longer[size] = 0;
        REQUIRE(ASSET_FILE::write_file(path.c_str(), longer, size + 1));
        AssetReader reader;
        CHECK_FALSE(reader.open(path.c_str()));
        MEMORY::heap_allocator()->free(longer);
    }

    std::filesystem::remove(path);
    MEMORY::heap_allocator()->free(bytes);
}

TEST_CASE("asset/asset_reader: read_prelude from the reader parses into a matching view") {
    const std::string path = temp_asset_path("prelude");
    usz size = 0;
    u8* bytes = write_mesh_file(path, &size);

    AssetReader reader;
    REQUIRE(reader.open(path.c_str()));

    usz prelude_size = 0;
    u8* prelude = reader.read_prelude(MEMORY::heap_allocator(), &prelude_size);
    REQUIRE(prelude != nullptr);
    CHECK(prelude_size == ASSET_FILE::prelude_size(1, 6));
    CHECK(std::memcmp(prelude, bytes, prelude_size) == 0);
    CHECK(is_aligned(prelude, ASSET_FILE::PAYLOAD_ALIGNMENT));

    AssetView view;
    REQUIRE(view.parse(prelude, prelude_size) == ASSET_PARSE_OK);
    CHECK(reader.matches(view));
    CHECK(view.dependencies[0] == MATERIAL_GUID);

    // The same bytes ASSET_FILE::read_prelude gives.
    usz other_size = 0;
    u8* other = ASSET_FILE::read_prelude(path.c_str(), MEMORY::heap_allocator(), &other_size);
    REQUIRE(other != nullptr);
    CHECK(other_size == prelude_size);
    CHECK(std::memcmp(other, prelude, prelude_size) == 0);

    // Reading the prelude again after a chunk read still starts at 0.
    const ChunkEntry* bounds = view.find_chunk(CHUNK_TAG::BOUNDS);
    REQUIRE(bounds != nullptr);
    u8 scratch[48];
    REQUIRE(reader.read_chunk(*bounds, scratch));
    u8* again = reader.read_prelude(MEMORY::heap_allocator(), nullptr);
    REQUIRE(again != nullptr);
    CHECK(std::memcmp(again, prelude, prelude_size) == 0);

    MEMORY::heap_allocator()->free(again);
    MEMORY::heap_allocator()->free(other);
    MEMORY::heap_allocator()->free(prelude);
    reader.close();
    std::filesystem::remove(path);
    MEMORY::heap_allocator()->free(bytes);
}

TEST_CASE("asset/asset_reader: read_chunk seeks to a chunk by its table entry") {
    const std::string path = temp_asset_path("chunks");
    usz size = 0;
    u8* bytes = write_mesh_file(path, &size);

    usz prelude_size = 0;
    u8* prelude = ASSET_FILE::read_prelude(path.c_str(), MEMORY::heap_allocator(), &prelude_size);
    REQUIRE(prelude != nullptr);
    AssetView view;
    REQUIRE(view.parse(prelude, prelude_size) == ASSET_PARSE_OK);

    AssetReader reader;
    REQUIRE(reader.open(path.c_str()));
    REQUIRE(reader.matches(view));

    SUBCASE("into caller memory, in any order") {
        const ChunkEntry* bounds = view.find_chunk(CHUNK_TAG::BOUNDS);
        REQUIRE(bounds != nullptr);
        f32 box[12];
        REQUIRE(reader.read_chunk(*bounds, box));
        CHECK(box[0] == -1.0f);
        CHECK(box[5] == 3.0f);
        CHECK(box[9] == 3.75f);

        // Then something earlier in the file: the reader seeks backwards.
        const ChunkEntry* name = view.find_chunk(CHUNK_TAG::NAME);
        REQUIRE(name != nullptr);
        char text[8];
        REQUIRE(reader.read_chunk(*name, text));
        CHECK(doctest::String(text) == "rock");
    }

    SUBCASE("into a fresh aligned buffer, walking repeated tags") {
        const ChunkEntry* first = view.find_chunk(CHUNK_TAG::VERTICES);
        const ChunkEntry* second = view.find_chunk(CHUNK_TAG::VERTICES, first);
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);

        u8* normals = reader.read_chunk(*second, MEMORY::heap_allocator());
        REQUIRE(normals != nullptr);
        CHECK(is_aligned(normals, ASSET_FILE::PAYLOAD_ALIGNMENT));
        bool intact = true;
        for (usz i = 0; i < second->size; ++i) {
            intact = intact && normals[i] == static_cast<u8>(200 - i);
        }
        CHECK(intact);

        u8* positions = reader.read_chunk(*first, MEMORY::heap_allocator());
        REQUIRE(positions != nullptr);
        CHECK(positions[119] == 119);
        CHECK(std::memcmp(positions, bytes + first->offset, first->size) == 0);

        MEMORY::heap_allocator()->free(positions);
        MEMORY::heap_allocator()->free(normals);
    }

    SUBCASE("by tag through the view") {
        const ChunkEntry* entry = nullptr;
        u8* positions = reader.read_chunk(view, CHUNK_TAG::VERTICES, MEMORY::heap_allocator(), &entry);
        REQUIRE(positions != nullptr);
        REQUIRE(entry == view.find_chunk(CHUNK_TAG::VERTICES)); // the first one
        CHECK(entry->size == 120);
        CHECK(positions[7] == 7);
        MEMORY::heap_allocator()->free(positions);

        // The out entry is optional.
        u8* again = reader.read_chunk(view, CHUNK_TAG::BOUNDS, MEMORY::heap_allocator());
        REQUIRE(again != nullptr);
        MEMORY::heap_allocator()->free(again);
    }

    SUBCASE("an absent tag reads nothing and reports no entry") {
        const ChunkEntry* entry = &view.chunks[0];
        CHECK(reader.read_chunk(view, CHUNK_TAG::PIXELS, MEMORY::heap_allocator(), &entry) == nullptr);
        CHECK(entry == nullptr);
    }

    SUBCASE("an empty chunk succeeds into memory and allocates nothing") {
        const ChunkEntry* indices = view.find_chunk(CHUNK_TAG::INDICES);
        REQUIRE(indices != nullptr);
        CHECK(indices->size == 0);
        u8 untouched = 0xab;
        CHECK(reader.read_chunk(*indices, &untouched));
        CHECK(untouched == 0xab);
        CHECK(reader.read_chunk(*indices, MEMORY::heap_allocator()) == nullptr);
        const ChunkEntry* entry = nullptr;
        CHECK(reader.read_chunk(view, CHUNK_TAG::INDICES, MEMORY::heap_allocator(), &entry) == nullptr);
        CHECK(entry == indices); // found, just empty
    }

    SUBCASE("payloads land on an arena") {
        ArenaAllocator arena(64 * MEMORY::KB);
        const ChunkEntry* entry = nullptr;
        u8* normals = reader.read_chunk(view, CHUNK_TAG::VERTICES, &arena, &entry);
        REQUIRE(normals != nullptr);
        CHECK(normals >= reinterpret_cast<u8*>(arena.data));
        CHECK(normals + entry->size <= reinterpret_cast<u8*>(arena.data) + arena.arena_size);
        arena.release();
    }

    SUBCASE("an entry outside the file is refused before any read") {
        ChunkEntry bogus = view.chunks[2];
        u8 scratch[256];

        bogus.offset = view.header->file_size + 64;
        bogus.size = 0;
        CHECK_FALSE(reader.read_chunk(bogus, scratch));

        bogus = view.chunks[2];
        bogus.size = view.header->file_size; // runs past the end
        CHECK_FALSE(reader.read_chunk(bogus, scratch));
        CHECK(reader.read_chunk(bogus, MEMORY::heap_allocator()) == nullptr);

        bogus = view.chunks[2];
        bogus.offset += 8; // misaligned: not an entry this file could have
        CHECK_FALSE(reader.read_chunk(bogus, scratch));

        bogus = view.chunks[2];
        bogus.size = ~0ull; // wraps
        CHECK_FALSE(reader.read_chunk(bogus, scratch));
    }

    reader.close();
    MEMORY::heap_allocator()->free(prelude);
    std::filesystem::remove(path);
    MEMORY::heap_allocator()->free(bytes);
}

TEST_CASE("asset/asset_reader: a stale view is rejected until its prelude is re-read") {
    const std::string path = temp_asset_path("stale");
    usz size = 0;
    u8* bytes = write_mesh_file(path, &size);

    usz prelude_size = 0;
    u8* prelude = ASSET_FILE::read_prelude(path.c_str(), MEMORY::heap_allocator(), &prelude_size);
    REQUIRE(prelude != nullptr);
    AssetView view;
    REQUIRE(view.parse(prelude, prelude_size) == ASSET_PARSE_OK);

    // The file is re-imported: same guid, new content, new size.
    AssetWriter reimported;
    fill_mesh(reimported);
    reimported.content_hash = 0x5678;
    const u8 extra[16] = {};
    reimported.add_chunk(CHUNK_TAG::SOURCE, 1, CHUNK_FLAG::EDITOR_ONLY, extra, sizeof(extra));
    usz new_size = 0;
    u8* new_bytes = reimported.write(MEMORY::heap_allocator(), &new_size);
    reimported.free();
    REQUIRE(new_bytes != nullptr);
    REQUIRE(ASSET_FILE::write_file(path.c_str(), new_bytes, new_size));

    AssetReader reader;
    REQUIRE(reader.open(path.c_str()));
    CHECK(reader.header.guid == view.header->guid);
    CHECK_FALSE(reader.matches(view));
    CHECK(reader.read_chunk(view, CHUNK_TAG::BOUNDS, MEMORY::heap_allocator()) == nullptr);

    SUBCASE("a view of a different asset never matches") {
        AssetWriter other;
        other.type = ASSET_TYPE::TEXTURE;
        other.guid = MATERIAL_GUID;
        usz other_size = 0;
        u8* other_bytes = other.write(MEMORY::heap_allocator(), &other_size);
        other.free();
        AssetView other_view;
        REQUIRE(other_view.parse(other_bytes, other_size) == ASSET_PARSE_OK);
        CHECK_FALSE(reader.matches(other_view));
        MEMORY::heap_allocator()->free(other_bytes);
    }

    SUBCASE("refreshing the view from the open reader") {
        usz fresh_size = 0;
        u8* fresh = reader.read_prelude(MEMORY::heap_allocator(), &fresh_size);
        REQUIRE(fresh != nullptr);
        AssetView fresh_view;
        REQUIRE(fresh_view.parse(fresh, fresh_size) == ASSET_PARSE_OK);
        CHECK(reader.matches(fresh_view));
        CHECK(fresh_view.chunk_count() == 7);
        CHECK(fresh_view.header->content_hash == 0x5678);

        const ChunkEntry* entry = nullptr;
        u8* bounds = reader.read_chunk(fresh_view, CHUNK_TAG::BOUNDS, MEMORY::heap_allocator(), &entry);
        REQUIRE(bounds != nullptr);
        CHECK(entry->size == 48);
        MEMORY::heap_allocator()->free(bounds);
        MEMORY::heap_allocator()->free(fresh);
    }

    reader.close();
    MEMORY::heap_allocator()->free(new_bytes);
    MEMORY::heap_allocator()->free(prelude);
    std::filesystem::remove(path);
    MEMORY::heap_allocator()->free(bytes);
}

TEST_CASE("asset/asset_reader: cooking copies chunks from disk one at a time") {
    const std::string path = temp_asset_path("cook_source");
    usz size = 0;
    u8* bytes = write_mesh_file(path, &size);

    usz prelude_size = 0;
    u8* prelude = ASSET_FILE::read_prelude(path.c_str(), MEMORY::heap_allocator(), &prelude_size);
    REQUIRE(prelude != nullptr);
    AssetView source;
    REQUIRE(source.parse(prelude, prelude_size) == ASSET_PARSE_OK);

    AssetReader reader;
    REQUIRE(reader.open(path.c_str()));
    REQUIRE(reader.matches(source));

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
        if (chunk.is_editor_only()) {
            continue;
        }
        u8* payload = reader.read_chunk(chunk, MEMORY::heap_allocator());
        REQUIRE((payload != nullptr) == (chunk.size > 0));
        cooked.add_chunk(chunk.tag, chunk.version, chunk.flags, payload, chunk.size);
        MEMORY::heap_allocator()->free(payload);
    }
    reader.close();

    usz cooked_size = 0;
    u8* cooked_bytes = cooked.write(MEMORY::heap_allocator(), &cooked_size);
    cooked.free();
    REQUIRE(cooked_bytes != nullptr);
    CHECK(cooked_size < size);

    AssetView view;
    REQUIRE(view.parse(cooked_bytes, cooked_size) == ASSET_PARSE_OK);
    CHECK(view.is_cooked());
    CHECK_FALSE(view.has_editor_chunks());
    CHECK(view.chunk_count() == 5);
    const ChunkEntry* second = view.find_chunk(CHUNK_TAG::VERTICES, view.find_chunk(CHUNK_TAG::VERTICES));
    REQUIRE(second != nullptr);
    CHECK(second->size == 200);
    const ChunkEntry* original = source.find_chunk(CHUNK_TAG::VERTICES, source.find_chunk(CHUNK_TAG::VERTICES));
    CHECK(std::memcmp(cooked_bytes + second->offset, bytes + original->offset, 200) == 0);

    MEMORY::heap_allocator()->free(cooked_bytes);
    MEMORY::heap_allocator()->free(prelude);
    std::filesystem::remove(path);
    MEMORY::heap_allocator()->free(bytes);
}
