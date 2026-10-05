#include "support/test_support.hpp"

#include "engine/asset/asset_view.hpp"
#include "engine/asset/asset_writer.hpp"
#include "engine/asset/asset_types/mesh_asset.hpp"
#include "engine/memory/heap_allocator.hpp"

#include <cstring>

namespace {

const AssetGuid MESH_GUID = {0xabcd, 0xef01};
const AssetGuid MATERIAL_GUID = {0x1, 0x2};

// A unit quad in the XY plane at z = 0 plus one far vertex, as an importer
// would hand it over: positions in one stream, normals and texcoords
// interleaved in a second, two triangles, two submeshes.
struct QuadSource {
    f32 positions[5 * 3] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 3, 0, 4};
    u8 shading[5 * 16] = {};
    u32 indices[6] = {0, 1, 2, 0, 2, 3};
    MeshSourceSubmesh submeshes[2];
    MeshSource source;

    void init() {
        for (u32 i = 0; i < sizeof(this->shading); ++i) {
            this->shading[i] = static_cast<u8>(i);
        }
        this->submeshes[0].first_index = 0;
        this->submeshes[0].index_count = 3;
        this->submeshes[0].material = 0;
        this->submeshes[1].first_index = 3;
        this->submeshes[1].index_count = 3;

        this->source.vertex_count = 5;
        this->source.stream_count = 2;
        this->source.streams[0].stride = 12;
        this->source.streams[0].vertices = this->positions;
        this->source.streams[1].stride = 16;
        this->source.streams[1].vertices = this->shading;
        this->source.attribute_count = 3;
        this->source.attributes[0].semantic = VERTEX_SEMANTIC_POSITION;
        this->source.attributes[0].format = VERTEX_FORMAT_F32x3;
        this->source.attributes[0].stream = 0;
        this->source.attributes[1].semantic = VERTEX_SEMANTIC_NORMAL;
        this->source.attributes[1].format = VERTEX_FORMAT_SNORM16x4;
        this->source.attributes[1].stream = 1;
        this->source.attributes[2].semantic = VERTEX_SEMANTIC_TEXCOORD;
        this->source.attributes[2].format = VERTEX_FORMAT_F32x2;
        this->source.attributes[2].stream = 1;
        this->source.attributes[2].offset = 8;
        this->source.indices = this->indices;
        this->source.index_count = 6;
        this->source.submeshes = this->submeshes;
        this->source.submesh_count = 2;
    }
};

// Puts the writer's chunks in a file in memory (with one material
// dependency) and parses it back the way a loader does; the MESH payload is
// handed to the view straight from the file bytes. The file buffer is left
// for the caller to free.
MeshParseError round_trip(const MeshAssetWriter& mesh, AssetView& file, MeshAssetView& view, u8** bytes) {
    AssetWriter writer;
    writer.type = ASSET_TYPE::MESH;
    writer.guid = MESH_GUID;
    writer.add_dependency(MATERIAL_GUID);
    mesh.add_chunks(writer);
    usz size = 0;
    *bytes = writer.write(MEMORY::heap_allocator(), &size);
    writer.free();
    REQUIRE(*bytes != nullptr);
    REQUIRE((file = AssetView::parse(*bytes, size)).is_ok());
    const ChunkEntry* desc_chunk = file.find_chunk(CHUNK_TYPE::MESH);
    REQUIRE(desc_chunk != nullptr);
    return view.parse(file, *bytes + desc_chunk->offset, static_cast<usz>(desc_chunk->size));
}

} // namespace

TEST_CASE("asset/mesh_asset_writer: a writer starts empty and adds nothing until built") {
    MeshAssetWriter mesh;
    CHECK_FALSE(mesh.is_built());
    CHECK(mesh.stream_data(0) == nullptr);
    CHECK(mesh.stream_size(0) == 0);

    AssetWriter writer;
    mesh.add_chunks(writer);
    CHECK(writer.chunk_count() == 0);
    writer.free();
    mesh.free();
}

