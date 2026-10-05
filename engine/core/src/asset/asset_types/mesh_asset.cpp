#include "engine/asset/asset_types/mesh_asset.hpp"

#include "engine/memory/heap_allocator.hpp"

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>

// --- Formats --------------------------------------------------------------------

u32 VERTEX_FORMAT::size(const u32 format) {
    switch (format) {
    case VERTEX_FORMAT_F32: return 4;
    case VERTEX_FORMAT_F32x2: return 8;
    case VERTEX_FORMAT_F32x3: return 12;
    case VERTEX_FORMAT_F32x4: return 16;
    case VERTEX_FORMAT_F16x2: return 4;
    case VERTEX_FORMAT_F16x4: return 8;
    case VERTEX_FORMAT_UNORM8x4: return 4;
    case VERTEX_FORMAT_SNORM8x4: return 4;
    case VERTEX_FORMAT_UNORM16x2: return 4;
    case VERTEX_FORMAT_UNORM16x4: return 8;
    case VERTEX_FORMAT_SNORM16x2: return 4;
    case VERTEX_FORMAT_SNORM16x4: return 8;
    case VERTEX_FORMAT_UINT8x4: return 4;
    case VERTEX_FORMAT_UINT16x4: return 8;
    case VERTEX_FORMAT_UINT32: return 4;
    default: return 0;
    }
}

const char* VERTEX_FORMAT::name(const u32 format) {
    switch (format) {
    case VERTEX_FORMAT_NONE: return "none";
    case VERTEX_FORMAT_F32: return "F32";
    case VERTEX_FORMAT_F32x2: return "F32x2";
    case VERTEX_FORMAT_F32x3: return "F32x3";
    case VERTEX_FORMAT_F32x4: return "F32x4";
    case VERTEX_FORMAT_F16x2: return "F16x2";
    case VERTEX_FORMAT_F16x4: return "F16x4";
    case VERTEX_FORMAT_UNORM8x4: return "UNORM8x4";
    case VERTEX_FORMAT_SNORM8x4: return "SNORM8x4";
    case VERTEX_FORMAT_UNORM16x2: return "UNORM16x2";
    case VERTEX_FORMAT_UNORM16x4: return "UNORM16x4";
    case VERTEX_FORMAT_SNORM16x2: return "SNORM16x2";
    case VERTEX_FORMAT_SNORM16x4: return "SNORM16x4";
    case VERTEX_FORMAT_UINT8x4: return "UINT8x4";
    case VERTEX_FORMAT_UINT16x4: return "UINT16x4";
    case VERTEX_FORMAT_UINT32: return "UINT32";
    default: return "unknown";
    }
}

// --- Errors -----------------------------------------------------------------------

const char* MESH_ASSET::parse_error_name(const MeshParseError error) {
    switch (error) {
    case MESH_PARSE_OK: return "ok";
    case MESH_PARSE_NOT_A_MESH: return "not a mesh asset";
    case MESH_PARSE_MISSING_CHUNK: return "missing MESH, VERT, INDX or BBOX chunk";
    case MESH_PARSE_UNSUPPORTED_VERSION: return "unsupported mesh payload version";
    case MESH_PARSE_BAD_DESC: return "invalid mesh description";
    case MESH_PARSE_BAD_STREAM: return "invalid vertex stream";
    case MESH_PARSE_BAD_ATTRIBUTE: return "invalid vertex attribute";
    case MESH_PARSE_BAD_INDICES: return "index chunk size does not match the description";
    case MESH_PARSE_BAD_SUBMESH: return "invalid submesh";
    case MESH_PARSE_BAD_BOUNDS: return "invalid bounds chunk";
    }
    return "unknown error";
}

// --- MeshAssetView ----------------------------------------------------------------

void MeshAssetView::reset() {
    this->desc = nullptr;
    this->streams = nullptr;
    this->attributes = nullptr;
    this->submeshes = nullptr;
    for (u32 i = 0; i < MESH_ASSET::MAX_STREAMS; ++i) {
        this->vertex_chunks[i] = nullptr;
    }
    this->index_chunk = nullptr;
    this->bounds_chunk = nullptr;
}

