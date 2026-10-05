#pragma once

#include "engine/asset/asset_reader.hpp"
#include "engine/asset/asset_view.hpp"
#include "engine/asset/asset_writer.hpp"
#include "engine/defines.hpp"

// Payload layout of a mesh asset (ASSET_TYPE::MESH). The `MESH` chunk holds
// the counts, the vertex streams and attributes, and the submesh table; each
// vertex stream is its own `VERT` chunk (in stream order), the indices are the
// `INDX` chunk and the bounds the `BBOX` chunk. docs/asset_format.md "Mesh
// payloads" is the specification.
//
//     AssetView view;  ... parsed from the file's prelude ...
//     AssetReader reader;  ... open on the same file ...
//     ReadChunk desc = reader.read_chunk(view, CHUNK_TYPE::MESH, allocator);
//     MeshAssetView mesh;
//     if (mesh.parse(view, desc) == MESH_PARSE_OK) {
//         const VertexAttributeDesc* position = mesh.find_attribute(VERTEX_SEMANTIC_POSITION, 0);
//         void* stream = reader.read_chunk(*mesh.vertex_chunks[position->stream], allocator);
//         MeshBounds bounds;
//         reader.read_chunk(*mesh.bounds_chunk, &bounds);
//     }
//
// The MESH payload is small and validated up front; the geometry chunks are
// read afterwards, by the caller, straight into upload memory. Vertex data
// is laid out for direct upload: the streams map to vertex input bindings
// and the attributes to vertex input attributes, with no shuffling. Formats
// are engine enums; the graphics module maps VertexFormat to VkFormat.

namespace MESH_ASSET {

// Payload version of the MESH, VERT, INDX and BBOX chunks written by this code.
constexpr u32 VERSION = 1;
// Vulkan guarantees at least 16 vertex input bindings and attributes.
constexpr u32 MAX_STREAMS = 16;
constexpr u32 MAX_ATTRIBUTES = 16;
// `SubmeshDesc::material` for a submesh with no material.
constexpr u32 NO_MATERIAL = 0xffffffffu;

} // namespace MESH_ASSET

// Element formats of a vertex attribute. Values are stored on disk: never
// renumber, only append.
enum VertexFormat : u32 {
    VERTEX_FORMAT_NONE = 0,

    VERTEX_FORMAT_F32 = 1,
    VERTEX_FORMAT_F32x2 = 2,
    VERTEX_FORMAT_F32x3 = 3,
    VERTEX_FORMAT_F32x4 = 4,

    VERTEX_FORMAT_F16x2 = 10,
    VERTEX_FORMAT_F16x4 = 11,

    VERTEX_FORMAT_UNORM8x4 = 20, // colors, normalized to [0, 1]
    VERTEX_FORMAT_SNORM8x4 = 21, // normals and tangents, normalized to [-1, 1]
    VERTEX_FORMAT_UNORM16x2 = 22,
    VERTEX_FORMAT_UNORM16x4 = 23,
    VERTEX_FORMAT_SNORM16x2 = 24,
    VERTEX_FORMAT_SNORM16x4 = 25,

    VERTEX_FORMAT_UINT8x4 = 30, // joint indices
    VERTEX_FORMAT_UINT16x4 = 31,
    VERTEX_FORMAT_UINT32 = 32,
};

// What a vertex attribute means to a shader. `semantic_index` separates
// several of the same kind (TEXCOORD 0 and 1).
enum VertexSemantic : u32 {
    VERTEX_SEMANTIC_POSITION = 0,
    VERTEX_SEMANTIC_NORMAL = 1,
    VERTEX_SEMANTIC_TANGENT = 2, // xyz tangent, w handedness
    VERTEX_SEMANTIC_COLOR = 3,
    VERTEX_SEMANTIC_TEXCOORD = 4,
    VERTEX_SEMANTIC_JOINTS = 5,
    VERTEX_SEMANTIC_WEIGHTS = 6,
};

enum MeshIndexFormat : u32 {
    MESH_INDEX_U16 = 0,
    MESH_INDEX_U32 = 1,
};

enum MeshTopology : u32 {
    MESH_TOPOLOGY_TRIANGLE_LIST = 0, // the only one in version 1
};

namespace VERTEX_FORMAT {

// Bytes of one element, 0 for a value that is not a VertexFormat.
u32 size(u32 format);
const char* name(u32 format);

} // namespace VERTEX_FORMAT

