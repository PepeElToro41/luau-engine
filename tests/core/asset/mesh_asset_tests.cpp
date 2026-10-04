#include "support/test_support.hpp"

#include "engine/asset/asset_file.hpp"
#include "engine/asset/asset_reader.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"

#include <cstring>
#include <filesystem>
#include <string>

namespace {

const AssetGuid MESH_GUID = {0xabcd, 0xef01};
const AssetGuid MATERIAL_GUID = {0x1, 0x2};

std::string temp_asset_path(const char* stem) {
    return (std::filesystem::temp_directory_path() / (std::string("luau_engine_mesh_") + stem + ASSET_FILE::EXTENSION)).string();
}

// A quad as two triangles with two vertex streams (positions; normals +
// texcoords interleaved), 16-bit indices, one submesh per triangle. Tests
// mutate the tables before writing to exercise the validation.
struct MeshBuilder {
    MeshDesc desc;
    DynamicArray<VertexStreamDesc> streams;
    DynamicArray<VertexAttributeDesc> attributes;
    DynamicArray<SubmeshDesc> submeshes;
    DynamicArray<u8> stream_bytes[2];
    DynamicArray<u8> indices;
    MeshBounds bounds;
    bool with_material = true;

    void init() {
        this->desc.vertex_count = 4;
        this->desc.index_count = 6;
        this->desc.index_format = MESH_INDEX_U16;
        this->desc.stream_count = 2;
        this->desc.attribute_count = 3;
        this->desc.submesh_count = 2;

        VertexStreamDesc positions;
        positions.stride = 12;
        VertexStreamDesc shading;
        shading.stride = 16;
        this->streams.push(positions);
        this->streams.push(shading);

        VertexAttributeDesc position;
        position.semantic = VERTEX_SEMANTIC_POSITION;
        position.format = VERTEX_FORMAT_F32x3;
        position.stream = 0;
        VertexAttributeDesc normal;
        normal.semantic = VERTEX_SEMANTIC_NORMAL;
        normal.format = VERTEX_FORMAT_SNORM16x4;
        normal.stream = 1;
        VertexAttributeDesc texcoord;
        texcoord.semantic = VERTEX_SEMANTIC_TEXCOORD;
        texcoord.format = VERTEX_FORMAT_F32x2;
        texcoord.stream = 1;
        texcoord.offset = 8;
        this->attributes.push(position);
        this->attributes.push(normal);
        this->attributes.push(texcoord);

        SubmeshDesc first;
        first.first_index = 0;
        first.index_count = 3;
        first.material = 0;
        first.bounds_max[0] = 1;
        SubmeshDesc second;
        second.first_index = 3;
        second.index_count = 3;
        second.material = MESH_ASSET::NO_MATERIAL;
        this->submeshes.push(first);
        this->submeshes.push(second);

        const f32 positions_data[12] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
        const u8* position_bytes = reinterpret_cast<const u8*>(positions_data);
        for (usz i = 0; i < sizeof(positions_data); ++i) {
            this->stream_bytes[0].push(position_bytes[i]);
        }
        for (usz i = 0; i < 16 * 4; ++i) {
            this->stream_bytes[1].push(static_cast<u8>(i));
        }
        const u16 index_data[6] = {0, 1, 2, 0, 2, 3};
        const u8* index_bytes = reinterpret_cast<const u8*>(index_data);
        for (usz i = 0; i < sizeof(index_data); ++i) {
            this->indices.push(index_bytes[i]);
        }

        this->bounds.max[0] = 1;
        this->bounds.max[1] = 1;
        this->bounds.center[0] = 0.5f;
        this->bounds.center[1] = 0.5f;
        this->bounds.radius = 0.75f;
    }

    // The MESH payload: desc + the three tables, in a fresh aligned heap
    // buffer the caller frees. This is what read_chunk hands a loader.
    u8* build_payload(usz* out_size) const {
        const usz stream_bytes = sizeof(VertexStreamDesc) * this->streams.count;
        const usz attribute_bytes = sizeof(VertexAttributeDesc) * this->attributes.count;
        const usz submesh_bytes = sizeof(SubmeshDesc) * this->submeshes.count;
        const usz size = sizeof(MeshDesc) + stream_bytes + attribute_bytes + submesh_bytes;
        u8* payload = static_cast<u8*>(MEMORY::heap_allocator()->allocate(size, ASSET_FILE::PAYLOAD_ALIGNMENT));
        u8* cursor = payload;
        std::memcpy(cursor, &this->desc, sizeof(MeshDesc));
        cursor += sizeof(MeshDesc);
        std::memcpy(cursor, this->streams.data, stream_bytes);
        cursor += stream_bytes;
        std::memcpy(cursor, this->attributes.data, attribute_bytes);
        cursor += attribute_bytes;
        std::memcpy(cursor, this->submeshes.data, submesh_bytes);
        *out_size = size;
        return payload;
    }

