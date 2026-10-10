#include "support/test_support.hpp"

#include "engine/asset/asset_reader.hpp"
#include "engine/asset/asset_resource_provider.hpp"
#include "engine/asset/asset_view.hpp"
#include "engine/asset/asset_writer.hpp"
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

    writer.add_chunk(CHUNK_TYPE::NAME, 1, CHUNK_FLAG::EDITOR_ONLY, name, sizeof(name));
    writer.add_chunk(CHUNK_TYPE::MESH, 1, 0, nullptr, 0);
    writer.add_chunk(CHUNK_TYPE::VERTICES, 1, 0, positions, sizeof(positions));
    writer.add_chunk(CHUNK_TYPE::VERTICES, 1, 0, normals, sizeof(normals));
    writer.add_chunk(CHUNK_TYPE::INDICES, 1, 0, nullptr, 0);
    writer.add_chunk(CHUNK_TYPE::BOUNDS, 1, 0, bounds, sizeof(bounds));
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

// Reads and parses the prelude of `path`; release it with free_prelude.
AssetView scan(const std::string& path) {
    AssetView view = ASSET_FILE::read_prelude(path.c_str(), MEMORY::heap_allocator());
    REQUIRE(view.is_ok());
    return view;
}

void free_scan(AssetView* view) {
    ASSET_FILE::free_prelude(view, MEMORY::heap_allocator());
}

} // namespace