namespace {

bool desc_is_valid(const MeshDesc& desc) {
    if (desc.vertex_count == 0 || desc.index_count == 0 || desc.index_count % 3 != 0) {
        return false;
    }
    if (MESH_ASSET::index_size(desc.index_format) == 0 || desc.topology != MESH_TOPOLOGY_TRIANGLE_LIST) {
        return false;
    }
    if (desc.stream_count == 0 || desc.stream_count > MESH_ASSET::MAX_STREAMS) {
        return false;
    }
    if (desc.attribute_count == 0 || desc.attribute_count > MESH_ASSET::MAX_ATTRIBUTES) {
        return false;
    }
    if (desc.submesh_count == 0 || desc.flags != 0) {
        return false;
    }
    for (u32 i = 0; i < 8; ++i) {
        if (desc.reserved[i] != 0) {
            return false;
        }
    }
    return true;
}

} // namespace

MeshParseError MeshAssetView::parse(const AssetView& file, const void* payload, const usz size) {
    this->reset();

    if (!file.is_ok() || file.header->type != ASSET_TYPE::MESH) {
        return MESH_PARSE_NOT_A_MESH;
    }
    const ChunkEntry* desc_chunk = file.find_chunk(CHUNK_TYPE::MESH);
    const ChunkEntry* index_chunk = file.find_chunk(CHUNK_TYPE::INDICES);
    const ChunkEntry* bounds_chunk = file.find_chunk(CHUNK_TYPE::BOUNDS);
    if (desc_chunk == nullptr || index_chunk == nullptr || bounds_chunk == nullptr) {
        return MESH_PARSE_MISSING_CHUNK;
    }
    if (desc_chunk->version != MESH_ASSET::VERSION || index_chunk->version != MESH_ASSET::VERSION ||
        bounds_chunk->version != MESH_ASSET::VERSION) {
        return MESH_PARSE_UNSUPPORTED_VERSION;
    }

    // The payload must be the MESH chunk's bytes; copy the desc out so its
    // counts are checked before the tables are trusted.
    if (payload == nullptr || size != desc_chunk->size || size < sizeof(MeshDesc)) {
        return MESH_PARSE_BAD_DESC;
    }
    ENGINE_ASSERT(reinterpret_cast<uintptr_t>(payload) % alignof(MeshDesc) == 0,
                  "MeshAssetView::parse: the payload must be %zu-byte aligned (any allocator gives this)", alignof(MeshDesc));
    const u8* desc_bytes = static_cast<const u8*>(payload);
    MeshDesc desc;
    std::memcpy(&desc, desc_bytes, sizeof(MeshDesc));
    if (!desc_is_valid(desc)) {
        return MESH_PARSE_BAD_DESC;
    }
    if (desc_chunk->size != MESH_ASSET::desc_size(desc.stream_count, desc.attribute_count, desc.submesh_count)) {
        return MESH_PARSE_BAD_DESC;
    }

    const VertexStreamDesc* streams = reinterpret_cast<const VertexStreamDesc*>(desc_bytes + sizeof(MeshDesc));
    const VertexAttributeDesc* attributes = reinterpret_cast<const VertexAttributeDesc*>(streams + desc.stream_count);
    const SubmeshDesc* submeshes = reinterpret_cast<const SubmeshDesc*>(attributes + desc.attribute_count);

    // Streams: the i-th VERT chunk belongs to the i-th stream, no more, no less.
    const ChunkEntry* vertex_chunks[MESH_ASSET::MAX_STREAMS] = {};
    const ChunkEntry* vertex_chunk = nullptr;
    for (u32 i = 0; i < desc.stream_count; ++i) {
        vertex_chunk = file.find_chunk(CHUNK_TYPE::VERTICES, vertex_chunk);
        if (vertex_chunk == nullptr) {
            return MESH_PARSE_MISSING_CHUNK;
        }
        if (vertex_chunk->version != MESH_ASSET::VERSION) {
            return MESH_PARSE_UNSUPPORTED_VERSION;
        }
        VertexStreamDesc stream;
        std::memcpy(&stream, streams + i, sizeof(VertexStreamDesc));
        if (stream.stride == 0 || stream.stride % 4 != 0 || stream.reserved != 0) {
            return MESH_PARSE_BAD_STREAM;
        }
        if (vertex_chunk->size != static_cast<u64>(stream.stride) * desc.vertex_count) {
            return MESH_PARSE_BAD_STREAM;
        }
        vertex_chunks[i] = vertex_chunk;
    }
    if (file.find_chunk(CHUNK_TYPE::VERTICES, vertex_chunk) != nullptr) {
        return MESH_PARSE_BAD_DESC;
    }

    // Attributes: known format, inside their stream's vertex, unique semantic.
    for (u32 i = 0; i < desc.attribute_count; ++i) {
        VertexAttributeDesc attribute;
        std::memcpy(&attribute, attributes + i, sizeof(VertexAttributeDesc));
        const u32 size = VERTEX_FORMAT::size(attribute.format);
        if (size == 0 || attribute.semantic > VERTEX_SEMANTIC_WEIGHTS || attribute.stream >= desc.stream_count) {
            return MESH_PARSE_BAD_ATTRIBUTE;
        }
        VertexStreamDesc stream;
        std::memcpy(&stream, streams + attribute.stream, sizeof(VertexStreamDesc));
        if (attribute.offset > stream.stride || size > stream.stride - attribute.offset) {
            return MESH_PARSE_BAD_ATTRIBUTE;
        }
        for (u32 j = 0; j < i; ++j) {
            VertexAttributeDesc other;
            std::memcpy(&other, attributes + j, sizeof(VertexAttributeDesc));
            if (other.semantic == attribute.semantic && other.semantic_index == attribute.semantic_index) {
                return MESH_PARSE_BAD_ATTRIBUTE;
            }
        }
    }

    if (index_chunk->size != static_cast<u64>(MESH_ASSET::index_size(desc.index_format)) * desc.index_count) {
        return MESH_PARSE_BAD_INDICES;
    }

    for (u32 i = 0; i < desc.submesh_count; ++i) {
        SubmeshDesc submesh;
        std::memcpy(&submesh, submeshes + i, sizeof(SubmeshDesc));
        if (submesh.index_count == 0 || submesh.index_count % 3 != 0 || submesh.first_index > desc.index_count ||
            submesh.index_count > desc.index_count - submesh.first_index) {
            return MESH_PARSE_BAD_SUBMESH;
        }
        if (submesh.base_vertex >= desc.vertex_count) {
            return MESH_PARSE_BAD_SUBMESH;
        }
        if (submesh.material != MESH_ASSET::NO_MATERIAL && submesh.material >= file.dependency_count()) {
            return MESH_PARSE_BAD_SUBMESH;
        }
        if (submesh.reserved[0] != 0 || submesh.reserved[1] != 0) {
            return MESH_PARSE_BAD_SUBMESH;
        }
    }

    if (bounds_chunk->size != sizeof(MeshBounds)) {
        return MESH_PARSE_BAD_BOUNDS;
    }

    this->desc = reinterpret_cast<const MeshDesc*>(desc_bytes);
    this->streams = streams;
    this->attributes = attributes;
    this->submeshes = submeshes;
    for (u32 i = 0; i < desc.stream_count; ++i) {
        this->vertex_chunks[i] = vertex_chunks[i];
    }
    this->index_chunk = index_chunk;
    this->bounds_chunk = bounds_chunk;
    return MESH_PARSE_OK;
}