    void write(AssetWriter& writer, const u32 version = MESH_ASSET::VERSION) const {
        writer.type = ASSET_TYPE::MESH;
        writer.guid = MESH_GUID;
        if (this->with_material) {
            writer.add_dependency(MATERIAL_GUID);
        }
        usz payload_size = 0;
        u8* payload = this->build_payload(&payload_size);
        writer.add_chunk(CHUNK_TAG::MESH, version, 0, payload, payload_size);
        MEMORY::heap_allocator()->free(payload);
        for (usz i = 0; i < this->streams.count && i < 2; ++i) {
            writer.add_chunk(CHUNK_TAG::VERTICES, version, 0, this->stream_bytes[i].data, this->stream_bytes[i].count);
        }
        writer.add_chunk(CHUNK_TAG::INDICES, version, 0, this->indices.data, this->indices.count);
        writer.add_chunk(CHUNK_TAG::BOUNDS, version, 0, &this->bounds, sizeof(MeshBounds));
    }

    void free() {
        this->streams.free();
        this->attributes.free();
        this->submeshes.free();
        this->stream_bytes[0].free();
        this->stream_bytes[1].free();
        this->indices.free();
    }
};

// Writes the file in memory, parses its prelude into `file` and the MESH
// payload into `mesh`, as a loader would after read_prelude and read_chunk.
// The file buffer and the payload are left for the caller to free.
MeshParseError round_trip(const MeshBuilder& builder, AssetView& file, MeshAssetView& mesh, u8** bytes, u8** payload) {
    AssetWriter writer;
    builder.write(writer);
    usz size = 0;
    *bytes = writer.write(MEMORY::heap_allocator(), &size);
    writer.free();
    REQUIRE(*bytes != nullptr);
    REQUIRE(file.parse(*bytes, size) == ASSET_PARSE_OK);
    usz payload_size = 0;
    *payload = builder.build_payload(&payload_size);
    return mesh.parse(file, *payload, payload_size);
}

void free_round_trip(u8* bytes, u8* payload) {
    MEMORY::heap_allocator()->free(payload);
    MEMORY::heap_allocator()->free(bytes);
}

// Rewrites `source`'s chunks into `out`, keeping the first `keep_vertex`
// VERT chunks and duplicating the last one `extra_vertex` times.
void copy_chunks(const AssetView& source, const u8* bytes, AssetWriter& out, const usz keep_vertex, const usz extra_vertex) {
    out.type = source.header->type;
    out.guid = source.header->guid;
    for (usz i = 0; i < source.dependency_count(); ++i) {
        out.add_dependency(source.dependencies[i]);
    }
    usz vertex_chunks = 0;
    for (usz i = 0; i < source.chunk_count(); ++i) {
        const ChunkEntry& chunk = source.chunks[i];
        if (chunk.tag == CHUNK_TAG::VERTICES && vertex_chunks++ >= keep_vertex) {
            continue;
        }
        out.add_chunk(chunk.tag, chunk.version, chunk.flags, bytes + chunk.offset, chunk.size);
        if (chunk.tag == CHUNK_TAG::VERTICES && vertex_chunks == keep_vertex) {
            for (usz extra = 0; extra < extra_vertex; ++extra) {
                out.add_chunk(chunk.tag, chunk.version, chunk.flags, bytes + chunk.offset, chunk.size);
            }
        }
    }
}

} // namespace

TEST_CASE("asset/mesh_asset: on-disk structs have their documented sizes") {
    CHECK(sizeof(MeshDesc) == 64);
    CHECK(sizeof(VertexStreamDesc) == 8);
    CHECK(sizeof(VertexAttributeDesc) == 8);
    CHECK(sizeof(SubmeshDesc) == 48);
    CHECK(sizeof(MeshBounds) == 48);
    CHECK(MESH_ASSET::desc_size(0, 0, 0) == 64);
    CHECK(MESH_ASSET::desc_size(2, 3, 2) == 64 + 16 + 24 + 96);
}

