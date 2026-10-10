#pragma once

#include "engine/asset/asset_types/mesh_asset.hpp"
#include "engine/defines.hpp"
#include "engine/gpu/gpu_types.hpp"
#include "engine/shaders/reflection.hpp"

// A mesh's vertex layout as the GPU sees it (streams and attributes from the
// asset, plus the VERTEX_LAYOUT hash pipelines are keyed by), and the
// function that matches it to a vertex stage's reflected inputs under the
// VERTEX_LAYOUT location convention.

struct GpuMeshLayout {
    VertexStreamDesc streams[MESH_ASSET::MAX_STREAMS] = {};
    u32 stream_count = 0;
    VertexAttributeDesc attributes[MESH_ASSET::MAX_ATTRIBUTES] = {};
    u32 attribute_count = 0;
    u64 hash = 0;

    void set(const VertexStreamDesc* streams, u32 stream_count, const VertexAttributeDesc* attributes, u32 attribute_count);
    const VertexAttributeDesc* find_location(u32 location) const;
};

namespace VERTEX_INPUT {

// Fills `out` with one binding per mesh stream and one attribute per input
// the shader declares. False (with `error`) if the mesh lacks an attribute
// or its format does not match the input's scalar type.
bool build(const ShaderReflection& vertex, const GpuMeshLayout& mesh, GpuVertexInput& out, char* error, usz error_size);

} // namespace VERTEX_INPUT
