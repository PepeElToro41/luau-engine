#include "engine/render/outline.hpp"

#include "engine/ecs/world.hpp"
#include "engine/gpu/gpu.hpp"
#include "engine/render/components.hpp"
#include "engine/render/gpu_asset_cache.hpp"
#include "engine/render/pipelines.hpp"
#include "engine/render/shader_library.hpp"
#include "engine/utils/log.hpp"

#include <cstdio>

namespace {

// The `#pragma pass` tags of render/outline.slang. A FULLSCREEN pass picks
// its shader's pass by the graph pass's tag, so the flood and the two
// resolve passes are one shader entity with three tags.
constexpr const char* FLOOD_TAG = "outline_flood";
constexpr const char* RESOLVE_TAG = "outline_resolve";
constexpr const char* RESOLVE_BEHIND_TAG = "outline_resolve_behind";

// The push constant blocks of render/engine/outline.slang.
struct FloodConstants {
    i32 step = 1;
};

struct ResolveConstants {
    f32 color[4] = {};
    f32 width = 0.0f;
    f32 softness = 0.0f;
};

u32 round_up_pow2(const u32 value) {
    u32 result = 1;
    while (result < value && result < (1u << 12)) {
        result <<= 1;
    }
    return result;
}

// Flood steps `max_width`, `max_width` / 2, ... 1.
u32 flood_count_for(const u32 max_width) {
    u32 count = 1;
    for (u32 step = max_width; step > 1; step >>= 1) {
        count += 1;
    }
    return count < OUTLINE_MAX_FLOOD_PASSES ? count : OUTLINE_MAX_FLOOD_PASSES;
}

// A resolve pass: the flooded buffer in, the frame out, its depth filled in
// by update() once it knows what the forward pass left.
RenderPassHandle add_resolve(RenderGraph& graph, const char* name, const char* tag, const EntityId shader, const RenderResourceHandle seeds) {
    const RenderPassHandle pass = graph.add_pass(name, RENDER_PASS_FULLSCREEN);
    if (!pass.is_valid()) {
        return pass;
    }
    graph.set_fullscreen_shader(pass, shader);
    graph.set_draw_scene_tag(pass, tag);
    graph.add_input(pass, seeds);
    graph.set_color_attachment(pass, 0, graph.backbuffer, RENDER_LOAD_LOAD, RENDER_STORE_STORE);
    return pass;
}

// The passes in the order they must run.
u32 wanted_order(const Outline& outline, RenderPassHandle* out) {
    u32 count = 0;
    out[count++] = outline.mask;
    for (u32 i = 0; i < outline.flood_count; ++i) {
        out[count++] = outline.flood[i];
    }
    // The occluded side first; the depth test keeps them apart anyway.
    if (outline.resolve_behind.is_valid()) {
        out[count++] = outline.resolve_behind;
    }
    out[count++] = outline.resolve;
    return count;
}

} // namespace

// --- Init ---------------------------------------------------------------------

