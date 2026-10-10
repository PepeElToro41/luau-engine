#include "engine/render/gpu_asset_cache.hpp"

#include "engine/asset/asset_types/texture_asset.hpp"
#include "engine/memory/heap_allocator.hpp"

#include <cstdio>
#include <cstring>
#include <new>

// --- Lifetime ------------------------------------------------------------------

void GPU_ASSETS::init(GpuAssetCache& cache, GpuContext* gpu, AssetResourceProvider* provider) {
    cache.gpu = gpu;
    cache.provider = provider;
}

static void destroy_mesh(GpuAssetCache& cache, GpuMesh& mesh) {
    for (u32 i = 0; i < mesh.stream_count; ++i) {
        GPU::destroy_buffer(cache.gpu, mesh.streams[i]);
        mesh.streams[i] = GpuBuffer{};
    }
    if (mesh.indices.is_valid()) {
        GPU::destroy_buffer(cache.gpu, mesh.indices);
        mesh.indices = GpuBuffer{};
    }
    mesh.stream_count = 0;
}

void GPU_ASSETS::shutdown(GpuAssetCache& cache) {
    if (cache.gpu != nullptr) {
        for (auto& entry : cache.meshes) {
            if (entry.value != nullptr) {
                destroy_mesh(cache, *entry.value);
                MEMORY::heap_allocator()->free(entry.value);
            }
        }
        for (auto& entry : cache.textures) {
            if (entry.value.texture.is_valid()) {
                GPU::destroy_texture(cache.gpu, entry.value.texture);
            }
        }
        for (GpuMesh*& primitive : cache.primitives) {
            if (primitive != nullptr) {
                destroy_mesh(cache, *primitive);
                MEMORY::heap_allocator()->free(primitive);
                primitive = nullptr;
            }
        }
    }
    cache.meshes.free();
    cache.textures.free();
    cache.gpu = nullptr;
    cache.provider = nullptr;
}

// --- Meshes --------------------------------------------------------------------

static bool upload_mesh(GpuAssetCache& cache, GpuMesh& mesh, const VertexStreamDesc* streams, const u32 stream_count, const u8* const* stream_data,
    const VertexAttributeDesc* attributes, const u32 attribute_count, const SubmeshDesc* submeshes, const u32 submesh_count, const u32 vertex_count,
    const void* index_data, const u32 index_count, const u32 index_format, const MeshBounds& bounds) {
    mesh.layout.set(streams, stream_count, attributes, attribute_count);
    mesh.vertex_count = vertex_count;
    mesh.index_count = index_count;
    mesh.index_type = index_format == MESH_INDEX_U32 ? GPU_INDEX_U32 : GPU_INDEX_U16;
    mesh.bounds = bounds;
    mesh.submesh_count = submesh_count < GPU_MESH_MAX_SUBMESHES ? submesh_count : GPU_MESH_MAX_SUBMESHES;
    memcpy(mesh.submeshes, submeshes, mesh.submesh_count * sizeof(SubmeshDesc));

    for (u32 i = 0; i < stream_count; ++i) {
        GpuBufferDesc desc;
        desc.size = static_cast<u64>(streams[i].stride) * vertex_count;
        desc.usage = GPU_BUFFER_USAGE_VERTEX;
        mesh.streams[i] = GPU::create_buffer(cache.gpu, desc, stream_data[i]);
        if (!mesh.streams[i].is_valid()) {
            destroy_mesh(cache, mesh);
            return false;
        }
        mesh.stream_count = i + 1;
    }
    GpuBufferDesc index_desc;
    index_desc.size = static_cast<u64>(MESH_ASSET::index_size(index_format)) * index_count;
    index_desc.usage = GPU_BUFFER_USAGE_INDEX;
    mesh.indices = GPU::create_buffer(cache.gpu, index_desc, index_data);
    if (!mesh.indices.is_valid()) {
        destroy_mesh(cache, mesh);
        return false;
    }
    return true;
}