TEST_CASE("asset/asset_resource_provider: add copies the prelude and reads nothing") {
    const std::string path = temp_asset_path("add");
    usz size = 0;
    u8* bytes = write_mesh_file(path, &size);
    AssetView view = scan(path);

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
    CHECK(rock->view.data != view.data);
    CHECK(rock->view.size == view.size);
    CHECK(is_aligned(rock->view.data, ASSET_FILE::PAYLOAD_ALIGNMENT));
    CHECK(rock->view.is_ok());
    CHECK(rock->view.chunk_count() == 6);
    CHECK(rock->view.dependencies[0] == MATERIAL_GUID);

    // The caller's buffer can go: the resource does not point into it.
    free_scan(&view);
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
    CHECK(rock->find_payload(CHUNK_TYPE::BOUNDS, &entry) == nullptr);
    CHECK(entry == rock->view.find_chunk(CHUNK_TYPE::BOUNDS)); // the entry is known even unloaded
    CHECK(rock->find_payload(CHUNK_TYPE::PIXELS, &entry) == nullptr);
    CHECK(entry == nullptr);

    SUBCASE("an unparsed view is refused") {
        AssetView empty;
        CHECK(provider.add(empty, path.c_str()) == nullptr);
        CHECK(provider.count() == 1);
    }

    SUBCASE("adding the same asset again returns the same resource") {
        AssetView again = scan(path);
        CHECK(provider.add(again, path.c_str()) == rock);
        CHECK(provider.count() == 1);

        // A new path for the same file (moved in the OS) is taken over.
        const std::string moved = temp_asset_path("add_moved");
        CHECK(provider.add(again, moved.c_str()) == rock);
        CHECK(doctest::String(rock->path) == moved.c_str());
        free_scan(&again);
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
    AssetView view = scan(path);

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
        const u8* positions = rock->find_payload(CHUNK_TYPE::VERTICES, &entry);
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

        const f32* box = reinterpret_cast<const f32*>(rock->find_payload(CHUNK_TYPE::BOUNDS));
        REQUIRE(box != nullptr);
        CHECK(box[0] == -1.0f);
        CHECK(box[9] == 3.75f);

        // Empty chunks are resident with no bytes; the editor-only NAME was skipped.
        CHECK(rock->payload(1) == nullptr);
        CHECK(rock->is_resident(1));
        CHECK(rock->find_payload(CHUNK_TYPE::INDICES, &entry) == nullptr);
        CHECK(entry->size == 0);
        CHECK(rock->payload(0) == nullptr);
        CHECK_FALSE(rock->is_resident(0));

        // Loaded means loaded: the file is not needed again.
        std::filesystem::remove(path);
        CHECK(provider.get(ROCK_GUID) == rock);
        CHECK(rock->find_payload(CHUNK_TYPE::VERTICES) == positions);
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
    free_scan(&view);
    std::filesystem::remove(path);
    MEMORY::heap_allocator()->free(bytes);
}

TEST_CASE("asset/asset_resource_provider: get_chunk reads one chunk on demand") {
    const std::string path = temp_asset_path("chunk");
    usz size = 0;
    u8* bytes = write_mesh_file(path, &size);
    AssetView view = scan(path);

    AssetResourceProvider provider;
    AssetResource* rock = provider.add(view, path.c_str());
    REQUIRE(rock != nullptr);

    SUBCASE("by tag, including editor-only chunks") {
        const ChunkEntry* entry = nullptr;
        const u8* name = provider.get_chunk(rock, CHUNK_TYPE::NAME, &entry);
        REQUIRE(name != nullptr);
        REQUIRE(entry != nullptr);
        CHECK(entry->tag == CHUNK_TYPE::NAME);
        CHECK(doctest::String(reinterpret_cast<const char*>(name)) == "rock");
        CHECK(rock->is_resident(0));
        CHECK_FALSE(rock->is_loaded()); // only NAME is in
        CHECK(rock->resident_bytes == 5);
        CHECK(provider.resident_bytes() == 5);

        // Asking again hands out the same buffer without a read.
        std::filesystem::remove(path);
        CHECK(provider.get_chunk(rock, CHUNK_TYPE::NAME) == name);
        CHECK(rock->find_payload(CHUNK_TYPE::NAME) == name);
        // A chunk that is not resident needs the file.
        CHECK(provider.get_chunk(rock, CHUNK_TYPE::BOUNDS) == nullptr);
        REQUIRE(ASSET_FILE::write_file(path.c_str(), bytes, size));

        // A full load then reads only what is missing and keeps NAME.
        REQUIRE(provider.get(ROCK_GUID) == rock);
        CHECK(rock->find_payload(CHUNK_TYPE::NAME) == name);
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
        CHECK(provider.get_chunk(rock, CHUNK_TYPE::PIXELS, &entry) == nullptr);
        CHECK(entry == nullptr);
        CHECK(provider.get_chunk(rock, CHUNK_TYPE::INDICES, &entry) == nullptr);
        REQUIRE(entry != nullptr);
        CHECK(entry->size == 0);
        CHECK(provider.get_chunk_at(rock, 4) == nullptr);
        CHECK(provider.resident_bytes() == 0);
    }

    provider.free();
    free_scan(&view);
    std::filesystem::remove(path);
    MEMORY::heap_allocator()->free(bytes);
}

TEST_CASE("asset/asset_resource_provider: unload streams payloads out and keeps the asset known") {
    const std::string path = temp_asset_path("unload");
    usz size = 0;
    u8* bytes = write_mesh_file(path, &size);
    AssetView view = scan(path);

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
        CHECK(rock->view.is_ok()); // the prelude stays
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
    free_scan(&view);
    std::filesystem::remove(path);
    MEMORY::heap_allocator()->free(bytes);
}

TEST_CASE("asset/asset_resource_provider: a file re-imported since the scan is refreshed before it is read") {
    const std::string path = temp_asset_path("stale");
    usz size = 0;
    u8* bytes = write_mesh_file(path, &size);
    AssetView view = scan(path);

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
    reimported.add_chunk(CHUNK_TYPE::SOURCE, 1, CHUNK_FLAG::EDITOR_ONLY, source, sizeof(source));
    u8 normals[200];
    for (usz i = 0; i < sizeof(normals); ++i) {
        normals[i] = static_cast<u8>(i * 2);
    }
    reimported.add_chunk(CHUNK_TYPE::VERTICES, 1, 0, normals, sizeof(normals));
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
        CHECK(rock->view.size == ASSET_FILE::prelude_size(0, 2));
        CHECK_FALSE(rock->is_loaded());
        CHECK(rock->resident_bytes == 0);
        CHECK(provider.resident_bytes() == 0);

        REQUIRE(provider.get(ROCK_GUID) == rock);
        const u8* fresh = rock->find_payload(CHUNK_TYPE::VERTICES);
        REQUIRE(fresh != nullptr);
        CHECK(fresh[1] == 2);
        CHECK(fresh[100] == 200);
        CHECK(rock->payload(0) == nullptr); // SOURCE is editor-only

        // Refreshing an up-to-date resource changes nothing.
        CHECK(provider.refresh(rock));
        CHECK(rock->is_loaded());
        CHECK(rock->find_payload(CHUNK_TYPE::VERTICES) == fresh);
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
        const AssetView old = AssetView::parse(view.data, view.size); // the first scan's bytes
        REQUIRE(old.is_ok());
        REQUIRE(provider.add(old, path.c_str()) == rock);     // back to the stale prelude
        CHECK(rock->view.header->content_hash == 0x1234);
        const ChunkEntry* entry = nullptr;
        const u8* fresh = provider.get_chunk(rock, CHUNK_TYPE::VERTICES, &entry);
        REQUIRE(fresh != nullptr);
        REQUIRE(entry != nullptr);
        CHECK(entry == rock->view.find_chunk(CHUNK_TYPE::VERTICES));
        CHECK(rock->view.header->content_hash == 0x5678);
        CHECK(fresh[1] == 2);
    }

    SUBCASE("add with a newer view replaces the prelude without touching the file") {
        AssetView newer = scan(path);
        std::filesystem::remove(path);
        CHECK(provider.add(newer, path.c_str()) == rock);
        CHECK(rock->view.header->content_hash == 0x5678);
        CHECK(rock->view.chunk_count() == 2);
        CHECK_FALSE(rock->is_loaded());
        CHECK(provider.resident_bytes() == 0);
        free_scan(&newer);
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
    free_scan(&view);
    std::filesystem::remove(path);
    MEMORY::heap_allocator()->free(bytes);
}

TEST_CASE("asset/asset_resource_provider: require_cooked refuses editor files") {
    const std::string path = temp_asset_path("cooked");
    usz size = 0;
    u8* bytes = write_mesh_file(path, &size);
    AssetView view = scan(path);

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
    cooked.add_chunk(CHUNK_TYPE::BOUNDS, 1, 0, bounds, sizeof(bounds));
    usz cooked_size = 0;
    u8* cooked_bytes = cooked.write(MEMORY::heap_allocator(), &cooked_size);
    cooked.free();
    REQUIRE(ASSET_FILE::write_file(path.c_str(), cooked_bytes, cooked_size));
    AssetView cooked_view = scan(path);

    AssetResource* rock = provider.add(cooked_view, path.c_str());
    REQUIRE(rock != nullptr);
    REQUIRE(provider.get(ROCK_GUID) == rock);
    CHECK(rock->resident_bytes == 48);

    // The file reverting to an editor file is caught on refresh.
    REQUIRE(ASSET_FILE::write_file(path.c_str(), bytes, size));
    CHECK_FALSE(provider.refresh(rock));
    CHECK(rock->view.is_cooked());

    provider.free();
    free_scan(&cooked_view);
    MEMORY::heap_allocator()->free(cooked_bytes);
    free_scan(&view);
    std::filesystem::remove(path);
    MEMORY::heap_allocator()->free(bytes);
}

TEST_CASE("asset/asset_resource_provider: resources keep their addresses as others are added") {
    AssetResourceProvider provider;
    std::string paths[40];
    AssetView views[40];
    AssetResource* resources[40];

    for (u32 i = 0; i < 40; ++i) {
        AssetWriter writer;
        writer.type = ASSET_TYPE::TEXTURE;
        writer.guid = {0x1000 + i, 0x2000};
        writer.content_hash = i;
        const u8 pixel[4] = {static_cast<u8>(i), 0, 0, 0};
        writer.add_chunk(CHUNK_TYPE::PIXELS, 1, 0, pixel, sizeof(pixel));
        usz size = 0;
        u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
        writer.free();
        REQUIRE(bytes != nullptr);
        paths[i] = temp_asset_path(("many_" + std::to_string(i)).c_str());
        REQUIRE(ASSET_FILE::write_file(paths[i].c_str(), bytes, size));
        MEMORY::heap_allocator()->free(bytes);

        views[i] = scan(paths[i]);
        resources[i] = provider.add(views[i], paths[i].c_str());
        REQUIRE(resources[i] != nullptr);
        if (i % 2 == 0) {
            REQUIRE(provider.get(views[i]) == resources[i]);
        }
    }
    CHECK(provider.count() == 40);
    CHECK(provider.resident_bytes() == 20 * 4);

    bool stable = true;
    for (u32 i = 0; i < 40; ++i) {
        const AssetGuid guid = {0x1000 + i, 0x2000};
        stable = stable && provider.find(guid) == resources[i];
        stable = stable && resources[i]->view.header->content_hash == i;
        const u8* pixel = provider.get_chunk(resources[i], CHUNK_TYPE::PIXELS);
        stable = stable && pixel != nullptr && pixel[0] == i;
    }
    CHECK(stable);
    CHECK(provider.resident_bytes() == 40 * 4);

    provider.free();
    CHECK(provider.resident_bytes() == 0);
    for (u32 i = 0; i < 40; ++i) {
        free_scan(&views[i]);
        std::filesystem::remove(paths[i]);
    }
}

// --- Text assets --------------------------------------------------------------

#include "engine/asset/text_asset.hpp"

#include <fstream>

namespace {

const AssetGuid TEXT_GUID = {0x179a52dd06b33f96ull, 0xe26f6a98c55cbe22ull};
constexpr const char* TEXT_GUID_TEXT = "e26f6a98c55cbe22179a52dd06b33f96";

std::string temp_material_path(const char* stem) {
    return (std::filesystem::temp_directory_path() / (std::string("luau_engine_provider_") + stem + ".material")).string();
}

void write_text_file(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string material_text(const char* shader) {
    return "guid = " + std::string(TEXT_GUID_TEXT) + "\nshader = " + shader + "\n";
}

AssetView scan_text(const std::string& path) {
    AssetView view = TEXT_ASSET::read_prelude(path.c_str(), MEMORY::heap_allocator());
    REQUIRE(view.is_ok());
    return view;
}

} // namespace

TEST_CASE("asset/asset_resource_provider: a text asset loads its file as the TEXT chunk") {
    const std::string path = temp_material_path("text");
    const std::string text = material_text("unlit");
    write_text_file(path, text);
    AssetView view = scan_text(path);

    AssetResourceProvider provider;
    AssetResource* material = provider.add(view, path.c_str());
    REQUIRE(material != nullptr);
    free_scan(&view);
    CHECK(material->is_text());
    CHECK(material->guid == TEXT_GUID);
    CHECK(material->view.header->type == ASSET_TYPE::MATERIAL);
    CHECK_FALSE(material->is_loaded());
    CHECK(provider.resident_bytes() == 0);

    REQUIRE(provider.get(TEXT_GUID) == material);
    CHECK(material->is_loaded());
    const ChunkEntry* entry = nullptr;
    const u8* payload = material->find_payload(CHUNK_TYPE::TEXT, &entry);
    REQUIRE(payload != nullptr);
    REQUIRE(entry != nullptr);
    CHECK(entry->size == text.size());
    CHECK(std::memcmp(payload, text.data(), text.size()) == 0);
    CHECK(is_aligned(payload, ASSET_FILE::PAYLOAD_ALIGNMENT));
    CHECK(material->resident_bytes == text.size());
    CHECK(provider.resident_bytes() == text.size());

    SUBCASE("a second get reads nothing and returns the same payload") {
        std::filesystem::remove(path);
        CHECK(provider.get(TEXT_GUID) == material);
        CHECK(material->find_payload(CHUNK_TYPE::TEXT) == payload);
    }
    SUBCASE("unload streams the text out and get reads it again") {
        provider.unload(material);
        CHECK_FALSE(material->is_loaded());
        CHECK(provider.resident_bytes() == 0);
        REQUIRE(provider.get(TEXT_GUID) == material);
        const u8* again = material->find_payload(CHUNK_TYPE::TEXT, &entry);
        REQUIRE(again != nullptr);
        CHECK(std::memcmp(again, text.data(), text.size()) == 0);
    }
    SUBCASE("get_chunk reads the one chunk") {
        provider.unload(material);
        const u8* chunk = provider.get_chunk(material, CHUNK_TYPE::TEXT, &entry);
        REQUIRE(chunk != nullptr);
        CHECK(entry->size == text.size());
        CHECK(provider.get_chunk(material, CHUNK_TYPE::MESH) == nullptr);
    }
    SUBCASE("the cooked-only runtime accepts text assets") {
        AssetResourceProvider runtime;
        runtime.require_cooked = true;
        AssetView again = scan_text(path);
        CHECK(runtime.add(again, path.c_str()) != nullptr);
        free_scan(&again);
        runtime.free();
    }
    provider.free();
    std::filesystem::remove(path);
    CHECK_ARENA_CLEAN();
}

TEST_CASE("asset/asset_resource_provider: an edited text asset is refreshed before it is read") {
    const std::string path = temp_material_path("edited");
    const std::string old_text = material_text("unlit");
    write_text_file(path, old_text);
    AssetView view = scan_text(path);
    AssetResourceProvider provider;
    AssetResource* material = provider.add(view, path.c_str());
    REQUIRE(material != nullptr);
    free_scan(&view);
    REQUIRE(provider.get(TEXT_GUID) != nullptr);
    const u64 old_hash = material->view.header->content_hash;

    // Edit the file: same guid, longer text.
    const std::string new_text = material_text("textured") + "\n[params]\ntint = 1 1 1 1\n";
    write_text_file(path, new_text);

    SUBCASE("get with the text resident does not notice (nothing is read)") {
        CHECK(provider.get(TEXT_GUID) == material);
        CHECK(material->view.header->content_hash == old_hash);
    }
    SUBCASE("refresh takes the new prelude and drops the old text") {
        REQUIRE(provider.refresh(material));
        CHECK(material->view.header->content_hash != old_hash);
        CHECK(material->view.find_chunk(CHUNK_TYPE::TEXT)->size == new_text.size());
        CHECK_FALSE(material->is_loaded());
        CHECK(provider.resident_bytes() == 0);
        REQUIRE(provider.get(TEXT_GUID) == material);
        const ChunkEntry* entry = nullptr;
        const u8* payload = material->find_payload(CHUNK_TYPE::TEXT, &entry);
        REQUIRE(payload != nullptr);
        CHECK(entry->size == new_text.size());
        CHECK(std::memcmp(payload, new_text.data(), new_text.size()) == 0);
    }
    SUBCASE("get after unload reads the new version") {
        provider.unload(material);
        REQUIRE(provider.get(TEXT_GUID) == material);
        const ChunkEntry* entry = nullptr;
        const u8* payload = material->find_payload(CHUNK_TYPE::TEXT, &entry);
        REQUIRE(payload != nullptr);
        CHECK(entry->size == new_text.size());
        CHECK(provider.resident_bytes() == new_text.size());
    }
    SUBCASE("a file that now holds another guid is refused and the resource is unchanged") {
        write_text_file(path, "guid = 99aabbccddeeff001122334455667788\nshader = unlit\n");
        provider.unload(material);
        CHECK(provider.get(TEXT_GUID) == nullptr);
        CHECK(material->view.header->content_hash == old_hash);
        CHECK(provider.find(TEXT_GUID) == material);
    }
    SUBCASE("a file that lost its guid is refused") {
        write_text_file(path, "shader = unlit\n");
        provider.unload(material);
        CHECK(provider.get(TEXT_GUID) == nullptr);
        CHECK(material->view.header->content_hash == old_hash);
    }
    SUBCASE("a file that disappeared is an error") {
        std::filesystem::remove(path);
        provider.unload(material);
        CHECK(provider.get(TEXT_GUID) == nullptr);
        CHECK_FALSE(provider.refresh(material));
    }
    provider.free();
    std::filesystem::remove(path);
    CHECK_ARENA_CLEAN();
}