TEST_CASE("asset/mesh_asset_writer: a two stream quad builds its tables, bounds and 16-bit indices") {
    QuadSource quad;
    quad.init();
    MeshAssetWriter mesh;
    REQUIRE(mesh.build(quad.source, MeshImportOptions{}) == MESH_WRITE_OK);
    CHECK(mesh.is_built());

    CHECK(mesh.desc.vertex_count == 5);
    CHECK(mesh.desc.index_count == 6);
    CHECK(mesh.desc.index_format == MESH_INDEX_U16);
    CHECK(mesh.desc.topology == MESH_TOPOLOGY_TRIANGLE_LIST);
    CHECK(mesh.desc.stream_count == 2);
    CHECK(mesh.desc.attribute_count == 3);
    CHECK(mesh.desc.submesh_count == 2);
    CHECK(mesh.desc_size() == MESH_ASSET::desc_size(2, 3, 2));

    // Streams are copied as given, back to back.
    REQUIRE(mesh.streams.count == 2);
    CHECK(mesh.streams[0].stride == 12);
    CHECK(mesh.streams[1].stride == 16);
    CHECK(mesh.stream_size(0) == 60);
    CHECK(mesh.stream_size(1) == 80);
    CHECK(mesh.vertices.count == 140);
    CHECK(mesh.stream_data(0) == mesh.vertices.data);
    CHECK(mesh.stream_data(1) == mesh.vertices.data + 60);
    CHECK(std::memcmp(mesh.stream_data(0), quad.positions, sizeof(quad.positions)) == 0);
    CHECK(std::memcmp(mesh.stream_data(1), quad.shading, sizeof(quad.shading)) == 0);
    REQUIRE(mesh.attributes.count == 3);
    CHECK(mesh.attributes[2].semantic == VERTEX_SEMANTIC_TEXCOORD);
    CHECK(mesh.attributes[2].offset == 8);

    // Indices narrowed to 16 bits.
    REQUIRE(mesh.indices.count == 12);
    u16 indices[6];
    std::memcpy(indices, mesh.indices.data, sizeof(indices));
    CHECK(indices[0] == 0);
    CHECK(indices[2] == 2);
    CHECK(indices[5] == 3);

    // Whole mesh bounds include the far vertex (3, 0, 4) that no index
    // references: box (0,0,0)-(3,1,4), center (1.5, 0.5, 2), radius to a corner.
    CHECK(mesh.bounds.min[0] == 0);
    CHECK(mesh.bounds.max[0] == 3);
    CHECK(mesh.bounds.max[1] == 1);
    CHECK(mesh.bounds.max[2] == 4);
    CHECK(mesh.bounds.center[0] == doctest::Approx(1.5f));
    CHECK(mesh.bounds.center[1] == doctest::Approx(0.5f));
    CHECK(mesh.bounds.center[2] == doctest::Approx(2.0f));
    CHECK(mesh.bounds.radius == doctest::Approx(2.5495f).epsilon(0.001));

    // Submesh bounds cover only the vertices each one indexes.
    REQUIRE(mesh.submeshes.count == 2);
    CHECK(mesh.submeshes[0].first_index == 0);
    CHECK(mesh.submeshes[0].index_count == 3);
    CHECK(mesh.submeshes[0].base_vertex == 0);
    CHECK(mesh.submeshes[0].material == 0);
    CHECK(mesh.submeshes[0].bounds_min[0] == 0);
    CHECK(mesh.submeshes[0].bounds_max[0] == 1);
    CHECK(mesh.submeshes[0].bounds_max[1] == 1);
    CHECK(mesh.submeshes[0].bounds_max[2] == 0);
    CHECK(mesh.submeshes[1].first_index == 3);
    CHECK(mesh.submeshes[1].material == MESH_ASSET::NO_MATERIAL);
    CHECK(mesh.submeshes[1].bounds_min[0] == 0);
    CHECK(mesh.submeshes[1].bounds_max[0] == 1);

    AssetView file;
    MeshAssetView view;
    u8* bytes = nullptr;
    REQUIRE(round_trip(mesh, file, view, &bytes) == MESH_PARSE_OK);
    CHECK(view.desc->index_format == MESH_INDEX_U16);
    CHECK(view.find_attribute(VERTEX_SEMANTIC_TEXCOORD, 0)->stream == 1);
    CHECK(view.stream_size(1) == 80);
    CHECK(view.index_bytes() == 12);
    CHECK(view.vertex_chunks[1]->size == 80);
    CHECK(std::memcmp(bytes + view.vertex_chunks[0]->offset, quad.positions, sizeof(quad.positions)) == 0);
    MeshBounds bounds;
    std::memcpy(&bounds, bytes + view.bounds_chunk->offset, sizeof(bounds));
    CHECK(bounds.max[2] == 4);
    MEMORY::heap_allocator()->free(bytes);
    mesh.free();
}