namespace MESH_ASSET {

// Bytes of one index, 0 for a value that is not a MeshIndexFormat.
constexpr u32 index_size(const u32 format) {
    return format == MESH_INDEX_U16 ? 2 : format == MESH_INDEX_U32 ? 4 : 0;
}

} // namespace MESH_ASSET

// --- On-disk structures ------------------------------------------------------
// Written and read verbatim. Changing a layout means bumping
// MESH_ASSET::VERSION and keeping a reader for the old one.

// MESH payload: this struct, then `stream_count` VertexStreamDesc,
// `attribute_count` VertexAttributeDesc and `submesh_count` SubmeshDesc, back
// to back in that order.
struct MeshDesc {
    u32 vertex_count = 0;
    u32 index_count = 0;
    u32 index_format = MESH_INDEX_U16;          // MeshIndexFormat
    u32 topology = MESH_TOPOLOGY_TRIANGLE_LIST; // MeshTopology
    u32 stream_count = 0;                       // VERT chunks, at most MESH_ASSET::MAX_STREAMS
    u32 attribute_count = 0;                    // at most MESH_ASSET::MAX_ATTRIBUTES
    u32 submesh_count = 0;
    u32 flags = 0;                              // no bits defined in version 1, zero
    u32 reserved[8] = {0, 0, 0, 0, 0, 0, 0, 0};
};

// One vertex stream: the i-th entry describes the i-th VERT chunk, which
// holds `vertex_count` vertices of `stride` bytes each.
struct VertexStreamDesc {
    u32 stride = 0; // bytes per vertex, nonzero, multiple of 4
    u32 reserved = 0;
};

struct VertexAttributeDesc {
    u8 semantic = 0;       // VertexSemantic
    u8 semantic_index = 0; // TEXCOORD1 is semantic TEXCOORD, index 1
    u8 format = 0;         // VertexFormat
    u8 stream = 0;         // index into the stream table
    u32 offset = 0;        // bytes from the start of the vertex inside its stream
};

// A range of the index buffer drawn with one material. `material` indexes the
// file's dependency table (a material asset) or is MESH_ASSET::NO_MATERIAL.
struct SubmeshDesc {
    u32 first_index = 0;
    u32 index_count = 0; // multiple of 3
    u32 base_vertex = 0; // added to every index
    u32 material = MESH_ASSET::NO_MATERIAL;
    f32 bounds_min[3] = {0, 0, 0}; // object space, over this submesh's vertices
    f32 bounds_max[3] = {0, 0, 0};
    u32 reserved[2] = {0, 0};
};

// BBOX payload: object-space bounds of the whole mesh.
struct MeshBounds {
    f32 min[3] = {0, 0, 0};
    f32 max[3] = {0, 0, 0};
    f32 center[3] = {0, 0, 0}; // of the bounding sphere
    f32 radius = 0;
    u32 reserved[2] = {0, 0};
};

static_assert(sizeof(MeshDesc) == 64, "MeshDesc must be 64 bytes on disk");
static_assert(sizeof(VertexStreamDesc) == 8, "VertexStreamDesc must be 8 bytes on disk");
static_assert(sizeof(VertexAttributeDesc) == 8, "VertexAttributeDesc must be 8 bytes on disk");
static_assert(sizeof(SubmeshDesc) == 48, "SubmeshDesc must be 48 bytes on disk");
static_assert(sizeof(MeshBounds) == 48, "MeshBounds must be 48 bytes on disk");
static_assert(alignof(MeshDesc) <= 8 && alignof(SubmeshDesc) <= 8 && alignof(MeshBounds) <= 8, "payloads are placed at 64-byte aligned offsets");

namespace MESH_ASSET {

// Size of the MESH payload for these table counts.
constexpr usz desc_size(const u32 stream_count, const u32 attribute_count, const u32 submesh_count) {
    return sizeof(MeshDesc) + sizeof(VertexStreamDesc) * stream_count + sizeof(VertexAttributeDesc) * attribute_count +
           sizeof(SubmeshDesc) * submesh_count;
}

} // namespace MESH_ASSET

// --- Reading ------------------------------------------------------------------

