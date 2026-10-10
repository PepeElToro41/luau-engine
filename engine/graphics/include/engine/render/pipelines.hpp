#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/gpu/gpu.hpp"
#include "engine/render/vertex_input.hpp"
#include "engine/templates/hash_map.hpp"

// The pipeline cache: one GpuPipeline per (shader entity, shader
// generation, pass tag, mesh vertex layout, target formats), built on first
// request. A failed build is cached as an invalid pipeline so it is
// reported once. A reloaded shader gets a new generation, so its old
// pipelines simply stop being requested; release_shader() frees them.

struct Renderer;
struct Shader;

struct PipelineKey {
    EntityId shader = 0;
    u32 generation = 0;
    u64 tag = 0;
    u64 vertex_layout = 0;
    u64 formats = 0;

    bool operator==(const PipelineKey& other) const {
        return this->shader == other.shader && this->generation == other.generation && this->tag == other.tag && this->vertex_layout == other.vertex_layout &&
               this->formats == other.formats;
    }
};

struct PipelineKeyHash {
    usz operator()(const PipelineKey& key) const;
};

struct PipelineCache {
    HashMap<PipelineKey, GpuPipeline, PipelineKeyHash> pipelines;
};

namespace PIPELINES {

// The pipeline drawing `shader`'s pass `tag` into `formats` with `mesh`'s
// vertex layout (nullptr: no vertex input, fullscreen state). Invalid when
// the shader has no such pass, the mesh lacks an attribute, or creation
// failed (logged once).
GpuPipeline get(Renderer& renderer, EntityId shader_entity, const Shader& shader, u64 tag, const GpuTargetFormats& formats, const GpuMeshLayout* mesh);
// Releases (deferred) every pipeline of `shader`, any generation.
void release_shader(Renderer& renderer, EntityId shader);
// Destroys every pipeline now. The GPU must be idle.
void free(Renderer& renderer);

} // namespace PIPELINES
