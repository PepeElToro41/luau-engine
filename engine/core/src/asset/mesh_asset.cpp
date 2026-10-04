#include "engine/asset/mesh_asset.hpp"

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

    if (!file.is_parsed() || file.header->type != ASSET_TYPE::MESH) {
        return MESH_PARSE_NOT_A_MESH;
    }
    const ChunkEntry* desc_chunk = file.find_chunk(CHUNK_TAG::MESH);
    const ChunkEntry* index_chunk = file.find_chunk(CHUNK_TAG::INDICES);
    const ChunkEntry* bounds_chunk = file.find_chunk(CHUNK_TAG::BOUNDS);
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
        vertex_chunk = file.find_chunk(CHUNK_TAG::VERTICES, vertex_chunk);
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
    if (file.find_chunk(CHUNK_TAG::VERTICES, vertex_chunk) != nullptr) {
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