TEST_CASE("asset/mesh_asset_writer: index format follows the vertex count and the option") {
    QuadSource quad;
    quad.init();
    MeshAssetWriter mesh;

    SUBCASE("compact_indices off keeps 32 bits") {
        MeshImportOptions options;
        options.compact_indices = false;
        REQUIRE(mesh.build(quad.source, options) == MESH_WRITE_OK);
        CHECK(mesh.desc.index_format == MESH_INDEX_U32);
        REQUIRE(mesh.indices.count == 24);
        CHECK(std::memcmp(mesh.indices.data, quad.indices, sizeof(quad.indices)) == 0);

        AssetView file;
        MeshAssetView view;
        u8* bytes = nullptr;
        CHECK(round_trip(mesh, file, view, &bytes) == MESH_PARSE_OK);
        CHECK(view.index_bytes() == 24);
        MEMORY::heap_allocator()->free(bytes);
    }
    SUBCASE("more than 65536 vertices needs 32 bits") {
        // Only the position stream, so the vertex buffer stays small enough
        // to build: 65537 vertices of one F32x3 each.
        DynamicArray<f32> positions;
        positions.resize(static_cast<usz>(65537) * 3);
        positions[65536 * 3] = 9;
        MeshSource source = quad.source;
        source.vertex_count = 65537;
        source.stream_count = 1;
        source.streams[0].vertices = positions.data;
        source.attribute_count = 1;
        const u32 indices[3] = {0, 1, 65536};
        source.indices = indices;
        source.index_count = 3;
        source.submesh_count = 0;
        REQUIRE(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_OK);
        CHECK(mesh.desc.index_format == MESH_INDEX_U32);
        CHECK(mesh.bounds.max[0] == 9);
        positions.free();
    }
    SUBCASE("exactly 65536 vertices still fit 16 bits") {
        DynamicArray<f32> positions;
        positions.resize(static_cast<usz>(65536) * 3);
        MeshSource source = quad.source;
        source.vertex_count = 65536;
        source.stream_count = 1;
        source.streams[0].vertices = positions.data;
        source.attribute_count = 1;
        const u32 indices[3] = {0, 1, 65535};
        source.indices = indices;
        source.index_count = 3;
        source.submesh_count = 0;
        REQUIRE(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_OK);
        CHECK(mesh.desc.index_format == MESH_INDEX_U16);
        u16 last;
        std::memcpy(&last, mesh.indices.data + 4, sizeof(last));
        CHECK(last == 65535);
        positions.free();
    }
    mesh.free();
}

TEST_CASE("asset/mesh_asset_writer: no submeshes means one over everything with no material") {
    QuadSource quad;
    quad.init();
    quad.source.submesh_count = 0;
    quad.source.submeshes = nullptr;
    MeshAssetWriter mesh;
    REQUIRE(mesh.build(quad.source, MeshImportOptions{}) == MESH_WRITE_OK);
    REQUIRE(mesh.desc.submesh_count == 1);
    CHECK(mesh.submeshes[0].first_index == 0);
    CHECK(mesh.submeshes[0].index_count == 6);
    CHECK(mesh.submeshes[0].material == MESH_ASSET::NO_MATERIAL);
    // Over the indexed vertices only: the far vertex is left out.
    CHECK(mesh.submeshes[0].bounds_max[0] == 1);
    CHECK(mesh.submeshes[0].bounds_max[2] == 0);

    AssetView file;
    MeshAssetView view;
    u8* bytes = nullptr;
    CHECK(round_trip(mesh, file, view, &bytes) == MESH_PARSE_OK);
    MEMORY::heap_allocator()->free(bytes);
    mesh.free();
}