TEST_CASE("asset/mesh_asset: vertex and index formats know their sizes") {
    CHECK(VERTEX_FORMAT::size(VERTEX_FORMAT_F32x3) == 12);
    CHECK(VERTEX_FORMAT::size(VERTEX_FORMAT_F16x4) == 8);
    CHECK(VERTEX_FORMAT::size(VERTEX_FORMAT_SNORM8x4) == 4);
    CHECK(VERTEX_FORMAT::size(VERTEX_FORMAT_UINT16x4) == 8);
    CHECK(VERTEX_FORMAT::size(VERTEX_FORMAT_NONE) == 0);
    CHECK(VERTEX_FORMAT::size(0xff) == 0);
    CHECK(doctest::String(VERTEX_FORMAT::name(VERTEX_FORMAT_UNORM8x4)) == "UNORM8x4");
    CHECK(doctest::String(VERTEX_FORMAT::name(0xff)) == "unknown");

    CHECK(MESH_ASSET::index_size(MESH_INDEX_U16) == 2);
    CHECK(MESH_ASSET::index_size(MESH_INDEX_U32) == 4);
    CHECK(MESH_ASSET::index_size(2) == 0);
}

TEST_CASE("asset/mesh_asset: a view starts empty and rejects files of other types") {
    MeshAssetView mesh;
    CHECK_FALSE(mesh.is_parsed());
    CHECK(mesh.find_attribute(VERTEX_SEMANTIC_POSITION, 0) == nullptr);
    CHECK(mesh.stream_size(0) == 0);
    CHECK(mesh.index_bytes() == 0);
    CHECK(mesh.index_chunk == nullptr);
    CHECK(mesh.bounds_chunk == nullptr);

    AssetView empty;
    MeshDesc desc;
    CHECK(mesh.parse(empty, &desc, sizeof(desc)) == MESH_PARSE_NOT_A_MESH);

    AssetWriter writer;
    writer.type = ASSET_TYPE::TEXTURE;
    writer.guid = MESH_GUID;
    usz size = 0;
    u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
    writer.free();
    REQUIRE(bytes != nullptr);
    AssetView file;
    REQUIRE(file.parse(bytes, size) == ASSET_PARSE_OK);
    CHECK(mesh.parse(file, &desc, sizeof(desc)) == MESH_PARSE_NOT_A_MESH);
    MEMORY::heap_allocator()->free(bytes);

    CHECK(doctest::String(MESH_ASSET::parse_error_name(MESH_PARSE_OK)) == "ok");
    CHECK(doctest::String(MESH_ASSET::parse_error_name(MESH_PARSE_BAD_SUBMESH)) != "ok");
}

