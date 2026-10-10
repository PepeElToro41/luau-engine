#pragma once

#include "engine/asset/asset_resource_provider.hpp"
#include "engine/asset/asset_types/mesh_asset.hpp"
#include "engine/asset/asset_view.hpp"
#include "engine/defines.hpp"
#include "engine/geometry/primitives.hpp"
#include "engine/gpu/gpu.hpp"
#include "engine/render/vertex_input.hpp"
#include "engine/templates/hash_map.hpp"

// Asset payloads as GPU objects, keyed by asset GUID and uploaded on first
// use: a mesh asset becomes device-local vertex and index buffers, a
// texture asset a mipmapped GpuTexture. The CPU payload is released from
// the AssetResourceProvider right after the upload. A failed load is
// remembered so it is reported once, not every frame.
//
//     const GpuMesh* mesh = GPU_ASSETS::get_mesh(cache, guid);   // nullptr: missing or bad
//     const GpuTexture* texture = GPU_ASSETS::get_texture(cache, guid);
//
// add_mesh() takes geometry built in memory (a MeshAssetWriter) under a
// GUID of the caller's choosing, for procedural meshes that have no file.
//
// The primitive shapes (engine/geometry/primitives.hpp) have no GUID: they
// are generated through PRIMITIVES::build and uploaded the first time a
// shape is asked for, one GpuMesh per shape for the whole scene.
//
//     const GpuMesh* cube = GPU_ASSETS::get_primitive(cache, PRIMITIVE_CUBE);

static constexpr u32 GPU_MESH_MAX_SUBMESHES = 16;

struct GpuMesh {
    GpuMeshLayout layout;
    GpuBuffer streams[MESH_ASSET::MAX_STREAMS] = {};
    u32 stream_count = 0;
    GpuBuffer indices;
    GpuIndexType index_type = GPU_INDEX_U16;
    u32 index_count = 0;
    u32 vertex_count = 0;
    SubmeshDesc submeshes[GPU_MESH_MAX_SUBMESHES] = {};
    u32 submesh_count = 0;
    MeshBounds bounds;
    // The asset could not be loaded; every get_mesh() returns nullptr.
    bool failed = false;
};

struct GpuTextureEntry {
    GpuTexture texture;
    bool failed = false;
};

struct GpuAssetCache {
    GpuContext* gpu = nullptr;
    AssetResourceProvider* provider = nullptr;
    HashMap<AssetGuid, GpuMesh*, AssetGuidHash> meshes;
    HashMap<AssetGuid, GpuTextureEntry, AssetGuidHash> textures;
    // One per PrimitiveShape, nullptr until first asked for.
    GpuMesh* primitives[PRIMITIVE_COUNT] = {};
    // How the round shapes are tessellated when they are built; changing it
    // after a shape was built has no effect on that shape.
    PrimitiveTessellation primitive_tessellation;
};

namespace GPU_ASSETS {

void init(GpuAssetCache& cache, GpuContext* gpu, AssetResourceProvider* provider);
// Destroys every buffer and texture immediately. The GPU must be idle.
void shutdown(GpuAssetCache& cache);

const GpuMesh* get_mesh(GpuAssetCache& cache, const AssetGuid& guid);
// The pointer is valid until the next get_texture call.
const GpuTexture* get_texture(GpuAssetCache& cache, const AssetGuid& guid);
// Uploads `built` under `guid`, replacing nothing: false if the GUID is
// taken or the upload fails.
bool add_mesh(GpuAssetCache& cache, const AssetGuid& guid, const MeshAssetWriter& built);
// The shared mesh of a primitive shape, generated and uploaded on first
// use; nullptr for an unknown shape or when the upload failed (remembered,
// reported once).
const GpuMesh* get_primitive(GpuAssetCache& cache, PrimitiveShape shape);

} // namespace GPU_ASSETS
