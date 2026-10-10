#include "render/passes.hpp"

#include "engine/ecs/world.hpp"
#include "engine/memory/temporal_allocator.hpp"
#include "engine/render/components.hpp"
#include "engine/render/renderer.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "engine/utils/hash.hpp"

#include <algorithm>

static constexpr u64 FULLSCREEN_TAG = HASH::fnv1a_str("fullscreen");

// --- Bind groups ----------------------------------------------------------------

GpuBindGroup RENDER_PASSES::material_group(Renderer& renderer, const EntityId entity, Material& material, const Shader& shader) {
    if (GpuBindGroup* cached = renderer.material_groups.find(entity)) {
        return *cached;
    }
    GpuBindGroup group;
    if (!shader.material_layout.is_valid()) {
        renderer.material_groups.insert(entity, group);
        return group;
    }
    MATERIAL::sync(material, shader);

    GpuBindGroupDesc desc;
    const ShaderReflection& interface = shader.program.material_interface;
    for (u32 b = 0; b < interface.binding_count; ++b) {
        const ReflectedBinding& binding = interface.bindings[b];
        switch (binding.type) {
        case GPU_BINDING_UNIFORM_BUFFER: {
            if (material.params == nullptr) {
                break;
            }
            const GpuUniformRange range = GPU::push_uniforms(renderer.gpu, material.params, material.param_size);
            if (range.buffer.is_valid()) {
                desc.bind_buffer(binding.binding, range.buffer, range.offset, range.size);
            }
            break;
        }
        case GPU_BINDING_TEXTURE: {
            GpuTexture texture = renderer.white;
            for (u32 i = 0; i < material.texture_count; ++i) {
                if (material.textures[i].binding == binding.binding) {
                    if (const GpuTexture* loaded = GPU_ASSETS::get_texture(renderer.assets, material.textures[i].texture)) {
                        texture = *loaded;
                    }
                }
            }
            desc.bind_texture(binding.binding, texture);
            break;
        }
        case GPU_BINDING_SAMPLER: {
            GpuSampler sampler = renderer.samplers[RENDERER_SAMPLER_LINEAR];
            for (u32 i = 0; i < material.sampler_count; ++i) {
                if (material.samplers[i].binding == binding.binding) {
                    sampler = GPU::sampler(renderer.gpu, material.samplers[i].sampler);
                }
            }
            desc.bind_sampler(binding.binding, sampler);
            break;
        }
        default:
            break;
        }
    }
    group = GPU::transient_bind_group(renderer.gpu, shader.material_layout, desc);
    renderer.material_groups.insert(entity, group);
    return group;
}

GpuBindGroup RENDER_PASSES::input_group(Renderer& renderer, const RenderPassContext& ctx, const Shader& shader, const u32 pass_index) {
    const GpuBindLayout layout = shader.input_layouts[pass_index];
    if (!layout.is_valid()) {
        return GpuBindGroup{};
    }
    const ShaderReflection& reflection = shader.program.passes[pass_index].reflection;
    GpuBindGroupDesc desc;
    for (u32 b = 0; b < reflection.binding_count; ++b) {
        const ReflectedBinding& binding = reflection.bindings[b];
        if (binding.set != 1) {
            continue;
        }
        if (binding.type == GPU_BINDING_TEXTURE) {
            const bool present = binding.binding < ctx.input_count && ctx.inputs[binding.binding].is_valid();
            desc.bind_texture(binding.binding, present ? ctx.inputs[binding.binding] : renderer.white);
        } else if (binding.type == GPU_BINDING_SAMPLER) {
            desc.bind_sampler(binding.binding, renderer.samplers[RENDERER_SAMPLER_LINEAR_CLAMP]);
        }
    }
    return GPU::transient_bind_group(renderer.gpu, layout, desc);
}

// --- Draw scene -----------------------------------------------------------------

namespace {

struct DrawItem {
    GpuPipeline pipeline;
    EntityId shader = 0;
    u32 pass_index = 0;
    EntityId material = 0;
    const GpuMesh* mesh = nullptr;
    Matrix4x4 model;
    u32 push_size = 0;
};

bool draw_order(const DrawItem& a, const DrawItem& b) {
    if (a.pipeline.id != b.pipeline.id) {
        return a.pipeline.id < b.pipeline.id;
    }
    if (a.material != b.material) {
        return a.material < b.material;
    }
    return a.mesh < b.mesh;
}

} // namespace