static GpuMesh* load_mesh(GpuAssetCache& cache, const AssetGuid& guid) {
    GpuMesh* mesh = MEMORY::heap_allocator()->allocate_array<GpuMesh>(1);
    new (mesh) GpuMesh();
    mesh->failed = true;

    AssetResource* resource = cache.provider != nullptr ? cache.provider->get(guid) : nullptr;
    if (resource == nullptr) {
        fprintf(stderr, "[assets] mesh %016llx%016llx is not registered or could not be loaded\n", static_cast<unsigned long long>(guid.hi),
            static_cast<unsigned long long>(guid.lo));
        return mesh;
    }
    const ChunkEntry* desc_entry = nullptr;
    const u8* desc_payload = resource->find_payload(CHUNK_TYPE::MESH, &desc_entry);
    MeshAssetView view;
    const MeshParseError parsed = desc_payload != nullptr ? view.parse(resource->view, desc_payload, desc_entry->size) : MESH_PARSE_MISSING_CHUNK;
    if (parsed != MESH_PARSE_OK) {
        fprintf(stderr, "[assets] %s: %s\n", resource->path, MESH_ASSET::parse_error_name(parsed));
        cache.provider->unload(resource);
        return mesh;
    }

    const u8* stream_data[MESH_ASSET::MAX_STREAMS];
    bool ok = true;
    for (u32 i = 0; i < view.desc->stream_count; ++i) {
        stream_data[i] = resource->payload(static_cast<usz>(view.vertex_chunks[i] - resource->view.chunks));
        ok &= stream_data[i] != nullptr;
    }
    const u8* index_data = resource->payload(static_cast<usz>(view.index_chunk - resource->view.chunks));
    MeshBounds bounds;
    if (const u8* bounds_data = resource->payload(static_cast<usz>(view.bounds_chunk - resource->view.chunks))) {
        memcpy(&bounds, bounds_data, sizeof(bounds));
    }
    if (!ok || index_data == nullptr) {
        fprintf(stderr, "[assets] %s: geometry chunks are not resident\n", resource->path);
        cache.provider->unload(resource);
        return mesh;
    }
    mesh->failed = !upload_mesh(cache, *mesh, view.streams, view.desc->stream_count, stream_data, view.attributes, view.desc->attribute_count, view.submeshes,
        view.desc->submesh_count, view.desc->vertex_count, index_data, view.desc->index_count, view.desc->index_format, bounds);
    if (mesh->failed) {
        fprintf(stderr, "[assets] %s: upload failed\n", resource->path);
    }
    // The GPU has its copy.
    cache.provider->unload(resource);
    return mesh;
}

const GpuMesh* GPU_ASSETS::get_mesh(GpuAssetCache& cache, const AssetGuid& guid) {
    if (guid.is_null()) {
        return nullptr;
    }
    GpuMesh** found = cache.meshes.find(guid);
    if (found == nullptr) {
        GpuMesh* loaded = load_mesh(cache, guid);
        found = &cache.meshes.insert(guid, loaded);
    }
    return *found != nullptr && !(*found)->failed ? *found : nullptr;
}

// A GpuMesh on the heap with `built` uploaded into it; `failed` set when
// the upload did not go through.
static GpuMesh* upload_built(GpuAssetCache& cache, const MeshAssetWriter& built) {
    GpuMesh* mesh = MEMORY::heap_allocator()->allocate_array<GpuMesh>(1);
    new (mesh) GpuMesh();
    const u8* stream_data[MESH_ASSET::MAX_STREAMS];
    for (u32 i = 0; i < built.desc.stream_count; ++i) {
        stream_data[i] = built.stream_data(i);
    }
    mesh->failed = !upload_mesh(cache, *mesh, built.streams.data, built.desc.stream_count, stream_data, built.attributes.data, built.desc.attribute_count,
        built.submeshes.data, built.desc.submesh_count, built.desc.vertex_count, built.indices.data, built.desc.index_count, built.desc.index_format, built.bounds);
    return mesh;
}

bool GPU_ASSETS::add_mesh(GpuAssetCache& cache, const AssetGuid& guid, const MeshAssetWriter& built) {
    if (guid.is_null() || !built.is_built() || cache.meshes.contains(guid)) {
        return false;
    }
    GpuMesh* mesh = upload_built(cache, built);
    cache.meshes.insert(guid, mesh);
    return !mesh->failed;
}

// --- Primitives ----------------------------------------------------------------