MeshParseError MeshAssetView::parse(const AssetView& file, const ReadChunk& chunk) {
    if (!chunk.is_ok()) {
        this->reset();
        return MESH_PARSE_BAD_DESC;
    }
    return this->parse(file, chunk.chunk_data, static_cast<usz>(chunk.entry.size));
}

const VertexAttributeDesc* MeshAssetView::find_attribute(const u32 semantic, const u32 semantic_index) const {
    if (this->desc == nullptr) {
        return nullptr;
    }
    for (u32 i = 0; i < this->desc->attribute_count; ++i) {
        const VertexAttributeDesc& attribute = this->attributes[i];
        if (attribute.semantic == semantic && attribute.semantic_index == semantic_index) {
            return &attribute;
        }
    }
    return nullptr;
}

u64 MeshAssetView::stream_size(const u32 stream) const {
    if (this->desc == nullptr || stream >= this->desc->stream_count) {
        return 0;
    }
    return static_cast<u64>(this->streams[stream].stride) * this->desc->vertex_count;
}

u64 MeshAssetView::index_bytes() const {
    if (this->desc == nullptr) {
        return 0;
    }
    return static_cast<u64>(MESH_ASSET::index_size(this->desc->index_format)) * this->desc->index_count;
}

// --- Writing ----------------------------------------------------------------------