enum MeshParseError {
    MESH_PARSE_OK = 0,
    MESH_PARSE_NOT_A_MESH,           // the file's type is not ASSET_TYPE::MESH
    MESH_PARSE_MISSING_CHUNK,        // no MESH, INDX or BBOX chunk, or fewer VERT chunks than streams
    MESH_PARSE_UNSUPPORTED_VERSION,  // a mesh chunk has a version this build does not read
    MESH_PARSE_BAD_DESC,             // MeshDesc is inconsistent (see docs/asset_format.md)
    MESH_PARSE_BAD_STREAM,           // a stream has a bad stride or its VERT chunk has the wrong size
    MESH_PARSE_BAD_ATTRIBUTE,        // an attribute has an unknown format or does not fit its stream
    MESH_PARSE_BAD_INDICES,          // the INDX chunk size does not match index_count and index_format
    MESH_PARSE_BAD_SUBMESH,          // a submesh range or material is out of bounds
    MESH_PARSE_BAD_BOUNDS,           // the BBOX payload has the wrong size
};

namespace MESH_ASSET {

const char* parse_error_name(MeshParseError error);

}

// Non-owning view over a MESH payload. parse() validates it against the
// file's chunk table (the VERT, INDX and BBOX entries give every size
// without reading a byte of geometry) and points into the payload bytes,
// which must outlive the view. The chunk entries it keeps are what to hand
// an AssetReader to fetch the geometry.
struct MeshAssetView {
    const MeshDesc* desc = nullptr;
    const VertexStreamDesc* streams = nullptr;       // desc->stream_count entries
    const VertexAttributeDesc* attributes = nullptr; // desc->attribute_count entries
    const SubmeshDesc* submeshes = nullptr;          // desc->submesh_count entries

    // Entries in `file`'s chunk table: the i-th stream's VERT chunk
    // (desc->stream_count entries used), the INDX chunk and the BBOX chunk,
    // whose payload is one MeshBounds.
    const ChunkEntry* vertex_chunks[MESH_ASSET::MAX_STREAMS] = {};
    const ChunkEntry* index_chunk = nullptr;
    const ChunkEntry* bounds_chunk = nullptr;

    // `file` is the parsed prelude; `payload` is the MESH chunk's bytes
    // (`size` of them, 8-byte aligned, as read_chunk returns them). On any
    // error the view is reset to empty.
    MeshParseError parse(const AssetView& file, const void* payload, usz size);
    // The same from what read_chunk(file, CHUNK_TYPE::MESH, ...) returned; a
    // chunk that is not ok() parses as MESH_PARSE_BAD_DESC.
    MeshParseError parse(const AssetView& file, const ReadChunk& chunk);
    void reset();

    bool is_parsed() const { return this->desc != nullptr; }

    // The attribute with this semantic and index, or nullptr.
    const VertexAttributeDesc* find_attribute(u32 semantic, u32 semantic_index) const;
    // Bytes of stream `stream`'s VERT payload: stride * vertex_count.
    u64 stream_size(u32 stream) const;
    u64 index_bytes() const;
};

// --- Writing ------------------------------------------------------------------
//
//     MeshSource source;  ... streams, attributes and indices as the importer built them ...
//     source.stream_count = 1;
//     source.streams[0] = {sizeof(Vertex), vertices};
//     source.attribute_count = 2;
//     source.attributes[0] = {VERTEX_SEMANTIC_POSITION, 0, VERTEX_FORMAT_F32x3, 0, 0};
//     source.attributes[1] = {VERTEX_SEMANTIC_TEXCOORD, 0, VERTEX_FORMAT_F32x2, 0, 12};
//     MeshAssetWriter mesh;
//     if (mesh.build(source, MeshImportOptions{}) == MESH_WRITE_OK) {
//         mesh.add_chunks(file); // the AssetWriter; NAME, IMPS, SRC are the importer's
//     }
//     mesh.free();
//
// The writer turns an importer's geometry into the MESH, VERT, INDX and BBOX
// payloads that MeshAssetView::parse reads: it validates the layout, picks
// the index format, computes the bounds and fills the tables. The vertex
// bytes are written as given (the importer decides interleaving and formats,
// the writer only checks them). It produces only those chunks; the container
// (GUID, dependencies, editor chunks) is the caller's.

// What the editor asks of a mesh import. Plain data so it can become the
// IMPS payload once that layout is defined; nothing beyond the essentials yet.
struct MeshImportOptions {
    // Write 16-bit indices when every index fits, 32-bit otherwise. False
    // always writes 32-bit.
    bool compact_indices = true;
};

// One vertex stream of a source: `vertex_count` vertices of `stride` bytes.
struct MeshSourceStream {
    u32 stride = 0;                 // bytes per vertex, nonzero, multiple of 4
    const void* vertices = nullptr; // vertex_count * stride bytes
};

