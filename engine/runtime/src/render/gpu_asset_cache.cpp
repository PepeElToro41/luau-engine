#include "engine/render/gpu_asset_cache.hpp"

#include "engine/asset/asset_types/texture_asset.hpp"
#include "engine/gpu/asset_formats.hpp"
#include "engine/memory/heap_allocator.hpp"

#include <cstdio>
#include <cstring>

// --- Lifetime ------------------------------------------------------------------

bool GpuAssetCache::init(GpuResourceManager* resources, AssetResourceProvider* provider) {
    this->resources = resources;
    this->provider = provider;
    return true;
}

void GpuAssetCache::shutdown() {
    if (this->resources != nullptr) {
        for (auto& entry : this->meshes) {
            if (entry.value != nullptr) {
                this->destroy_mesh(*entry.value);
                MEMORY::heap_allocator()->free(entry.value);
            }
        }
        for (auto& entry : this->textures) {
            if (entry.value.texture.is_valid()) {
                this->resources->destroy_texture(entry.value.texture);
            }
        }
    }
    this->meshes.free();
    this->textures.free();
    this->resources = nullptr;
    this->provider = nullptr;
}

void GpuAssetCache::destroy_mesh(GpuMesh& mesh) {
    for (u32 i = 0; i < mesh.stream_count; ++i) {
        if (mesh.streams[i].is_valid()) {
            this->resources->destroy_buffer(mesh.streams[i]);
        }
    }
    if (mesh.indices.is_valid()) {
        this->resources->destroy_buffer(mesh.indices);
    }
    mesh.stream_count = 0;
}

// --- Meshes --------------------------------------------------------------------

bool GpuAssetCache::upload_mesh(GpuMesh& mesh, const VertexStreamDesc* streams, const u32 stream_count, const u8* const* stream_data,
    const VertexAttributeDesc* attributes, const u32 attribute_count, const SubmeshDesc* submeshes, const u32 submesh_count, const u32 vertex_count,
    const void* index_data, const u32 index_count, const u32 index_format, const MeshBounds& bounds) {
    mesh.layout.set(streams, stream_count, attributes, attribute_count);
    mesh.vertex_count = vertex_count;
    mesh.index_count = index_count;
    mesh.index_type = index_format == MESH_INDEX_U32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    mesh.bounds = bounds;
    mesh.submesh_count = submesh_count < GPU_MESH_MAX_SUBMESHES ? submesh_count : GPU_MESH_MAX_SUBMESHES;
    memcpy(mesh.submeshes, submeshes, mesh.submesh_count * sizeof(SubmeshDesc));

    for (u32 i = 0; i < stream_count; ++i) {
        GpuBufferDesc desc;
        desc.size = static_cast<VkDeviceSize>(streams[i].stride) * vertex_count;
        desc.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (!this->resources->create_buffer(desc, stream_data[i], desc.size, mesh.streams[i])) {
            this->destroy_mesh(mesh);
            return false;
        }
        mesh.stream_count = i + 1;
    }
    GpuBufferDesc index_desc;
    index_desc.size = static_cast<VkDeviceSize>(MESH_ASSET::index_size(index_format)) * index_count;
    index_desc.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (!this->resources->create_buffer(index_desc, index_data, index_desc.size, mesh.indices)) {
        this->destroy_mesh(mesh);
        return false;
    }
    return true;
}

GpuMesh* GpuAssetCache::load_mesh(const AssetGuid& guid) {
    GpuMesh* mesh = MEMORY::heap_allocator()->allocate_array<GpuMesh>(1);
    new (mesh) GpuMesh();
    mesh->failed = true;

    AssetResource* resource = this->provider != nullptr ? this->provider->get(guid) : nullptr;
    if (resource == nullptr) {
        fprintf(stderr, "[assets] mesh %016llx%016llx is not registered or could not be loaded\n",
            static_cast<unsigned long long>(guid.hi), static_cast<unsigned long long>(guid.lo));
        return mesh;
    }
    const ChunkEntry* desc_entry = nullptr;
    const u8* desc_payload = resource->find_payload(CHUNK_TYPE::MESH, &desc_entry);
    MeshAssetView view;
    const MeshParseError parsed = desc_payload != nullptr ? view.parse(resource->view, desc_payload, desc_entry->size) : MESH_PARSE_MISSING_CHUNK;
    if (parsed != MESH_PARSE_OK) {
        fprintf(stderr, "[assets] %s: %s\n", resource->path, MESH_ASSET::parse_error_name(parsed));
        this->provider->unload(resource);
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
        this->provider->unload(resource);
        return mesh;
    }
    mesh->failed = !this->upload_mesh(*mesh, view.streams, view.desc->stream_count, stream_data, view.attributes, view.desc->attribute_count,
        view.submeshes, view.desc->submesh_count, view.desc->vertex_count, index_data, view.desc->index_count, view.desc->index_format, bounds);
    if (mesh->failed) {
        fprintf(stderr, "[assets] %s: upload failed\n", resource->path);
    }
    // The GPU has its copy.
    this->provider->unload(resource);
    return mesh;
}