TEST_CASE("asset/mesh_asset: a two-stream quad parses from its prelude and MESH payload") {
    MeshBuilder builder;
    builder.init();

    AssetView file;
    MeshAssetView mesh;
    u8* bytes = nullptr;
    u8* payload = nullptr;
    REQUIRE(round_trip(builder, file, mesh, &bytes, &payload) == MESH_PARSE_OK);

    CHECK(mesh.is_parsed());
    CHECK(mesh.desc == reinterpret_cast<const MeshDesc*>(payload)); // points into the payload
    CHECK(mesh.desc->vertex_count == 4);
    CHECK(mesh.desc->index_count == 6);
    CHECK(mesh.desc->stream_count == 2);
    CHECK(mesh.streams[0].stride == 12);
    CHECK(mesh.streams[1].stride == 16);
    CHECK(mesh.stream_size(0) == 48);
    CHECK(mesh.stream_size(1) == 64);
    CHECK(mesh.stream_size(2) == 0);
    CHECK(mesh.index_bytes() == 12);

    SUBCASE("attributes are found by semantic and index") {
        const VertexAttributeDesc* position = mesh.find_attribute(VERTEX_SEMANTIC_POSITION, 0);
        REQUIRE(position != nullptr);
        CHECK(position->format == VERTEX_FORMAT_F32x3);
        CHECK(position->stream == 0);
        CHECK(position->offset == 0);
        const VertexAttributeDesc* texcoord = mesh.find_attribute(VERTEX_SEMANTIC_TEXCOORD, 0);
        REQUIRE(texcoord != nullptr);
        CHECK(texcoord->stream == 1);
        CHECK(texcoord->offset == 8);
        CHECK(mesh.find_attribute(VERTEX_SEMANTIC_TEXCOORD, 1) == nullptr);
        CHECK(mesh.find_attribute(VERTEX_SEMANTIC_COLOR, 0) == nullptr);
    }

    SUBCASE("the geometry chunk entries are the file's, in stream order") {
        const ChunkEntry* first = file.find_chunk(CHUNK_TAG::VERTICES);
        CHECK(mesh.vertex_chunks[0] == first);
        CHECK(mesh.vertex_chunks[1] == file.find_chunk(CHUNK_TAG::VERTICES, first));
        CHECK(mesh.vertex_chunks[2] == nullptr);
        CHECK(mesh.vertex_chunks[0]->size == 48);
        CHECK(mesh.vertex_chunks[1]->size == 64);
        CHECK(mesh.index_chunk == file.find_chunk(CHUNK_TAG::INDICES));
        CHECK(mesh.index_chunk->size == 12);
        CHECK(mesh.bounds_chunk == file.find_chunk(CHUNK_TAG::BOUNDS));
        CHECK(mesh.bounds_chunk->size == sizeof(MeshBounds));

        // The in-memory file holds the bytes the entries point at.
        f32 third_vertex[3];
        std::memcpy(third_vertex, bytes + mesh.vertex_chunks[0]->offset + 12 * 2, sizeof(third_vertex));
        CHECK(third_vertex[0] == 1.0f);
        CHECK(third_vertex[1] == 1.0f);
        u16 indices[6];
        std::memcpy(indices, bytes + mesh.index_chunk->offset, sizeof(indices));
        CHECK(indices[4] == 2);
        CHECK(indices[5] == 3);
        MeshBounds bounds;
        std::memcpy(&bounds, bytes + mesh.bounds_chunk->offset, sizeof(bounds));
        CHECK(bounds.max[1] == 1.0f);
        CHECK(bounds.center[0] == 0.5f);
        CHECK(bounds.radius == 0.75f);
    }

    SUBCASE("submeshes reference the dependency table for materials") {
        CHECK(mesh.submeshes[0].material == 0);
        CHECK(file.dependencies[mesh.submeshes[0].material] == MATERIAL_GUID);
        CHECK(mesh.submeshes[1].material == MESH_ASSET::NO_MATERIAL);
        CHECK(mesh.submeshes[1].first_index == 3);
        CHECK(mesh.submeshes[0].bounds_max[0] == 1.0f);
    }

    free_round_trip(bytes, payload);
    builder.free();
}

TEST_CASE("asset/mesh_asset: loading through a file reads the tables, then each geometry chunk by its entry") {
    const std::string path = temp_asset_path("load");
    MeshBuilder builder;
    builder.init();

    AssetWriter writer;
    builder.write(writer);
    usz size = 0;
    u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
    writer.free();
    REQUIRE(bytes != nullptr);
    REQUIRE(ASSET_FILE::write_file(path.c_str(), bytes, size));
    MEMORY::heap_allocator()->free(bytes);

    // The scan: just the prelude.
    usz prelude_size = 0;
    u8* prelude = ASSET_FILE::read_prelude(path.c_str(), MEMORY::heap_allocator(), &prelude_size);
    REQUIRE(prelude != nullptr);
    AssetView file;
    REQUIRE(file.parse(prelude, prelude_size) == ASSET_PARSE_OK);

    // The load.
    AssetReader reader;
    REQUIRE(reader.open(path.c_str()));
    const ChunkEntry* desc_chunk = nullptr;
    u8* payload = reader.read_chunk(file, CHUNK_TAG::MESH, MEMORY::heap_allocator(), &desc_chunk);
    REQUIRE(payload != nullptr);
    MeshAssetView mesh;
    REQUIRE(mesh.parse(file, payload, desc_chunk->size) == MESH_PARSE_OK);

    // Bounds alone: one small read, what a culling pass or the editor wants.
    MeshBounds bounds;
    REQUIRE(reader.read_chunk(*mesh.bounds_chunk, &bounds));
    CHECK(bounds.radius == 0.75f);

    // The position stream alone (a depth-only pass), then the indices.
    const VertexAttributeDesc* position = mesh.find_attribute(VERTEX_SEMANTIC_POSITION, 0);
    REQUIRE(position != nullptr);
    u8* positions = reader.read_chunk(*mesh.vertex_chunks[position->stream], MEMORY::heap_allocator());
    REQUIRE(positions != nullptr);
    CHECK(std::memcmp(positions, builder.stream_bytes[0].data, mesh.stream_size(0)) == 0);
    u8* indices = reader.read_chunk(*mesh.index_chunk, MEMORY::heap_allocator());
    REQUIRE(indices != nullptr);
    CHECK(std::memcmp(indices, builder.indices.data, mesh.index_bytes()) == 0);
    reader.close();

    MEMORY::heap_allocator()->free(indices);
    MEMORY::heap_allocator()->free(positions);
    MEMORY::heap_allocator()->free(payload);
    MEMORY::heap_allocator()->free(prelude);
    std::filesystem::remove(path);
    builder.free();
}