// A range of the index buffer with one material. `material` is an index into
// the dependency table the caller is building (AssetWriter::add_dependency
// returns it) or MESH_ASSET::NO_MATERIAL; the writer cannot check it.
struct MeshSourceSubmesh {
    u32 first_index = 0;
    u32 index_count = 0; // nonzero, multiple of 3
    u32 material = MESH_ASSET::NO_MATERIAL;
};

// A mesh as an importer hands it to the writer. Indices are 32-bit, absolute
// into [0, vertex_count), a triangle list. A POSITION 0 attribute in F32x3 or
// F32x4 is required: the bounds are computed from it. With no submeshes the
// writer emits one covering every index with no material.
struct MeshSource {
    u32 vertex_count = 0;
    u32 stream_count = 0; // 1 to MESH_ASSET::MAX_STREAMS
    MeshSourceStream streams[MESH_ASSET::MAX_STREAMS];
    u32 attribute_count = 0; // 1 to MESH_ASSET::MAX_ATTRIBUTES
    VertexAttributeDesc attributes[MESH_ASSET::MAX_ATTRIBUTES];
    const u32* indices = nullptr;
    u32 index_count = 0; // nonzero, multiple of 3
    const MeshSourceSubmesh* submeshes = nullptr;
    u32 submesh_count = 0;
};

enum MeshWriteError {
    MESH_WRITE_OK = 0,
    MESH_WRITE_BAD_SOURCE,    // a count is zero or over its limit, or index_count is not a multiple of 3
    MESH_WRITE_BAD_STREAM,    // a stream has a null pointer or a stride that is zero or not a multiple of 4
    MESH_WRITE_BAD_ATTRIBUTE, // unknown format or semantic, stream out of range, does not fit, or duplicate
    MESH_WRITE_NO_POSITION,   // no POSITION 0 attribute in F32x3 or F32x4
    MESH_WRITE_BAD_INDEX,     // an index is >= vertex_count
    MESH_WRITE_BAD_SUBMESH,   // a submesh range is empty, not a multiple of 3 or out of the index buffer
};

namespace MESH_ASSET {

const char* write_error_name(MeshWriteError error);

}

// Builds the MESH, VERT, INDX and BBOX payloads for one mesh. build() fills
// the tables and buffers from a source; add_chunks() appends them to an
// AssetWriter at MESH_ASSET::VERSION, streams in order. The writer owns its
// buffers: call free() when done, or build() again to reuse them.
struct MeshAssetWriter {
    MeshAssetWriter();
    explicit MeshAssetWriter(BaseAllocator* allocator);

    MeshAssetWriter(const MeshAssetWriter&) = delete;
    MeshAssetWriter& operator=(const MeshAssetWriter&) = delete;

    // The MESH payload, as tables.
    MeshDesc desc;
    DynamicArray<VertexStreamDesc> streams;       // desc.stream_count entries
    DynamicArray<VertexAttributeDesc> attributes; // desc.attribute_count entries
    DynamicArray<SubmeshDesc> submeshes;          // desc.submesh_count entries
    // Every VERT payload back to back, stream 0 first; see stream_data().
    DynamicArray<u8> vertices;
    // The INDX payload: desc.index_count indices of desc.index_format.
    DynamicArray<u8> indices;
    // The BBOX payload.
    MeshBounds bounds;

    // Validates the source and fills everything above. Per-submesh and whole
    // mesh bounds come from the positions; the bounding sphere is centered on
    // the box and tight around the vertices. On error nothing is kept.
    MeshWriteError build(const MeshSource& source, const MeshImportOptions& options);

    bool is_built() const { return this->desc.vertex_count != 0; }

    // Size of the MESH payload: desc + the three tables.
    usz desc_size() const {
        return MESH_ASSET::desc_size(this->desc.stream_count, this->desc.attribute_count, this->desc.submesh_count);
    }
    // Writes the MESH payload into `out`, which must hold desc_size() bytes.
    void write_desc(void* out) const;

    // Stream `stream`'s VERT payload inside `vertices`, stream_size() bytes.
    const u8* stream_data(u32 stream) const;
    u64 stream_size(u32 stream) const;

    // Appends MESH, one VERT per stream, INDX and BBOX to `file`. Does
    // nothing when nothing is built.
    void add_chunks(AssetWriter& file) const;

    // Forgets the mesh, keeping the buffers for the next build().
    void clear();
    // Releases the storage.
    void free();

private:
    BaseAllocator* allocator = nullptr;
};
