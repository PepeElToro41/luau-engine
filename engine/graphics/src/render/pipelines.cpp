#include "engine/render/pipelines.hpp"

#include "engine/render/components.hpp"
#include "engine/render/renderer.hpp"
#include "engine/utils/hash.hpp"

#include <cstdio>

usz PipelineKeyHash::operator()(const PipelineKey& key) const {
    return static_cast<usz>(HASH::fnv1a(&key, sizeof(key)));
}

GpuPipeline PIPELINES::get(Renderer& renderer, const EntityId shader_entity, const Shader& shader, const u64 tag, const GpuTargetFormats& formats, const GpuMeshLayout* mesh) {
    PipelineKey key;
    key.shader = shader_entity;
    key.generation = shader.generation;
    key.tag = tag;
    key.vertex_layout = mesh != nullptr ? mesh->hash : 0;
    key.formats = formats.hash();
    if (const GpuPipeline* found = renderer.pipelines.pipelines.find(key)) {
        return *found;
    }

    GpuPipeline pipeline;
    const ShaderPassProgram* pass = shader.program.find_pass(tag);
    if (pass == nullptr) {
        // Not an error: the shader has no variant for this pass.
        renderer.pipelines.pipelines.insert(key, pipeline);
        return pipeline;
    }
    const u32 pass_index = static_cast<u32>(pass - shader.program.passes);

    GpuPipelineDesc desc;
    desc.vertex = pass->vertex.stage_desc();
    if (pass->fragment.is_valid()) {
        desc.fragment = pass->fragment.stage_desc();
    }
    char error[256];
    if (mesh != nullptr) {
        if (!VERTEX_INPUT::build(pass->reflection, *mesh, desc.vertex_input, error, sizeof(error))) {
            RENDERER::log(renderer, RENDER_LOG_ERROR, "shader %s, pass %s: %s", shader.name, pass->tag_name, error);
            renderer.pipelines.pipelines.insert(key, pipeline);
            return pipeline;
        }
    } else {
        // A fullscreen pass covers the target whatever its winding and
        // ignores whatever depth the target holds.
        desc.raster.cull = GPU_CULL_NONE;
        desc.depth.test = false;
        desc.depth.write = false;
    }

    // Set 0 is the frame's, set 1 the pass's inputs, set 2 the material;
    // the backend fills gaps with the empty layout.
    const u32 sets = pass->reflection.set_count();
    desc.bind_layout_count = sets > 1 ? sets : 1;
    desc.bind_layouts[0] = renderer.frame_layout;
    if (desc.bind_layout_count > 1) {
        desc.bind_layouts[1] = shader.input_layouts[pass_index];
    }
    if (desc.bind_layout_count > 2 && pass->uses_material()) {
        desc.bind_layouts[2] = shader.material_layout;
    }
    desc.push_constant_size = pass->reflection.push_constant_offset + pass->reflection.push_constant_size;
    desc.targets = formats;
    char name[96];
    snprintf(name, sizeof(name), "%s/%s", shader.name, pass->tag_name);
    desc.name = name;

    pipeline = GPU::create_pipeline(renderer.gpu, desc);
    if (!pipeline.is_valid()) {
        RENDERER::log(renderer, RENDER_LOG_ERROR, "shader %s, pass %s: pipeline creation failed", shader.name, pass->tag_name);
    }
    renderer.pipelines.pipelines.insert(key, pipeline);
    return pipeline;
}

void PIPELINES::release_shader(Renderer& renderer, const EntityId shader) {
    DynamicArray<PipelineKey> stale;
    for (auto& entry : renderer.pipelines.pipelines) {
        if (entry.key.shader == shader) {
            if (entry.value.is_valid()) {
                GPU::release_pipeline(renderer.gpu, entry.value);
            }
            stale.push(entry.key);
        }
    }
    for (const PipelineKey& key : stale) {
        renderer.pipelines.pipelines.remove(key);
    }
    stale.free();
}

void PIPELINES::free(Renderer& renderer) {
    for (auto& entry : renderer.pipelines.pipelines) {
        if (entry.value.is_valid()) {
            GPU::destroy_pipeline(renderer.gpu, entry.value);
        }
    }
    renderer.pipelines.pipelines.free();
}