TEST_CASE("asset/mesh_asset: 32-bit indices are sized accordingly") {
    MeshBuilder builder;
    builder.init();
    builder.desc.index_format = MESH_INDEX_U32;
    builder.indices.clear();
    const u32 index_data[6] = {0, 1, 2, 0, 2, 3};
    const u8* index_bytes = reinterpret_cast<const u8*>(index_data);
    for (usz i = 0; i < sizeof(index_data); ++i) {
        builder.indices.push(index_bytes[i]);
    }

    AssetView file;
    MeshAssetView mesh;
    u8* bytes = nullptr;
    u8* payload = nullptr;
    CHECK(round_trip(builder, file, mesh, &bytes, &payload) == MESH_PARSE_OK);
    CHECK(mesh.index_bytes() == 24);
    CHECK(mesh.index_chunk->size == 24);

    free_round_trip(bytes, payload);
    builder.free();
}

TEST_CASE("asset/mesh_asset: parse rejects missing chunks, unknown versions and a payload of the wrong size") {
    MeshBuilder builder;
    builder.init();
    usz payload_size = 0;
    u8* payload = builder.build_payload(&payload_size);

    SUBCASE("placeholder chunks from before the layout existed (version 0, size 0)") {
        AssetWriter writer;
        writer.type = ASSET_TYPE::MESH;
        writer.guid = MESH_GUID;
        writer.add_chunk(CHUNK_TAG::MESH, 0, 0, nullptr, 0);
        writer.add_chunk(CHUNK_TAG::VERTICES, 0, 0, nullptr, 0);
        writer.add_chunk(CHUNK_TAG::INDICES, 0, 0, nullptr, 0);
        writer.add_chunk(CHUNK_TAG::BOUNDS, 0, 0, nullptr, 0);
        usz size = 0;
        u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
        writer.free();
        AssetView file;
        REQUIRE(file.parse(bytes, size) == ASSET_PARSE_OK);
        MeshAssetView mesh;
        CHECK(mesh.parse(file, nullptr, 0) == MESH_PARSE_UNSUPPORTED_VERSION);
        MEMORY::heap_allocator()->free(bytes);
    }

    SUBCASE("a future version") {
        AssetWriter writer;
        builder.write(writer, MESH_ASSET::VERSION + 1);
        usz size = 0;
        u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
        writer.free();
        AssetView file;
        REQUIRE(file.parse(bytes, size) == ASSET_PARSE_OK);
        MeshAssetView mesh;
        CHECK(mesh.parse(file, payload, payload_size) == MESH_PARSE_UNSUPPORTED_VERSION);
        MEMORY::heap_allocator()->free(bytes);
    }

    SUBCASE("a payload that is not the MESH chunk's size, or missing") {
        AssetWriter writer;
        builder.write(writer);
        usz size = 0;
        u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
        writer.free();
        AssetView file;
        REQUIRE(file.parse(bytes, size) == ASSET_PARSE_OK);
        MeshAssetView mesh;
        CHECK(mesh.parse(file, payload, payload_size + 8) == MESH_PARSE_BAD_DESC);
        CHECK(mesh.parse(file, nullptr, payload_size) == MESH_PARSE_BAD_DESC);
        CHECK(mesh.parse(file, payload, payload_size) == MESH_PARSE_OK);
        MEMORY::heap_allocator()->free(bytes);
    }

    SUBCASE("no BBOX chunk, fewer VERT chunks than streams, or more") {
        AssetWriter writer;
        builder.write(writer);
        usz size = 0;
        u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
        writer.free();
        AssetView full;
        REQUIRE(full.parse(bytes, size) == ASSET_PARSE_OK);
        MeshAssetView mesh;

        AssetWriter without_bounds;
        without_bounds.type = ASSET_TYPE::MESH;
        without_bounds.guid = MESH_GUID;
        without_bounds.add_dependency(MATERIAL_GUID);
        for (usz i = 0; i < full.chunk_count(); ++i) {
            const ChunkEntry& chunk = full.chunks[i];
            if (chunk.tag != CHUNK_TAG::BOUNDS) {
                without_bounds.add_chunk(chunk.tag, chunk.version, chunk.flags, bytes + chunk.offset, chunk.size);
            }
        }
        usz without_size = 0;
        u8* without_bytes = without_bounds.write(MEMORY::heap_allocator(), &without_size);
        without_bounds.free();
        AssetView without_view;
        REQUIRE(without_view.parse(without_bytes, without_size) == ASSET_PARSE_OK);
        CHECK(mesh.parse(without_view, payload, payload_size) == MESH_PARSE_MISSING_CHUNK);
        MEMORY::heap_allocator()->free(without_bytes);

        AssetWriter fewer;
        copy_chunks(full, bytes, fewer, 1, 0);
        usz fewer_size = 0;
        u8* fewer_bytes = fewer.write(MEMORY::heap_allocator(), &fewer_size);
        fewer.free();
        AssetView fewer_view;
        REQUIRE(fewer_view.parse(fewer_bytes, fewer_size) == ASSET_PARSE_OK);
        CHECK(mesh.parse(fewer_view, payload, payload_size) == MESH_PARSE_MISSING_CHUNK);
        MEMORY::heap_allocator()->free(fewer_bytes);

        AssetWriter more;
        copy_chunks(full, bytes, more, 2, 1);
        usz more_size = 0;
        u8* more_bytes = more.write(MEMORY::heap_allocator(), &more_size);
        more.free();
        AssetView more_view;
        REQUIRE(more_view.parse(more_bytes, more_size) == ASSET_PARSE_OK);
        CHECK(mesh.parse(more_view, payload, payload_size) == MESH_PARSE_BAD_DESC);
        MEMORY::heap_allocator()->free(more_bytes);

        MEMORY::heap_allocator()->free(bytes);
    }

    MEMORY::heap_allocator()->free(payload);
    builder.free();
}