const GpuMesh* GPU_ASSETS::get_primitive(GpuAssetCache& cache, const PrimitiveShape shape) {
    if (!PRIMITIVES::is_valid(shape)) {
        return nullptr;
    }
    GpuMesh*& slot = cache.primitives[shape];
    if (slot == nullptr) {
        MeshAssetWriter built;
        const MeshWriteError error = PRIMITIVES::build(shape, built, cache.primitive_tessellation);
        if (error != MESH_WRITE_OK) {
            fprintf(stderr, "[assets] primitive %s could not be built: %s\n", PRIMITIVES::name(shape), MESH_ASSET::write_error_name(error));
            slot = MEMORY::heap_allocator()->allocate_array<GpuMesh>(1);
            new (slot) GpuMesh();
            slot->failed = true;
        } else {
            slot = upload_built(cache, built);
            if (slot->failed) {
                fprintf(stderr, "[assets] primitive %s: upload failed\n", PRIMITIVES::name(shape));
            }
        }
        built.free();
    }
    return slot->failed ? nullptr : slot;
}

// --- Textures ------------------------------------------------------------------

static bool load_texture(GpuAssetCache& cache, const AssetGuid& guid, GpuTextureEntry& out) {
    AssetResource* resource = cache.provider != nullptr ? cache.provider->get(guid) : nullptr;
    if (resource == nullptr) {
        fprintf(stderr, "[assets] texture %016llx%016llx is not registered or could not be loaded\n", static_cast<unsigned long long>(guid.hi),
            static_cast<unsigned long long>(guid.lo));
        return false;
    }
    const ChunkEntry* desc_entry = nullptr;
    const u8* desc_payload = resource->find_payload(CHUNK_TYPE::TEXTURE, &desc_entry);
    TextureAssetView view;
    const TextureParseError parsed = desc_payload != nullptr ? view.parse(resource->view, desc_payload, desc_entry->size) : TEXTURE_PARSE_BAD_DESC;
    if (parsed != TEXTURE_PARSE_OK) {
        fprintf(stderr, "[assets] %s: texture payload is invalid (%d)\n", resource->path, static_cast<int>(parsed));
        cache.provider->unload(resource);
        return false;
    }
    const GpuFormat format = GPU_FORMAT::from_texture(view.desc->format);
    const u8* pixels = resource->payload(static_cast<usz>(view.pixels_chunk - resource->view.chunks));
    if (format == GPU_FORMAT_UNDEFINED || pixels == nullptr || view.desc->dimension != TEXTURE_DIMENSION_2D || view.desc->layers != 1) {
        fprintf(stderr, "[assets] %s: only 2D single-layer textures in known formats are uploaded\n", resource->path);
        cache.provider->unload(resource);
        return false;
    }

    GpuTextureDesc desc;
    desc.format = format;
    desc.width = view.desc->width;
    desc.height = view.desc->height;
    desc.mip_levels = view.mip_count();
    desc.usage = GPU_TEXTURE_USAGE_SAMPLED;
    GpuTexture texture = GPU::create_texture(cache.gpu, desc);
    bool ok = texture.is_valid();
    if (ok) {
        GpuTextureUpload uploads[TEXTURE_ASSET::MAX_MIPS];
        const u32 mips = view.mip_count() < TEXTURE_ASSET::MAX_MIPS ? view.mip_count() : TEXTURE_ASSET::MAX_MIPS;
        for (u32 m = 0; m < mips; ++m) {
            uploads[m].pixels = view.mip_data(pixels, m);
            uploads[m].size = view.layer_size(m);
            uploads[m].mip_level = m;
        }
        ok = GPU::upload_texture(cache.gpu, texture, uploads, mips);
        if (!ok) {
            GPU::destroy_texture(cache.gpu, texture);
        }
    }
    cache.provider->unload(resource);
    if (!ok) {
        fprintf(stderr, "[assets] %s: upload failed\n", resource->path);
        return false;
    }
    out.texture = texture;
    return true;
}

const GpuTexture* GPU_ASSETS::get_texture(GpuAssetCache& cache, const AssetGuid& guid) {
    if (guid.is_null()) {
        return nullptr;
    }
    GpuTextureEntry* found = cache.textures.find(guid);
    if (found == nullptr) {
        GpuTextureEntry entry;
        entry.failed = !load_texture(cache, guid, entry);
        found = &cache.textures.insert(guid, entry);
    }
    return found->failed ? nullptr : &found->texture;
}
