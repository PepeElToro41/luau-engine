#pragma once

#include "engine/asset/asset_resource_provider.hpp"
#include "engine/asset/asset_types/mesh_asset.hpp"
#include "engine/asset/asset_view.hpp"
#include "engine/defines.hpp"
#include "engine/gpu/resource_manager.hpp"
#include "engine/gpu/vertex_input.hpp"
#include "engine/templates/hash_map.hpp"

// Asset payloads as GPU objects, keyed by asset GUID and uploaded on first
// use: a mesh asset becomes device-local vertex and index buffers, a
// texture asset a mipmapped GpuTexture. The CPU payload is released from
// the AssetResourceProvider right after the upload. A failed load is
// remembered so it is reported once, not every frame.
//
//     const GpuMesh* mesh = cache.get_mesh(guid);        // nullptr: missing or bad
//     const GpuTexture* texture = cache.get_texture(guid);
//
// add_mesh() takes geometry built in memory (a MeshAssetWriter) under a
// GUID of the caller's choosing, for procedural meshes that have no file.

static constexpr u32 GPU_MESH_MAX_SUBMESHES = 16;

struct GpuMesh {
    GpuMeshLayout layout;
    GpuBuffer streams[MESH_ASSET::MAX_STREAMS] = {};
    u32 stream_count = 0;
    GpuBuffer indices;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
    u32 index_count = 0;
    u32 vertex_count = 0;
    SubmeshDesc submeshes[GPU_MESH_MAX_SUBMESHES] = {};
    u32 submesh_count = 0;
    MeshBounds bounds;
    // The asset could not be loaded; every get_mesh() returns nullptr.
    bool failed = false;
};

struct GpuAssetCache {
    bool init(GpuResourceManager* resources, AssetResourceProvider* provider);
    // Destroys every buffer and texture immediately. The GPU must be idle.
    void shutdown();

    const GpuMesh* get_mesh(const AssetGuid& guid);
    const GpuTexture* get_texture(const AssetGuid& guid);
    // Uploads `built` under `guid`, replacing nothing: false if the GUID is
    // taken or the upload fails.
    bool add_mesh(const AssetGuid& guid, const MeshAssetWriter& built);

    GpuResourceManager* resources = nullptr;
    AssetResourceProvider* provider = nullptr;

private:
    struct TextureEntry {
        GpuTexture texture;
        bool failed = false;
    };

    GpuMesh* load_mesh(const AssetGuid& guid);
    bool load_texture(const AssetGuid& guid, TextureEntry& out);
    bool upload_mesh(GpuMesh& mesh, const VertexStreamDesc* streams, u32 stream_count, const u8* const* stream_data, const VertexAttributeDesc* attributes,
        u32 attribute_count, const SubmeshDesc* submeshes, u32 submesh_count, u32 vertex_count, const void* index_data, u32 index_count, u32 index_format,
        const MeshBounds& bounds);
    void destroy_mesh(GpuMesh& mesh);

    HashMap<AssetGuid, GpuMesh*, AssetGuidHash> meshes;
    HashMap<AssetGuid, TextureEntry, AssetGuidHash> textures;
};