const GpuMesh* GpuAssetCache::get_mesh(const AssetGuid& guid) {
    if (guid.is_null()) {
        return nullptr;
    }
    GpuMesh** found = this->meshes.find(guid);
    if (found == nullptr) {
        GpuMesh* loaded = this->load_mesh(guid);
        found = &this->meshes.insert(guid, loaded);
    }
    return *found != nullptr && !(*found)->failed ? *found : nullptr;
}

bool GpuAssetCache::add_mesh(const AssetGuid& guid, const MeshAssetWriter& built) {
    if (guid.is_null() || !built.is_built() || this->meshes.contains(guid)) {
        return false;
    }
    GpuMesh* mesh = MEMORY::heap_allocator()->allocate_array<GpuMesh>(1);
    new (mesh) GpuMesh();
    const u8* stream_data[MESH_ASSET::MAX_STREAMS];
    for (u32 i = 0; i < built.desc.stream_count; ++i) {
        stream_data[i] = built.stream_data(i);
    }
    mesh->failed = !this->upload_mesh(*mesh, built.streams.data, built.desc.stream_count, stream_data, built.attributes.data, built.desc.attribute_count,
        built.submeshes.data, built.desc.submesh_count, built.desc.vertex_count, built.indices.data, built.desc.index_count, built.desc.index_format, built.bounds);
    this->meshes.insert(guid, mesh);
    return !mesh->failed;
}

// --- Textures ------------------------------------------------------------------

bool GpuAssetCache::load_texture(const AssetGuid& guid, TextureEntry& out) {
    AssetResource* resource = this->provider != nullptr ? this->provider->get(guid) : nullptr;
    if (resource == nullptr) {
        fprintf(stderr, "[assets] texture %016llx%016llx is not registered or could not be loaded\n",
            static_cast<unsigned long long>(guid.hi), static_cast<unsigned long long>(guid.lo));
        return false;
    }
    const ChunkEntry* desc_entry = nullptr;
    const u8* desc_payload = resource->find_payload(CHUNK_TYPE::TEXTURE, &desc_entry);
    TextureAssetView view;
    const TextureParseError parsed = desc_payload != nullptr ? view.parse(resource->view, desc_payload, desc_entry->size) : TEXTURE_PARSE_BAD_DESC;
    if (parsed != TEXTURE_PARSE_OK) {
        fprintf(stderr, "[assets] %s: texture payload is invalid (%d)\n", resource->path, static_cast<int>(parsed));
        this->provider->unload(resource);
        return false;
    }
    const VkFormat format = ASSET_FORMATS::texture_format(view.desc->format);
    const u8* pixels = resource->payload(static_cast<usz>(view.pixels_chunk - resource->view.chunks));
    if (format == VK_FORMAT_UNDEFINED || pixels == nullptr || view.desc->dimension != TEXTURE_DIMENSION_2D || view.desc->layers != 1) {
        fprintf(stderr, "[assets] %s: only 2D single-layer textures in known formats are uploaded\n", resource->path);
        this->provider->unload(resource);
        return false;
    }

    GpuTextureDesc desc;
    desc.format = format;
    desc.width = view.desc->width;
    desc.height = view.desc->height;
    desc.mip_levels = view.mip_count();
    desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    GpuTexture texture;
    bool ok = this->resources->create_texture(desc, texture);
    if (ok) {
        GpuTextureUpload uploads[TEXTURE_ASSET::MAX_MIPS];
        const u32 mips = view.mip_count() < TEXTURE_ASSET::MAX_MIPS ? view.mip_count() : TEXTURE_ASSET::MAX_MIPS;
        for (u32 m = 0; m < mips; ++m) {
            uploads[m].pixels = view.mip_data(pixels, m);
            uploads[m].size = view.layer_size(m);
            uploads[m].mip_level = m;
        }
        ok = this->resources->upload_texture(texture, uploads, mips);
        if (!ok) {
            this->resources->destroy_texture(texture);
        }
    }
    this->provider->unload(resource);
    if (!ok) {
        fprintf(stderr, "[assets] %s: upload failed\n", resource->path);
        return false;
    }
    out.texture = texture;
    return true;
}

const GpuTexture* GpuAssetCache::get_texture(const AssetGuid& guid) {
    if (guid.is_null()) {
        return nullptr;
    }
    TextureEntry* found = this->textures.find(guid);
    if (found == nullptr) {
        TextureEntry entry;
        entry.failed = !this->load_texture(guid, entry);
        found = &this->textures.insert(guid, entry);
    }
    return found->failed ? nullptr : &found->texture;
}
