#pragma once

#include "engine/asset/asset_types/mesh_asset.hpp"
#include "engine/defines.hpp"
#include "engine/gpu/pipeline.hpp"
#include "engine/gpu/shaders/reflection.hpp"

// How a mesh's vertex streams feed a vertex shader. The mesh asset's stream
// and attribute tables map straight to vertex input bindings and
// attributes; the shader says which locations it reads (reflection), and
// the VERTEX_LAYOUT convention (mesh_asset.hpp) ties locations to
// semantics. build() fills a GraphicsPipelineDesc with exactly the
// attributes the shader uses.

// The parts of a mesh that decide its vertex input state, copied out of the
// asset so the pipeline cache can key on `hash` without the asset around.
struct GpuMeshLayout {
    VertexStreamDesc streams[MESH_ASSET::MAX_STREAMS] = {};
    u32 stream_count = 0;
    VertexAttributeDesc attributes[MESH_ASSET::MAX_ATTRIBUTES] = {};
    u32 attribute_count = 0;
    // VERTEX_LAYOUT::hash of the above.
    u64 hash = 0;

    void set(const VertexStreamDesc* streams, u32 stream_count, const VertexAttributeDesc* attributes, u32 attribute_count);
    // The attribute at `location` under the VERTEX_LAYOUT convention, or
    // nullptr.
    const VertexAttributeDesc* find_location(u32 location) const;
};

namespace VERTEX_INPUT {

// Adds one binding per stream (binding i = stream i) and one attribute per
// input the shader declares. False with `error` set when the mesh lacks an
// attribute the shader reads, or feeds it the wrong numeric class (an
// integer format into a float input or the reverse).
bool build(const ShaderReflection& vertex, const GpuMeshLayout& mesh, GraphicsPipelineDesc& desc, char* error, usz error_size);

} // namespace VERTEX_INPUT