TEST_CASE("asset/mesh_asset: parse rejects inconsistent descs") {
    MeshBuilder builder;
    builder.init();

    SUBCASE("zero vertices") { builder.desc.vertex_count = 0; }
    SUBCASE("index count not a multiple of three") { builder.desc.index_count = 5; }
    SUBCASE("unknown index format") { builder.desc.index_format = 9; }
    SUBCASE("unknown topology") { builder.desc.topology = 1; }
    SUBCASE("zero streams") { builder.desc.stream_count = 0; }
    SUBCASE("too many streams") { builder.desc.stream_count = MESH_ASSET::MAX_STREAMS + 1; }
    SUBCASE("zero attributes") { builder.desc.attribute_count = 0; }
    SUBCASE("zero submeshes") { builder.desc.submesh_count = 0; }
    SUBCASE("flags must be zero") { builder.desc.flags = 1; }
    SUBCASE("reserved must be zero") { builder.desc.reserved[7] = 1; }
    SUBCASE("table counts disagree with the payload size") { builder.desc.submesh_count = 3; }

    AssetView file;
    MeshAssetView mesh;
    u8* bytes = nullptr;
    u8* payload = nullptr;
    CHECK(round_trip(builder, file, mesh, &bytes, &payload) == MESH_PARSE_BAD_DESC);
    CHECK_FALSE(mesh.is_parsed());

    free_round_trip(bytes, payload);
    builder.free();
}