void RENDER_PASSES::draw_scene(Renderer& renderer, const RenderPassContext& ctx) {
    World* world = renderer.world;
    if (world == nullptr || ctx.desc == nullptr) {
        return;
    }
    const u64 tag = ctx.desc->tag_hash;

    // Collect. A MeshRenderer and a PrimitiveRenderer differ only in where
    // the GpuMesh comes from; `mesh` may be null when it could not be loaded.
    TemporalAllocator temp = TemporalAllocator::create();
    DynamicArray<DrawItem> items(&temp);
    const auto collect = [&](const Transform& transform, const EntityId material_entity, const GpuMesh* mesh) {
        if (mesh == nullptr) {
            return;
        }
        const Material* material = world->get<Material>(material_entity);
        if (material == nullptr) {
            return;
        }
        const Shader* shader = world->get<Shader>(material->shader);
        if (shader == nullptr || !shader->program.is_valid()) {
            return;
        }
        const ShaderPassProgram* pass = shader->program.find_pass(tag);
        if (pass == nullptr) {
            return;
        }
        const GpuPipeline pipeline = PIPELINES::get(renderer, material->shader, *shader, tag, ctx.formats, &mesh->layout);
        if (!pipeline.is_valid()) {
            return;
        }
        DrawItem item;
        item.pipeline = pipeline;
        item.shader = material->shader;
        item.pass_index = static_cast<u32>(pass - shader->program.passes);
        item.material = material_entity;
        item.mesh = mesh;
        item.model = transform.matrix();
        item.push_size = pass->reflection.push_constant_size;
        items.push(item);
    };
    world->query<Transform, MeshRenderer>().each([&](const EntityId, Transform& transform, MeshRenderer& mesh_renderer) {
        if (mesh_renderer.material != 0) {
            collect(transform, mesh_renderer.material, GPU_ASSETS::get_mesh(renderer.assets, mesh_renderer.mesh));
        }
    });
    world->query<Transform, PrimitiveRenderer>().each([&](const EntityId, Transform& transform, PrimitiveRenderer& primitive) {
        if (primitive.material != 0) {
            collect(transform, primitive.material, GPU_ASSETS::get_primitive(renderer.assets, primitive.shape));
        }
    });
    if (items.count == 0) {
        return;
    }
    std::sort(items.data, items.data + items.count, draw_order);

    // Record. Sets are rebound after every pipeline change: pipeline layouts
    // differ in push constants, which disturbs the bound sets.
    GpuPipeline bound_pipeline;
    EntityId bound_material = 0;
    const GpuMesh* bound_mesh = nullptr;
    for (const DrawItem& item : items) {
        if (item.pipeline.id != bound_pipeline.id) {
            GPU::cmd_bind_pipeline(ctx.cmd, item.pipeline);
            GPU::cmd_bind_group(ctx.cmd, 0, ctx.frame_group);
            const Shader* shader = world->get<Shader>(item.shader);
            const GpuBindGroup inputs = input_group(renderer, ctx, *shader, item.pass_index);
            if (inputs.is_valid()) {
                GPU::cmd_bind_group(ctx.cmd, 1, inputs);
            }
            bound_pipeline = item.pipeline;
            bound_material = 0;
        }
        if (item.material != bound_material) {
            Material* material = world->get<Material>(item.material);
            const Shader* shader = world->get<Shader>(item.shader);
            if (material != nullptr && shader != nullptr && shader->program.passes[item.pass_index].uses_material()) {
                const GpuBindGroup group = material_group(renderer, item.material, *material, *shader);
                if (group.is_valid()) {
                    GPU::cmd_bind_group(ctx.cmd, 2, group);
                }
            }
            bound_material = item.material;
        }
        if (item.push_size > 0) {
            const u32 size = item.push_size < sizeof(item.model) ? item.push_size : static_cast<u32>(sizeof(item.model));
            GPU::cmd_push_constants(ctx.cmd, &item.model, size);
        }
        if (item.mesh != bound_mesh) {
            for (u32 s = 0; s < item.mesh->stream_count; ++s) {
                GPU::cmd_bind_vertex_buffer(ctx.cmd, s, item.mesh->streams[s]);
            }
            GPU::cmd_bind_index_buffer(ctx.cmd, item.mesh->indices, item.mesh->index_type);
            bound_mesh = item.mesh;
        }
        for (u32 i = 0; i < item.mesh->submesh_count; ++i) {
            const SubmeshDesc& submesh = item.mesh->submeshes[i];
            GPU::cmd_draw_indexed(ctx.cmd, submesh.index_count, submesh.first_index, static_cast<i32>(submesh.base_vertex));
        }
    }
    items.free();
}

// --- Fullscreen -----------------------------------------------------------------

void RENDER_PASSES::fullscreen(Renderer& renderer, const RenderPassContext& ctx) {
    if (ctx.desc == nullptr || ctx.desc->shader == 0 || renderer.world == nullptr) {
        return;
    }
    const u64 tag = ctx.desc->tag_hash != 0 ? ctx.desc->tag_hash : FULLSCREEN_TAG;
    const Shader* shader = renderer.world->get<Shader>(ctx.desc->shader);
    const ShaderPassProgram* pass = shader != nullptr ? shader->program.find_pass(tag) : nullptr;
    if (pass == nullptr) {
        return;
    }
    const GpuPipeline pipeline = PIPELINES::get(renderer, ctx.desc->shader, *shader, tag, ctx.formats, nullptr);
    if (!pipeline.is_valid()) {
        return;
    }
    GPU::cmd_bind_pipeline(ctx.cmd, pipeline);
    GPU::cmd_bind_group(ctx.cmd, 0, ctx.frame_group);
    const GpuBindGroup inputs = input_group(renderer, ctx, *shader, static_cast<u32>(pass - shader->program.passes));
    if (inputs.is_valid()) {
        GPU::cmd_bind_group(ctx.cmd, 1, inputs);
    }
    if (pass->has_push_constants() && ctx.desc->constant_size > 0) {
        const u32 size = ctx.desc->constant_size < pass->reflection.push_constant_size ? ctx.desc->constant_size : pass->reflection.push_constant_size;
        GPU::cmd_push_constants(ctx.cmd, ctx.desc->constants, size);
    }
    GPU::cmd_draw(ctx.cmd, 3);
}

// --- Custom ---------------------------------------------------------------------

void RENDER_PASSES::custom(Renderer& renderer, const RenderPassContext& ctx) {
    if (ctx.desc == nullptr || ctx.desc->callback_hash == 0) {
        return;
    }
    const CustomPass* entry = renderer.custom_passes.find(ctx.desc->callback_hash);
    if (entry != nullptr && entry->fn != nullptr) {
        entry->fn(ctx, entry->user_data);
    }
}