const char* MESH_ASSET::write_error_name(const MeshWriteError error) {
    switch (error) {
    case MESH_WRITE_OK: return "ok";
    case MESH_WRITE_BAD_SOURCE: return "invalid source counts";
    case MESH_WRITE_BAD_STREAM: return "invalid vertex stream";
    case MESH_WRITE_BAD_ATTRIBUTE: return "invalid vertex attribute";
    case MESH_WRITE_NO_POSITION: return "no POSITION 0 attribute in F32x3 or F32x4";
    case MESH_WRITE_BAD_INDEX: return "an index is out of the vertex range";
    case MESH_WRITE_BAD_SUBMESH: return "invalid submesh range";
    }
    return "unknown error";
}

namespace {

// Running min/max over positions; starts inverted so the first point sets it.
struct BoundsAccumulator {
    f32 min[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    f32 max[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    void add(const f32* point) {
        for (u32 axis = 0; axis < 3; ++axis) {
            this->min[axis] = point[axis] < this->min[axis] ? point[axis] : this->min[axis];
            this->max[axis] = point[axis] > this->max[axis] ? point[axis] : this->max[axis];
        }
    }
};

// Reads the xyz of vertex `index` from the position attribute's stream.
void load_position(const MeshSource& source, const VertexAttributeDesc& position, const u32 index, f32* out) {
    const MeshSourceStream& stream = source.streams[position.stream];
    const u8* vertex = static_cast<const u8*>(stream.vertices) + static_cast<usz>(stream.stride) * index + position.offset;
    std::memcpy(out, vertex, sizeof(f32) * 3);
}

MeshWriteError validate_source(const MeshSource& source, const VertexAttributeDesc** out_position) {
    if (source.vertex_count == 0 || source.stream_count == 0 || source.stream_count > MESH_ASSET::MAX_STREAMS ||
        source.attribute_count == 0 || source.attribute_count > MESH_ASSET::MAX_ATTRIBUTES) {
        return MESH_WRITE_BAD_SOURCE;
    }
    if (source.indices == nullptr || source.index_count == 0 || source.index_count % 3 != 0) {
        return MESH_WRITE_BAD_SOURCE;
    }
    if (source.submesh_count != 0 && source.submeshes == nullptr) {
        return MESH_WRITE_BAD_SOURCE;
    }

    for (u32 i = 0; i < source.stream_count; ++i) {
        const MeshSourceStream& stream = source.streams[i];
        if (stream.vertices == nullptr || stream.stride == 0 || stream.stride % 4 != 0) {
            return MESH_WRITE_BAD_STREAM;
        }
    }

    const VertexAttributeDesc* position = nullptr;
    for (u32 i = 0; i < source.attribute_count; ++i) {
        const VertexAttributeDesc& attribute = source.attributes[i];
        const u32 size = VERTEX_FORMAT::size(attribute.format);
        if (size == 0 || attribute.semantic > VERTEX_SEMANTIC_WEIGHTS || attribute.stream >= source.stream_count) {
            return MESH_WRITE_BAD_ATTRIBUTE;
        }
        const u32 stride = source.streams[attribute.stream].stride;
        if (attribute.offset > stride || size > stride - attribute.offset) {
            return MESH_WRITE_BAD_ATTRIBUTE;
        }
        for (u32 j = 0; j < i; ++j) {
            const VertexAttributeDesc& other = source.attributes[j];
            if (other.semantic == attribute.semantic && other.semantic_index == attribute.semantic_index) {
                return MESH_WRITE_BAD_ATTRIBUTE;
            }
        }
        if (attribute.semantic == VERTEX_SEMANTIC_POSITION && attribute.semantic_index == 0) {
            position = &attribute;
        }
    }
    if (position == nullptr || (position->format != VERTEX_FORMAT_F32x3 && position->format != VERTEX_FORMAT_F32x4)) {
        return MESH_WRITE_NO_POSITION;
    }

    for (u32 i = 0; i < source.index_count; ++i) {
        if (source.indices[i] >= source.vertex_count) {
            return MESH_WRITE_BAD_INDEX;
        }
    }

    for (u32 i = 0; i < source.submesh_count; ++i) {
        const MeshSourceSubmesh& submesh = source.submeshes[i];
        if (submesh.index_count == 0 || submesh.index_count % 3 != 0 || submesh.first_index > source.index_count ||
            submesh.index_count > source.index_count - submesh.first_index) {
            return MESH_WRITE_BAD_SUBMESH;
        }
    }

    *out_position = position;
    return MESH_WRITE_OK;
}

} // namespace

// --- MeshAssetWriter --------------------------------------------------------------

MeshAssetWriter::MeshAssetWriter() : MeshAssetWriter(MEMORY::heap_allocator()) {}

MeshAssetWriter::MeshAssetWriter(BaseAllocator* allocator)
    : streams(allocator), attributes(allocator), submeshes(allocator), vertices(allocator), indices(allocator), allocator(allocator) {}

MeshWriteError MeshAssetWriter::build(const MeshSource& source, const MeshImportOptions& options) {
    this->clear();

    const VertexAttributeDesc* position = nullptr;
    const MeshWriteError error = validate_source(source, &position);
    if (error != MESH_WRITE_OK) {
        return error;
    }

    // Tables. The layout is the importer's; it is copied as given.
    for (u32 i = 0; i < source.stream_count; ++i) {
        VertexStreamDesc stream;
        stream.stride = source.streams[i].stride;
        this->streams.push(stream);
    }
    for (u32 i = 0; i < source.attribute_count; ++i) {
        this->attributes.push(source.attributes[i]);
    }

    // Vertex data, every stream back to back.
    usz vertex_bytes = 0;
    for (u32 i = 0; i < source.stream_count; ++i) {
        vertex_bytes += static_cast<usz>(source.streams[i].stride) * source.vertex_count;
    }
    this->vertices.resize(vertex_bytes);
    u8* cursor = this->vertices.data;
    for (u32 i = 0; i < source.stream_count; ++i) {
        const usz size = static_cast<usz>(source.streams[i].stride) * source.vertex_count;
        std::memcpy(cursor, source.streams[i].vertices, size);
        cursor += size;
    }

    // Indices: 16-bit when every index fits (they are all below vertex_count).
    const bool compact = options.compact_indices && source.vertex_count <= 0x10000u;
    const u32 index_format = compact ? MESH_INDEX_U16 : MESH_INDEX_U32;
    this->indices.resize(static_cast<usz>(MESH_ASSET::index_size(index_format)) * source.index_count);
    if (compact) {
        u16* out = reinterpret_cast<u16*>(this->indices.data);
        for (u32 i = 0; i < source.index_count; ++i) {
            const u16 index = static_cast<u16>(source.indices[i]);
            std::memcpy(out + i, &index, sizeof(index));
        }
    } else {
        std::memcpy(this->indices.data, source.indices, sizeof(u32) * source.index_count);
    }

    // Whole mesh bounds over every vertex: the box, then the tightest sphere
    // around the box's center.
    BoundsAccumulator whole;
    f32 point[3];
    for (u32 i = 0; i < source.vertex_count; ++i) {
        load_position(source, *position, i, point);
        whole.add(point);
    }
    f32 radius_squared = 0;
    for (u32 axis = 0; axis < 3; ++axis) {
        this->bounds.min[axis] = whole.min[axis];
        this->bounds.max[axis] = whole.max[axis];
        this->bounds.center[axis] = (whole.min[axis] + whole.max[axis]) * 0.5f;
    }
    for (u32 i = 0; i < source.vertex_count; ++i) {
        load_position(source, *position, i, point);
        f32 distance_squared = 0;
        for (u32 axis = 0; axis < 3; ++axis) {
            const f32 delta = point[axis] - this->bounds.center[axis];
            distance_squared += delta * delta;
        }
        radius_squared = distance_squared > radius_squared ? distance_squared : radius_squared;
    }
    this->bounds.radius = sqrtf(radius_squared);

    // Submeshes, each with its own box over the vertices its indices reach.
    MeshSourceSubmesh whole_mesh;
    whole_mesh.index_count = source.index_count;
    const MeshSourceSubmesh* source_submeshes = source.submesh_count != 0 ? source.submeshes : &whole_mesh;
    const u32 submesh_count = source.submesh_count != 0 ? source.submesh_count : 1;
    for (u32 i = 0; i < submesh_count; ++i) {
        const MeshSourceSubmesh& from = source_submeshes[i];
        SubmeshDesc submesh;
        submesh.first_index = from.first_index;
        submesh.index_count = from.index_count;
        submesh.base_vertex = 0;
        submesh.material = from.material;
        BoundsAccumulator box;
        for (u32 j = from.first_index; j < from.first_index + from.index_count; ++j) {
            load_position(source, *position, source.indices[j], point);
            box.add(point);
        }
        for (u32 axis = 0; axis < 3; ++axis) {
            submesh.bounds_min[axis] = box.min[axis];
            submesh.bounds_max[axis] = box.max[axis];
        }
        this->submeshes.push(submesh);
    }

    this->desc.vertex_count = source.vertex_count;
    this->desc.index_count = source.index_count;
    this->desc.index_format = index_format;
    this->desc.topology = MESH_TOPOLOGY_TRIANGLE_LIST;
    this->desc.stream_count = source.stream_count;
    this->desc.attribute_count = source.attribute_count;
    this->desc.submesh_count = submesh_count;
    return MESH_WRITE_OK;
}

void MeshAssetWriter::write_desc(void* out) const {
    u8* cursor = static_cast<u8*>(out);
    std::memcpy(cursor, &this->desc, sizeof(MeshDesc));
    cursor += sizeof(MeshDesc);
    if (this->streams.count > 0) {
        std::memcpy(cursor, this->streams.data, sizeof(VertexStreamDesc) * this->streams.count);
        cursor += sizeof(VertexStreamDesc) * this->streams.count;
    }
    if (this->attributes.count > 0) {
        std::memcpy(cursor, this->attributes.data, sizeof(VertexAttributeDesc) * this->attributes.count);
        cursor += sizeof(VertexAttributeDesc) * this->attributes.count;
    }
    if (this->submeshes.count > 0) {
        std::memcpy(cursor, this->submeshes.data, sizeof(SubmeshDesc) * this->submeshes.count);
    }
}

const u8* MeshAssetWriter::stream_data(const u32 stream) const {
    if (stream >= this->desc.stream_count) {
        return nullptr;
    }
    usz offset = 0;
    for (u32 i = 0; i < stream; ++i) {
        offset += static_cast<usz>(this->streams[i].stride) * this->desc.vertex_count;
    }
    return this->vertices.data + offset;
}

u64 MeshAssetWriter::stream_size(const u32 stream) const {
    if (stream >= this->desc.stream_count) {
        return 0;
    }
    return static_cast<u64>(this->streams[stream].stride) * this->desc.vertex_count;
}

void MeshAssetWriter::add_chunks(AssetWriter& file) const {
    if (!this->is_built()) {
        return;
    }
    DynamicArray<u8> payload(this->allocator);
    payload.resize(this->desc_size());
    this->write_desc(payload.data);
    file.add_chunk(CHUNK_TYPE::MESH, MESH_ASSET::VERSION, 0, payload.data, payload.count);
    payload.free();

    for (u32 i = 0; i < this->desc.stream_count; ++i) {
        file.add_chunk(CHUNK_TYPE::VERTICES, MESH_ASSET::VERSION, 0, this->stream_data(i), static_cast<usz>(this->stream_size(i)));
    }
    file.add_chunk(CHUNK_TYPE::INDICES, MESH_ASSET::VERSION, 0, this->indices.data, this->indices.count);
    file.add_chunk(CHUNK_TYPE::BOUNDS, MESH_ASSET::VERSION, 0, &this->bounds, sizeof(MeshBounds));
}

void MeshAssetWriter::clear() {
    this->desc = MeshDesc{};
    this->bounds = MeshBounds{};
    this->streams.clear();
    this->attributes.clear();
    this->submeshes.clear();
    this->vertices.clear();
    this->indices.clear();
}

void MeshAssetWriter::free() {
    this->desc = MeshDesc{};
    this->bounds = MeshBounds{};
    this->streams.free();
    this->attributes.free();
    this->submeshes.free();
    this->vertices.free();
    this->indices.free();
}