TEST_CASE("asset/mesh_asset: parse rejects inconsistent streams, attributes, indices, submeshes and bounds") {
    MeshBuilder builder;
    builder.init();
    MeshParseError expected = MESH_PARSE_OK;

    SUBCASE("stride of zero") {
        builder.streams[0].stride = 0;
        expected = MESH_PARSE_BAD_STREAM;
    }
    SUBCASE("stride not a multiple of four") {
        builder.streams[0].stride = 14;
        expected = MESH_PARSE_BAD_STREAM;
    }
    SUBCASE("stream reserved must be zero") {
        builder.streams[1].reserved = 1;
        expected = MESH_PARSE_BAD_STREAM;
    }
    SUBCASE("VERT chunk size does not match stride * vertex_count") {
        builder.stream_bytes[1].push(0);
        expected = MESH_PARSE_BAD_STREAM;
    }
    SUBCASE("unknown attribute format") {
        builder.attributes[0].format = 0xee;
        expected = MESH_PARSE_BAD_ATTRIBUTE;
    }
    SUBCASE("unknown semantic") {
        builder.attributes[0].semantic = 42;
        expected = MESH_PARSE_BAD_ATTRIBUTE;
    }
    SUBCASE("attribute stream out of range") {
        builder.attributes[0].stream = 2;
        expected = MESH_PARSE_BAD_ATTRIBUTE;
    }
    SUBCASE("attribute past the end of the vertex") {
        builder.attributes[2].offset = 12;
        expected = MESH_PARSE_BAD_ATTRIBUTE;
    }
    SUBCASE("duplicate semantic") {
        builder.attributes[2].semantic = VERTEX_SEMANTIC_NORMAL;
        expected = MESH_PARSE_BAD_ATTRIBUTE;
    }
    SUBCASE("INDX chunk size does not match") {
        builder.indices.push(0);
        expected = MESH_PARSE_BAD_INDICES;
    }
    SUBCASE("submesh past the index count") {
        builder.submeshes[1].index_count = 6;
        expected = MESH_PARSE_BAD_SUBMESH;
    }
    SUBCASE("submesh with no triangles") {
        builder.submeshes[1].index_count = 0;
        expected = MESH_PARSE_BAD_SUBMESH;
    }
    SUBCASE("submesh with a partial triangle") {
        builder.submeshes[0].index_count = 2;
        expected = MESH_PARSE_BAD_SUBMESH;
    }
    SUBCASE("base vertex out of range") {
        builder.submeshes[0].base_vertex = 4;
        expected = MESH_PARSE_BAD_SUBMESH;
    }
    SUBCASE("material not in the dependency table") {
        builder.submeshes[0].material = 1;
        expected = MESH_PARSE_BAD_SUBMESH;
    }
    SUBCASE("material index with no dependencies at all") {
        builder.with_material = false;
        expected = MESH_PARSE_BAD_SUBMESH;
    }
    SUBCASE("submesh reserved must be zero") {
        builder.submeshes[0].reserved[1] = 1;
        expected = MESH_PARSE_BAD_SUBMESH;
    }

    AssetView file;
    MeshAssetView mesh;
    u8* bytes = nullptr;
    u8* payload = nullptr;
    CHECK(round_trip(builder, file, mesh, &bytes, &payload) == expected);
    CHECK(mesh.is_parsed() == (expected == MESH_PARSE_OK));

    free_round_trip(bytes, payload);
    builder.free();
}

TEST_CASE("asset/mesh_asset: parse rejects a BBOX chunk of the wrong size") {
    MeshBuilder builder;
    builder.init();
    usz payload_size = 0;
    u8* payload = builder.build_payload(&payload_size);

    AssetWriter writer;
    writer.type = ASSET_TYPE::MESH;
    writer.guid = MESH_GUID;
    writer.add_dependency(MATERIAL_GUID);
    writer.add_chunk(CHUNK_TAG::MESH, MESH_ASSET::VERSION, 0, payload, payload_size);
    writer.add_chunk(CHUNK_TAG::VERTICES, MESH_ASSET::VERSION, 0, builder.stream_bytes[0].data, builder.stream_bytes[0].count);
    writer.add_chunk(CHUNK_TAG::VERTICES, MESH_ASSET::VERSION, 0, builder.stream_bytes[1].data, builder.stream_bytes[1].count);
    writer.add_chunk(CHUNK_TAG::INDICES, MESH_ASSET::VERSION, 0, builder.indices.data, builder.indices.count);
    writer.add_chunk(CHUNK_TAG::BOUNDS, MESH_ASSET::VERSION, 0, &builder.bounds, sizeof(MeshBounds) - 8);

    usz size = 0;
    u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
    writer.free();
    AssetView file;
    REQUIRE(file.parse(bytes, size) == ASSET_PARSE_OK);
    MeshAssetView mesh;
    CHECK(mesh.parse(file, payload, payload_size) == MESH_PARSE_BAD_BOUNDS);
    CHECK_FALSE(mesh.is_parsed());

    MEMORY::heap_allocator()->free(bytes);
    MEMORY::heap_allocator()->free(payload);
    builder.free();
}