TEST_CASE("asset/mesh_asset_writer: positions may be F32x4 and bounds read only xyz") {
    const f32 positions[2 * 4] = {-1, -2, -3, 100, 1, 2, 3, 100};
    const u32 indices[3] = {0, 1, 0};
    MeshSource source;
    source.vertex_count = 2;
    source.stream_count = 1;
    source.streams[0].stride = 16;
    source.streams[0].vertices = positions;
    source.attribute_count = 1;
    source.attributes[0].semantic = VERTEX_SEMANTIC_POSITION;
    source.attributes[0].format = VERTEX_FORMAT_F32x4;
    source.indices = indices;
    source.index_count = 3;

    MeshAssetWriter mesh;
    REQUIRE(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_OK);
    CHECK(mesh.bounds.min[2] == -3);
    CHECK(mesh.bounds.max[2] == 3);
    CHECK(mesh.bounds.center[0] == 0);
    CHECK(mesh.bounds.radius == doctest::Approx(3.7417f).epsilon(0.001));
    mesh.free();
}

TEST_CASE("asset/mesh_asset_writer: build rejects a bad source and leaves nothing behind") {
    QuadSource quad;
    quad.init();
    MeshAssetWriter mesh;
    REQUIRE(mesh.build(quad.source, MeshImportOptions{}) == MESH_WRITE_OK);

    SUBCASE("counts") {
        MeshSource source = quad.source;
        source.vertex_count = 0;
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_BAD_SOURCE);
        source = quad.source;
        source.stream_count = MESH_ASSET::MAX_STREAMS + 1;
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_BAD_SOURCE);
        source = quad.source;
        source.attribute_count = 0;
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_BAD_SOURCE);
        source = quad.source;
        source.index_count = 4;
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_BAD_SOURCE);
        source = quad.source;
        source.indices = nullptr;
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_BAD_SOURCE);
        source = quad.source;
        source.submeshes = nullptr;
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_BAD_SOURCE);
    }
    SUBCASE("streams") {
        MeshSource source = quad.source;
        source.streams[1].stride = 14;
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_BAD_STREAM);
        source.streams[1].stride = 0;
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_BAD_STREAM);
        source = quad.source;
        source.streams[0].vertices = nullptr;
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_BAD_STREAM);
    }
    SUBCASE("attributes") {
        MeshSource source = quad.source;
        source.attributes[1].format = 0xff;
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_BAD_ATTRIBUTE);
        source = quad.source;
        source.attributes[1].semantic = VERTEX_SEMANTIC_WEIGHTS + 1;
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_BAD_ATTRIBUTE);
        source = quad.source;
        source.attributes[1].stream = 2;
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_BAD_ATTRIBUTE);
        source = quad.source;
        source.attributes[2].offset = 12; // F32x2 at 12 overruns a 16 byte stride
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_BAD_ATTRIBUTE);
        source = quad.source;
        source.attributes[2].semantic = VERTEX_SEMANTIC_NORMAL; // duplicate NORMAL 0
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_BAD_ATTRIBUTE);
    }
    SUBCASE("position") {
        MeshSource source = quad.source;
        source.attributes[0].semantic_index = 1;
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_NO_POSITION);
        source = quad.source;
        source.attributes[0].format = VERTEX_FORMAT_F16x4;
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_NO_POSITION);
    }
    SUBCASE("indices") {
        MeshSource source = quad.source;
        const u32 indices[6] = {0, 1, 5, 0, 2, 3};
        source.indices = indices;
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_BAD_INDEX);
    }
    SUBCASE("submeshes") {
        MeshSource source = quad.source;
        MeshSourceSubmesh submeshes[2] = {quad.submeshes[0], quad.submeshes[1]};
        source.submeshes = submeshes;
        submeshes[1].index_count = 6; // 3 + 6 > 6
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_BAD_SUBMESH);
        submeshes[1].index_count = 2;
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_BAD_SUBMESH);
        submeshes[1].index_count = 0;
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_BAD_SUBMESH);
        submeshes[1] = quad.submeshes[1];
        submeshes[1].first_index = 7;
        CHECK(mesh.build(source, MeshImportOptions{}) == MESH_WRITE_BAD_SUBMESH);
    }

    // A failed build clears the previous result.
    CHECK_FALSE(mesh.is_built());
    CHECK(mesh.streams.count == 0);
    CHECK(mesh.vertices.count == 0);
    CHECK(mesh.indices.count == 0);
    AssetWriter writer;
    mesh.add_chunks(writer);
    CHECK(writer.chunk_count() == 0);
    writer.free();

    CHECK(doctest::String(MESH_ASSET::write_error_name(MESH_WRITE_NO_POSITION)) != "unknown error");
    mesh.free();
}
