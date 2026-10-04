#include "support/test_support.hpp"

#include "engine/asset/asset_file.hpp"
#include "engine/asset/asset_resource_provider.hpp"
#include "engine/memory/heap_allocator.hpp"

#include <cstring>
#include <filesystem>
#include <string>

namespace {

const AssetGuid ROCK_GUID = {0x10, 0x20};
const AssetGuid MATERIAL_GUID = {0x30, 0x40};
const AssetGuid UNKNOWN_GUID = {0x50, 0x60};

bool is_aligned(const void* pointer, const usz alignment) {
    return (reinterpret_cast<uintptr_t>(pointer) & (alignment - 1)) == 0;
}

std::string temp_asset_path(const char* stem) {
    return (std::filesystem::temp_directory_path() / (std::string("luau_engine_provider_") + stem + ASSET_FILE::EXTENSION)).string();
}

// A mesh-shaped file: an editor-only NAME, an empty MESH placeholder, two
// VERT streams with distinct patterns, an empty INDX and a 48-byte BBOX.
// Chunk indices: 0 NAME, 1 MESH, 2 VERT, 3 VERT, 4 INDX, 5 BBOX.
void fill_mesh(AssetWriter& writer, const u64 content_hash = 0x1234) {
    writer.type = ASSET_TYPE::MESH;
    writer.guid = ROCK_GUID;
    writer.content_hash = content_hash;
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

constexpr u64 RUNTIME_BYTES = 120 + 200 + 48;

// Writes the mesh file to `path` and returns its bytes for comparison.
u8* write_mesh_file(const std::string& path, usz* out_size, const u64 content_hash = 0x1234) {
    AssetWriter writer;
    fill_mesh(writer, content_hash);
    u8* bytes = writer.write(MEMORY::heap_allocator(), out_size);
    writer.free();
    REQUIRE(bytes != nullptr);
    REQUIRE(ASSET_FILE::write_file(path.c_str(), bytes, *out_size));
    return bytes;
}

// Reads the prelude of `path` and parses it into `view`; returns the buffer.
u8* scan(const std::string& path, AssetView& view) {
    usz size = 0;
    u8* prelude = ASSET_FILE::read_prelude(path.c_str(), MEMORY::heap_allocator(), &size);
    REQUIRE(prelude != nullptr);
    REQUIRE(view.parse(prelude, size) == ASSET_PARSE_OK);
    return prelude;
}

} // namespace

TEST_CASE("asset/asset_resource_provider: add copies the prelude and reads nothing") {
    const std::string path = temp_asset_path("add");
    usz size = 0;
    u8* bytes = write_mesh_file(path, &size);
    AssetView view;
    u8* prelude = scan(path, view);

    AssetResourceProvider provider;
    CHECK(provider.count() == 0);
    CHECK(provider.find(ROCK_GUID) == nullptr);

    AssetResource* rock = provider.add(view, path.c_str());
    REQUIRE(rock != nullptr);
    CHECK(provider.count() == 1);
    CHECK(provider.contains(ROCK_GUID));
    CHECK(provider.find(ROCK_GUID) == rock);
    CHECK(rock->guid == ROCK_GUID);
    CHECK(doctest::String(rock->path) == path.c_str());
    CHECK(rock->path != path.c_str());

    // Its own copy of the prelude, aligned like a payload buffer.
    CHECK(rock->prelude != prelude);
    CHECK(rock->prelude_size == view.size);
    CHECK(is_aligned(rock->prelude, ASSET_FILE::PAYLOAD_ALIGNMENT));
    CHECK(rock->view.is_parsed());
    CHECK(rock->view.data == rock->prelude);
    CHECK(rock->view.chunk_count() == 6);
    CHECK(rock->view.dependencies[0] == MATERIAL_GUID);

    // The caller's buffer can go: the resource does not point into it.
    MEMORY::heap_allocator()->free(prelude);
    std::memset(&view, 0, sizeof(view));
    CHECK(rock->view.header->guid == ROCK_GUID);

    // Nothing is resident yet.
    CHECK_FALSE(rock->is_loaded());
    CHECK(rock->resident_bytes == 0);
    CHECK(provider.resident_bytes() == 0);
    for (usz i = 0; i < 6; ++i) {
        CHECK(rock->payload(i) == nullptr);
    }
    CHECK(rock->is_resident(1)); // MESH is empty: nothing to read
    CHECK(rock->is_resident(4)); // INDX too
    CHECK_FALSE(rock->is_resident(2));
    CHECK_FALSE(rock->is_resident(99));
    CHECK(rock->payload(99) == nullptr);
    const ChunkEntry* entry = nullptr;
    CHECK(rock->find_payload(CHUNK_TAG::BOUNDS, &entry) == nullptr);
    CHECK(entry == rock->view.find_chunk(CHUNK_TAG::BOUNDS)); // the entry is known even unloaded
    CHECK(rock->find_payload(CHUNK_TAG::PIXELS, &entry) == nullptr);
    CHECK(entry == nullptr);

    SUBCASE("an unparsed view is refused") {
        AssetView empty;
        CHECK(provider.add(empty, path.c_str()) == nullptr);
        CHECK(provider.count() == 1);
    }

    SUBCASE("adding the same asset again returns the same resource") {
        AssetView again;
        u8* again_prelude = scan(path, again);
        CHECK(provider.add(again, path.c_str()) == rock);
        CHECK(provider.count() == 1);

        // A new path for the same file (moved in the OS) is taken over.
        const std::string moved = temp_asset_path("add_moved");
        CHECK(provider.add(again, moved.c_str()) == rock);
        CHECK(doctest::String(rock->path) == moved.c_str());
        MEMORY::heap_allocator()->free(again_prelude);
    }

    SUBCASE("remove forgets the asset") {
        CHECK(provider.remove(ROCK_GUID));
        CHECK(provider.count() == 0);
        CHECK(provider.find(ROCK_GUID) == nullptr);
        CHECK_FALSE(provider.remove(ROCK_GUID));
    }

    provider.free();
    CHECK(provider.count() == 0);
    std::filesystem::remove(path);
    MEMORY::heap_allocator()->free(bytes);
}

TEST_CASE("asset/asset_resource_provider: get loads every runtime chunk once") {
    const std::string path = temp_asset_path("get");
    usz size = 0;
    u8* bytes = write_mesh_file(path, &size);
    AssetView view;
    u8* prelude = scan(path, view);

    AssetResourceProvider provider;
    AssetResource* added = provider.add(view, path.c_str());
    REQUIRE(added != nullptr);

    SUBCASE("by guid") {
        AssetResource* rock = provider.get(ROCK_GUID);
        REQUIRE(rock == added);
        CHECK(rock->is_loaded());
        CHECK(rock->resident_bytes == RUNTIME_BYTES);
        CHECK(provider.resident_bytes() == RUNTIME_BYTES);

        // Payloads match the file, in aligned buffers the provider owns.
        const ChunkEntry* entry = nullptr;
        const u8* positions = rock->find_payload(CHUNK_TAG::VERTICES, &entry);
        REQUIRE(positions != nullptr);
        REQUIRE(entry != nullptr);
        CHECK(entry->size == 120);
        CHECK(is_aligned(positions, ASSET_FILE::PAYLOAD_ALIGNMENT));
        CHECK(std::memcmp(positions, bytes + entry->offset, entry->size) == 0);
        CHECK(positions == rock->payload(2));

        const u8* normals = rock->payload(3);
        REQUIRE(normals != nullptr);
        CHECK(normals[0] == 200);
        CHECK(normals[199] == 1);

        const f32* box = reinterpret_cast<const f32*>(rock->find_payload(CHUNK_TAG::BOUNDS));
        REQUIRE(box != nullptr);
        CHECK(box[0] == -1.0f);
        CHECK(box[9] == 3.75f);

        // Empty chunks are resident with no bytes; the editor-only NAME was skipped.
        CHECK(rock->payload(1) == nullptr);
        CHECK(rock->is_resident(1));
        CHECK(rock->find_payload(CHUNK_TAG::INDICES, &entry) == nullptr);
        CHECK(entry->size == 0);
        CHECK(rock->payload(0) == nullptr);
        CHECK_FALSE(rock->is_resident(0));

        // Loaded means loaded: the file is not needed again.
        std::filesystem::remove(path);
        CHECK(provider.get(ROCK_GUID) == rock);
        CHECK(rock->find_payload(CHUNK_TAG::VERTICES) == positions);
        CHECK(provider.load(rock));
        CHECK(provider.resident_bytes() == RUNTIME_BYTES);
    }

    SUBCASE("by view") {
        AssetResource* rock = provider.get(view);
        REQUIRE(rock == added);
        CHECK(rock->is_loaded());

        AssetView empty;
        CHECK(provider.get(empty) == nullptr);
    }

    SUBCASE("a guid that was never added is not loaded") {
        CHECK(provider.get(UNKNOWN_GUID) == nullptr);
        CHECK(provider.count() == 1);
        CHECK(provider.resident_bytes() == 0);
    }

    SUBCASE("a file that cannot be opened leaves the resource registered and unloaded") {
        std::filesystem::remove(path);
        CHECK(provider.get(ROCK_GUID) == nullptr);
        CHECK(provider.find(ROCK_GUID) == added);
        CHECK_FALSE(added->is_loaded());
        CHECK(provider.resident_bytes() == 0);

        // Put the file back and it loads.
        REQUIRE(ASSET_FILE::write_file(path.c_str(), bytes, size));
        CHECK(provider.get(ROCK_GUID) == added);
        CHECK(added->is_loaded());
    }

    provider.free();
    MEMORY::heap_allocator()->free(prelude);
    std::filesystem::remove(path);
    MEMORY::heap_allocator()->free(bytes);
}

TEST_CASE("asset/asset_resource_provider: get_chunk reads one chunk on demand") {
    const std::string path = temp_asset_path("chunk");
    usz size = 0;
    u8* bytes = write_mesh_file(path, &size);
    AssetView view;
    u8* prelude = scan(path, view);

    AssetResourceProvider provider;
    AssetResource* rock = provider.add(view, path.c_str());
    REQUIRE(rock != nullptr);

    SUBCASE("by tag, including editor-only chunks") {
        const ChunkEntry* entry = nullptr;
        const u8* name = provider.get_chunk(rock, CHUNK_TAG::NAME, &entry);
        REQUIRE(name != nullptr);
        REQUIRE(entry != nullptr);
        CHECK(entry->tag == CHUNK_TAG::NAME);
        CHECK(doctest::String(reinterpret_cast<const char*>(name)) == "rock");
        CHECK(rock->is_resident(0));
        CHECK_FALSE(rock->is_loaded()); // only NAME is in
        CHECK(rock->resident_bytes == 5);
        CHECK(provider.resident_bytes() == 5);

        // Asking again hands out the same buffer without a read.
        std::filesystem::remove(path);
        CHECK(provider.get_chunk(rock, CHUNK_TAG::NAME) == name);
        CHECK(rock->find_payload(CHUNK_TAG::NAME) == name);
        // A chunk that is not resident needs the file.
        CHECK(provider.get_chunk(rock, CHUNK_TAG::BOUNDS) == nullptr);
        REQUIRE(ASSET_FILE::write_file(path.c_str(), bytes, size));

        // A full load then reads only what is missing and keeps NAME.
        REQUIRE(provider.get(ROCK_GUID) == rock);
        CHECK(rock->find_payload(CHUNK_TAG::NAME) == name);
        CHECK(rock->resident_bytes == RUNTIME_BYTES + 5);
    }

    SUBCASE("by index") {
        const u8* normals = provider.get_chunk_at(rock, 3);
        REQUIRE(normals != nullptr);
        CHECK(normals[0] == 200);
        CHECK(rock->payload(3) == normals);
        CHECK(rock->resident_bytes == 200);
        CHECK(provider.get_chunk_at(rock, 3) == normals);
        CHECK(provider.get_chunk_at(rock, 6) == nullptr);
    }

    SUBCASE("absent and empty chunks") {
        const ChunkEntry* entry = &view.chunks[0];
        CHECK(provider.get_chunk(rock, CHUNK_TAG::PIXELS, &entry) == nullptr);
        CHECK(entry == nullptr);
        CHECK(provider.get_chunk(rock, CHUNK_TAG::INDICES, &entry) == nullptr);
        REQUIRE(entry != nullptr);
        CHECK(entry->size == 0);
        CHECK(provider.get_chunk_at(rock, 4) == nullptr);
        CHECK(provider.resident_bytes() == 0);
    }

    provider.free();
    MEMORY::heap_allocator()->free(prelude);
    std::filesystem::remove(path);
    MEMORY::heap_allocator()->free(bytes);
}

TEST_CASE("asset/asset_resource_provider: unload streams payloads out and keeps the asset known") {
    const std::string path = temp_asset_path("unload");
    usz size = 0;
    u8* bytes = write_mesh_file(path, &size);
    AssetView view;
    u8* prelude = scan(path, view);

    AssetResourceProvider provider;
    AssetResource* rock = provider.get(view); // not added yet
    CHECK(rock == nullptr);
    rock = provider.add(view, path.c_str());
    REQUIRE(provider.get(ROCK_GUID) == rock);
    REQUIRE(rock->is_loaded());

    SUBCASE("by resource") {
        provider.unload(rock);
        CHECK_FALSE(rock->is_loaded());
        CHECK(rock->resident_bytes == 0);
        CHECK(provider.resident_bytes() == 0);
        CHECK(rock->payload(2) == nullptr);
        CHECK(rock->view.is_parsed()); // the prelude stays
        CHECK(provider.find(ROCK_GUID) == rock);

        // The next get reads it again.
        CHECK(provider.get(ROCK_GUID) == rock);
        CHECK(rock->is_loaded());
        CHECK(provider.resident_bytes() == RUNTIME_BYTES);
    }

    SUBCASE("by guid") {
        CHECK(provider.unload(ROCK_GUID));
        CHECK_FALSE(rock->is_loaded());
        CHECK_FALSE(provider.unload(UNKNOWN_GUID));
    }

    SUBCASE("all at once") {
        provider.unload_all();
        CHECK_FALSE(rock->is_loaded());
        CHECK(provider.resident_bytes() == 0);
        CHECK(provider.count() == 1);
    }

    SUBCASE("remove releases a loaded asset") {
        CHECK(provider.remove(ROCK_GUID));
        CHECK(provider.count() == 0);
        CHECK(provider.resident_bytes() == 0);
    }

    provider.free();
    MEMORY::heap_allocator()->free(prelude);
    std::filesystem::remove(path);
    MEMORY::heap_allocator()->free(bytes);
}

TEST_CASE("asset/asset_resource_provider: a file re-imported since the scan is refreshed before it is read") {
    const std::string path = temp_asset_path("stale");
    usz size = 0;
    u8* bytes = write_mesh_file(path, &size);
    AssetView view;
    u8* prelude = scan(path, view);

    AssetResourceProvider provider;
    AssetResource* rock = provider.add(view, path.c_str());
    REQUIRE(provider.get(ROCK_GUID) == rock);
    const u8* old_normals = rock->payload(3);
    REQUIRE(old_normals != nullptr);

    // Re-import: new content hash, an extra SOURCE chunk before the mesh
    // chunks shifts every offset.
    AssetWriter reimported;
    reimported.type = ASSET_TYPE::MESH;
    reimported.guid = ROCK_GUID;
    reimported.content_hash = 0x5678;
    const u8 source[16] = {9, 9, 9};
    reimported.add_chunk(CHUNK_TAG::SOURCE, 1, CHUNK_FLAG::EDITOR_ONLY, source, sizeof(source));
    u8 normals[200];
    for (usz i = 0; i < sizeof(normals); ++i) {
        normals[i] = static_cast<u8>(i * 2);
    }
    reimported.add_chunk(CHUNK_TAG::VERTICES, 1, 0, normals, sizeof(normals));
    usz new_size = 0;
    u8* new_bytes = reimported.write(MEMORY::heap_allocator(), &new_size);
    reimported.free();
    REQUIRE(new_bytes != nullptr);
    REQUIRE(ASSET_FILE::write_file(path.c_str(), new_bytes, new_size));

    SUBCASE("a loaded resource is left alone until something is read") {
        CHECK(provider.get(ROCK_GUID) == rock);
        CHECK(rock->view.header->content_hash == 0x1234);
        CHECK(rock->payload(3) == old_normals);
    }

    SUBCASE("refresh re-reads the prelude and drops the old payloads") {
        CHECK(provider.refresh(rock));
        CHECK(rock->view.header->content_hash == 0x5678);
        CHECK(rock->view.chunk_count() == 2);
        CHECK(rock->prelude_size == ASSET_FILE::prelude_size(0, 2));
        CHECK_FALSE(rock->is_loaded());
        CHECK(rock->resident_bytes == 0);
        CHECK(provider.resident_bytes() == 0);

        REQUIRE(provider.get(ROCK_GUID) == rock);
        const u8* fresh = rock->find_payload(CHUNK_TAG::VERTICES);
        REQUIRE(fresh != nullptr);
        CHECK(fresh[1] == 2);
        CHECK(fresh[100] == 200);
        CHECK(rock->payload(0) == nullptr); // SOURCE is editor-only

        // Refreshing an up-to-date resource changes nothing.
        CHECK(provider.refresh(rock));
        CHECK(rock->is_loaded());
        CHECK(rock->find_payload(CHUNK_TAG::VERTICES) == fresh);
    }

    SUBCASE("an unloaded resource refreshes on its next get") {
        provider.unload(rock);
        REQUIRE(provider.get(ROCK_GUID) == rock);
        CHECK(rock->view.header->content_hash == 0x5678);
        CHECK(rock->view.chunk_count() == 2);
        CHECK(rock->is_loaded());
        CHECK(rock->resident_bytes == 200);
    }

    SUBCASE("get_chunk follows the new table; get_chunk_at refuses the old one") {
        provider.unload(rock);
        CHECK(provider.get_chunk_at(rock, 3) == nullptr); // was VERT in the old table, the file changed
        CHECK(rock->view.chunk_count() == 2);          // but the prelude is now fresh
        CHECK(provider.resident_bytes() == 0);

        provider.unload(rock);
        AssetView old;
        REQUIRE(old.parse(prelude, view.size) == ASSET_PARSE_OK); // the first scan's bytes
        REQUIRE(provider.add(old, path.c_str()) == rock);     // back to the stale prelude
        CHECK(rock->view.header->content_hash == 0x1234);
        const ChunkEntry* entry = nullptr;
        const u8* fresh = provider.get_chunk(rock, CHUNK_TAG::VERTICES, &entry);
        REQUIRE(fresh != nullptr);
        REQUIRE(entry != nullptr);
        CHECK(entry == rock->view.find_chunk(CHUNK_TAG::VERTICES));
        CHECK(rock->view.header->content_hash == 0x5678);
        CHECK(fresh[1] == 2);
    }

    SUBCASE("add with a newer view replaces the prelude without touching the file") {
        AssetView newer;
        u8* newer_prelude = scan(path, newer);
        std::filesystem::remove(path);
        CHECK(provider.add(newer, path.c_str()) == rock);
        CHECK(rock->view.header->content_hash == 0x5678);
        CHECK(rock->view.chunk_count() == 2);
        CHECK_FALSE(rock->is_loaded());
        CHECK(provider.resident_bytes() == 0);
        MEMORY::heap_allocator()->free(newer_prelude);
    }

    SUBCASE("a path that now holds another asset is an error") {
        AssetWriter other;
        other.type = ASSET_TYPE::TEXTURE;
        other.guid = MATERIAL_GUID;
        usz other_size = 0;
        u8* other_bytes = other.write(MEMORY::heap_allocator(), &other_size);
        other.free();
        REQUIRE(ASSET_FILE::write_file(path.c_str(), other_bytes, other_size));
        MEMORY::heap_allocator()->free(other_bytes);

        CHECK_FALSE(provider.refresh(rock));
        CHECK(rock->view.header->content_hash == 0x1234); // unchanged
        CHECK(rock->payload(3) == old_normals);
        provider.unload(rock);
        CHECK(provider.get(ROCK_GUID) == nullptr);
        CHECK(rock->guid == ROCK_GUID);
    }

    provider.free();
    MEMORY::heap_allocator()->free(new_bytes);
    MEMORY::heap_allocator()->free(prelude);
    std::filesystem::remove(path);
    MEMORY::heap_allocator()->free(bytes);
}

TEST_CASE("asset/asset_resource_provider: require_cooked refuses editor files") {
    const std::string path = temp_asset_path("cooked");
    usz size = 0;
    u8* bytes = write_mesh_file(path, &size);
    AssetView view;
    u8* prelude = scan(path, view);

    AssetResourceProvider provider;
    provider.require_cooked = true;
    CHECK(provider.add(view, path.c_str()) == nullptr);
    CHECK(provider.count() == 0);

    // A cooked copy of the same asset is fine.
    AssetWriter cooked;
    cooked.type = ASSET_TYPE::MESH;
    cooked.flags = ASSET_FLAG::COOKED;
    cooked.guid = ROCK_GUID;
    cooked.content_hash = 0x1234;
    const f32 bounds[12] = {-1, -2, -3, 1, 2, 3, 0, 0, 0, 3.75f, 0, 0};
    cooked.add_chunk(CHUNK_TAG::BOUNDS, 1, 0, bounds, sizeof(bounds));
    usz cooked_size = 0;
    u8* cooked_bytes = cooked.write(MEMORY::heap_allocator(), &cooked_size);
    cooked.free();
    REQUIRE(ASSET_FILE::write_file(path.c_str(), cooked_bytes, cooked_size));
    AssetView cooked_view;
    u8* cooked_prelude = scan(path, cooked_view);

    AssetResource* rock = provider.add(cooked_view, path.c_str());
    REQUIRE(rock != nullptr);
    REQUIRE(provider.get(ROCK_GUID) == rock);
    CHECK(rock->resident_bytes == 48);

    // The file reverting to an editor file is caught on refresh.
    REQUIRE(ASSET_FILE::write_file(path.c_str(), bytes, size));
    CHECK_FALSE(provider.refresh(rock));
    CHECK(rock->view.is_cooked());

    provider.free();
    MEMORY::heap_allocator()->free(cooked_prelude);
    MEMORY::heap_allocator()->free(cooked_bytes);
    MEMORY::heap_allocator()->free(prelude);
    std::filesystem::remove(path);
    MEMORY::heap_allocator()->free(bytes);
}

TEST_CASE("asset/asset_resource_provider: resources keep their addresses as others are added") {
    AssetResourceProvider provider;
    std::string paths[40];
    u8* buffers[40];
    AssetResource* resources[40];

    for (u32 i = 0; i < 40; ++i) {
        AssetWriter writer;
        writer.type = ASSET_TYPE::TEXTURE;
        writer.guid = {0x1000 + i, 0x2000};
        writer.content_hash = i;
        const u8 pixel[4] = {static_cast<u8>(i), 0, 0, 0};
        writer.add_chunk(CHUNK_TAG::PIXELS, 1, 0, pixel, sizeof(pixel));
        usz size = 0;
        u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
        writer.free();
        REQUIRE(bytes != nullptr);
        paths[i] = temp_asset_path(("many_" + std::to_string(i)).c_str());
        REQUIRE(ASSET_FILE::write_file(paths[i].c_str(), bytes, size));
        MEMORY::heap_allocator()->free(bytes);

        AssetView view;
        buffers[i] = scan(paths[i], view);
        resources[i] = provider.add(view, paths[i].c_str());
        REQUIRE(resources[i] != nullptr);
        if (i % 2 == 0) {
            REQUIRE(provider.get(view) == resources[i]);
        }
    }
    CHECK(provider.count() == 40);
    CHECK(provider.resident_bytes() == 20 * 4);

    bool stable = true;
    for (u32 i = 0; i < 40; ++i) {
        const AssetGuid guid = {0x1000 + i, 0x2000};
        stable = stable && provider.find(guid) == resources[i];
        stable = stable && resources[i]->view.header->content_hash == i;
        const u8* pixel = provider.get_chunk(resources[i], CHUNK_TAG::PIXELS);
        stable = stable && pixel != nullptr && pixel[0] == i;
    }
    CHECK(stable);
    CHECK(provider.resident_bytes() == 40 * 4);

    provider.free();
    CHECK(provider.resident_bytes() == 0);
    for (u32 i = 0; i < 40; ++i) {
        MEMORY::heap_allocator()->free(buffers[i]);
        std::filesystem::remove(paths[i]);
    }
}