bool OUTLINE::init(Renderer& renderer, Outline& outline, const OutlineDesc& desc) {
    RenderGraph& graph = renderer.graph;
    const char* prefix = desc.name != nullptr && desc.name[0] != '\0' ? desc.name : "outline";
    outline.max_width = round_up_pow2(desc.max_width > 0 ? desc.max_width : 1);
    outline.flood_count = flood_count_for(outline.max_width);
    if (outline.width > static_cast<f32>(outline.max_width)) {
        outline.width = static_cast<f32>(outline.max_width);
    }
    outline.shader = SHADER_LIBRARY::load(renderer, OUTLINE::SHADER_NAME);

    char name[RENDER_GRAPH::NAME_MAX];
    const auto too_long = [&](const char* what) {
        LOG::error(LOG_CATEGORY::RENDER, "outline '%s': the name of its %s does not fit or is taken", prefix, what);
        return false;
    };

    // The two seed buffers the flood ping-pongs between, and the mask's own
    // depth so the nearest front face wins where the silhouette overlaps.
    RenderResourceDesc seeds;
    seeds.aspect = RENDER_ASPECT_COLOR;
    seeds.format = static_cast<RenderFormat>(GPU_FORMAT_RGBA16_UINT);
    for (u32 i = 0; i < 2; ++i) {
        snprintf(name, sizeof(name), "%s_seeds_%c", prefix, i == 0 ? 'a' : 'b');
        outline.seeds[i] = graph.add_resource(name, seeds);
        if (!outline.seeds[i].is_valid()) {
            return too_long("seed buffer");
        }
    }
    RenderResourceDesc mask_depth;
    mask_depth.aspect = RENDER_ASPECT_DEPTH;
    snprintf(name, sizeof(name), "%s_mask_depth", prefix);
    outline.mask_depth = graph.add_resource(name, mask_depth);
    if (!outline.mask_depth.is_valid()) {
        return too_long("mask depth");
    }

    // The mask. Its clear value must be all zeros: that is the texel the
    // seed encoding reads back as "no seed here", and an attachment clears
    // to opaque black by default. Zero is also the one clear value an
    // integer attachment can be given this way, since the graph carries a
    // clear as four floats and only all-zero bits mean the same in both.
    snprintf(name, sizeof(name), "%s_mask", prefix);
    outline.mask = graph.add_pass(name, desc.mask != nullptr ? RENDER_PASS_CUSTOM : RENDER_PASS_DRAW_SCENE);
    if (!outline.mask.is_valid()) {
        return too_long("mask pass");
    }
    static constexpr f32 NO_SEED[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    graph.set_color_attachment(outline.mask, 0, outline.seeds[0], RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
    graph.set_clear_color(outline.mask, 0, NO_SEED);
    graph.set_depth_attachment(outline.mask, outline.mask_depth, RENDER_LOAD_CLEAR, RENDER_STORE_DONT_CARE);
    if (desc.mask != nullptr) {
        graph.set_custom_callback(outline.mask, name);
        snprintf(outline.mask_callback, sizeof(outline.mask_callback), "%s", name);
        RENDERER::register_custom_pass(renderer, outline.mask_callback, desc.mask, desc.mask_user_data);
    } else {
        graph.set_draw_scene_tag(outline.mask, OUTLINE::MASK_TAG_NAME);
    }

    // The flood. Pass i reads the buffer the one before wrote and writes
    // the other, so the chain ends in seeds[flood_count % 2]. Nothing is
    // loaded: every pixel of the target is written.
    for (u32 i = 0; i < outline.flood_count; ++i) {
        snprintf(name, sizeof(name), "%s_flood_%u", prefix, i);
        const RenderPassHandle pass = graph.add_pass(name, RENDER_PASS_FULLSCREEN);
        if (!pass.is_valid()) {
            return too_long("flood pass");
        }
        outline.flood[i] = pass;
        graph.set_fullscreen_shader(pass, outline.shader);
        graph.set_draw_scene_tag(pass, FLOOD_TAG);
        graph.add_input(pass, outline.seeds[i % 2]);
        graph.set_color_attachment(pass, 0, outline.seeds[(i + 1) % 2], RENDER_LOAD_DONT_CARE, RENDER_STORE_STORE);
    }
    const RenderResourceHandle flooded = outline.seeds[outline.flood_count % 2];

    if (desc.behind) {
        snprintf(name, sizeof(name), "%s_behind", prefix);
        outline.resolve_behind = add_resolve(graph, name, RESOLVE_BEHIND_TAG, outline.shader, flooded);
        if (!outline.resolve_behind.is_valid()) {
            return too_long("behind pass");
        }
    }
    outline.resolve = add_resolve(graph, prefix, RESOLVE_TAG, outline.shader, flooded);
    if (!outline.resolve.is_valid()) {
        return too_long("resolve pass");
    }
    OUTLINE::set_enabled(renderer, outline, outline.enabled);
    OUTLINE::update(renderer, outline);

    if (outline.shader == 0) {
        LOG::error(LOG_CATEGORY::RENDER, "outline '%s': render/%s.slang did not load; the passes draw nothing", prefix, OUTLINE::SHADER_NAME);
        return false;
    }
    return true;
}

void OUTLINE::shutdown(Renderer& renderer, Outline& outline) {
    RenderGraph& graph = renderer.graph;
    if (outline.mask_callback[0] != '\0') {
        RENDERER::unregister_custom_pass(renderer, outline.mask_callback);
        outline.mask_callback[0] = '\0';
    }
    graph.remove_pass(outline.resolve);
    graph.remove_pass(outline.resolve_behind);
    for (u32 i = 0; i < outline.flood_count; ++i) {
        graph.remove_pass(outline.flood[i]);
        outline.flood[i] = RenderPassHandle{};
    }
    graph.remove_pass(outline.mask);
    graph.remove_resource(outline.seeds[0]);
    graph.remove_resource(outline.seeds[1]);
    graph.remove_resource(outline.mask_depth);
    outline.resolve = RenderPassHandle{};
    outline.resolve_behind = RenderPassHandle{};
    outline.mask = RenderPassHandle{};
    outline.seeds[0] = RenderResourceHandle{};
    outline.seeds[1] = RenderResourceHandle{};
    outline.mask_depth = RenderResourceHandle{};
    outline.flood_count = 0;
}

void OUTLINE::set_enabled(Renderer& renderer, Outline& outline, const bool enabled) {
    RenderGraph& graph = renderer.graph;
    outline.enabled = enabled;
    graph.set_enabled(outline.mask, enabled);
    for (u32 i = 0; i < outline.flood_count; ++i) {
        graph.set_enabled(outline.flood[i], enabled);
    }
    graph.set_enabled(outline.resolve, enabled);
    if (outline.resolve_behind.is_valid()) {
        graph.set_enabled(outline.resolve_behind, enabled);
    }
}

// --- Update -------------------------------------------------------------------

namespace {

void push_resolve_constants(RenderGraph& graph, const RenderPassHandle pass, const f32 color[4], const f32 width, const f32 softness) {
    if (!pass.is_valid()) {
        return;
    }
    ResolveConstants constants;
    for (u32 i = 0; i < 4; ++i) {
        constants.color[i] = color[i];
    }
    constants.width = width;
    constants.softness = softness;
    graph.set_constants(pass, &constants, sizeof(constants));
}

// The scene depth the resolve passes test against: whatever the forward
// pass draws into, which is kept stored for them (the default graph
// discards it, and the demo's toggles rewrite it).
void follow_scene_depth(RenderGraph& graph, const Outline& outline, const RenderPassHandle forward_handle) {
    const RenderPassDesc* forward = graph.pass(forward_handle);
    if (forward == nullptr || !forward->has_depth) {
        return;
    }
    const RenderResourceHandle depth = forward->depth.resource;
    if (forward->depth.store != RENDER_STORE_STORE) {
        graph.set_depth_attachment(forward_handle, depth, forward->depth.load, RENDER_STORE_STORE);
    }
    const RenderPassHandle resolves[2] = {outline.resolve_behind, outline.resolve};
    for (const RenderPassHandle pass : resolves) {
        const RenderPassDesc* desc = graph.pass(pass);
        if (desc == nullptr) {
            continue;
        }
        // Stored, not discarded: a pass after these (the editor's grid)
        // loads the same depth.
        if (!desc->has_depth || desc->depth.resource != depth || desc->depth.store != RENDER_STORE_STORE) {
            graph.set_depth_attachment(pass, depth, RENDER_LOAD_LOAD, RENDER_STORE_STORE);
        }
    }
}

// Keeps the chain contiguous and in order, ending right before `before` or
// at the end of the frame.
void keep_order(RenderGraph& graph, const Outline& outline, const RenderPassHandle before) {
    RenderPassHandle wanted[OUTLINE_MAX_FLOOD_PASSES + 3];
    const u32 count = wanted_order(outline, wanted);

    usz end = graph.order.count;
    if (before.is_valid()) {
        for (usz i = 0; i < graph.order.count; ++i) {
            if (graph.order[i] == before) {
                end = i;
                break;
            }
        }
    }
    if (end >= count) {
        bool in_place = true;
        for (u32 i = 0; i < count; ++i) {
            if (graph.order[end - count + i] != wanted[i]) {
                in_place = false;
                break;
            }
        }
        if (in_place) {
            return;
        }
    }
    for (u32 i = 0; i < count; ++i) {
        graph.move_pass(wanted[i], before);
    }
}

} // namespace

void OUTLINE::update(Renderer& renderer, Outline& outline, const RenderPassHandle before) {
    RenderGraph& graph = renderer.graph;
    if (!outline.mask.is_valid()) {
        return;
    }
    for (u32 i = 0; i < outline.flood_count; ++i) {
        FloodConstants constants;
        constants.step = static_cast<i32>(outline.max_width >> i);
        graph.set_constants(outline.flood[i], &constants, sizeof(constants));
    }
    const f32 width = outline.width < static_cast<f32>(outline.max_width) ? outline.width : static_cast<f32>(outline.max_width);
    push_resolve_constants(graph, outline.resolve, outline.color, width, outline.softness);
    push_resolve_constants(graph, outline.resolve_behind, outline.behind_color, width, outline.softness);

    follow_scene_depth(graph, outline, renderer.default_forward);
    keep_order(graph, outline, before);
}

// --- Mask drawing -------------------------------------------------------------

bool OUTLINE::draw_mask(Renderer& renderer, const RenderPassContext& ctx, const EntityId shader_entity, const EntityId entity) {
    World* world = renderer.world;
    if (world == nullptr || shader_entity == 0 || entity == 0 || !ctx.in_render_pass) {
        return false;
    }
    const Transform* transform = world->get<Transform>(entity);
    if (transform == nullptr) {
        return false;
    }
    const GpuMesh* mesh = nullptr;
    if (const MeshRenderer* mesh_renderer = world->get<MeshRenderer>(entity)) {
        mesh = GPU_ASSETS::get_mesh(renderer.assets, mesh_renderer->mesh);
    } else if (const PrimitiveRenderer* primitive = world->get<PrimitiveRenderer>(entity)) {
        mesh = GPU_ASSETS::get_primitive(renderer.assets, primitive->shape);
    }
    if (mesh == nullptr) {
        return false;
    }
    const Shader* shader = world->get<Shader>(shader_entity);
    if (shader == nullptr || !shader->program.is_valid()) {
        return false;
    }
    const ShaderPassProgram* pass = shader->program.find_pass(OUTLINE::MASK_TAG);
    if (pass == nullptr) {
        return false;
    }
    const GpuPipeline pipeline = PIPELINES::get(renderer, shader_entity, *shader, OUTLINE::MASK_TAG, ctx.formats, &mesh->layout);
    if (!pipeline.is_valid()) {
        return false;
    }
    // One entity per call rebinds the pipeline and set 0 every time, which
    // is what outlining a handful of objects costs; a mask over many would
    // sort and bind the way RENDER_PASSES::draw_scene does.
    GPU::cmd_bind_pipeline(ctx.cmd, pipeline);
    GPU::cmd_bind_group(ctx.cmd, 0, ctx.frame_group);
    const Matrix4x4 model = transform->matrix();
    if (pass->has_push_constants()) {
        const u32 size = pass->reflection.push_constant_size < sizeof(model) ? pass->reflection.push_constant_size : static_cast<u32>(sizeof(model));
        GPU::cmd_push_constants(ctx.cmd, &model, size);
    }
    for (u32 s = 0; s < mesh->stream_count; ++s) {
        GPU::cmd_bind_vertex_buffer(ctx.cmd, s, mesh->streams[s]);
    }
    GPU::cmd_bind_index_buffer(ctx.cmd, mesh->indices, mesh->index_type);
    for (u32 i = 0; i < mesh->submesh_count; ++i) {
        const SubmeshDesc& submesh = mesh->submeshes[i];
        GPU::cmd_draw_indexed(ctx.cmd, submesh.index_count, submesh.first_index, static_cast<i32>(submesh.base_vertex));
    }
    return true;
}
