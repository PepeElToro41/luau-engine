#include "engine/render/graph_executor.hpp"

#include "engine/render/renderer.hpp"
#include "render/passes.hpp"

#include <utility>

// --- Helpers -------------------------------------------------------------------

static u32 texture_usage(const u32 usage) {
    u32 bits = 0;
    if (usage & RENDER_USAGE_COLOR) {
        bits |= GPU_TEXTURE_USAGE_COLOR_ATTACHMENT;
    }
    if (usage & RENDER_USAGE_DEPTH) {
        bits |= GPU_TEXTURE_USAGE_DEPTH_ATTACHMENT;
    }
    if (usage & RENDER_USAGE_SAMPLED) {
        bits |= GPU_TEXTURE_USAGE_SAMPLED;
    }
    if (usage & RENDER_USAGE_TRANSFER_SRC) {
        bits |= GPU_TEXTURE_USAGE_TRANSFER_SRC;
    }
    if (usage & RENDER_USAGE_TRANSFER_DST) {
        bits |= GPU_TEXTURE_USAGE_TRANSFER_DST;
    }
    if (usage & RENDER_USAGE_STORAGE) {
        bits |= GPU_TEXTURE_USAGE_STORAGE;
    }
    return bits;
}

static GpuResourceState to_gpu(const RenderResourceState state) {
    switch (state) {
    case RENDER_STATE_COLOR_ATTACHMENT:
        return GPU_STATE_COLOR_ATTACHMENT;
    case RENDER_STATE_DEPTH_ATTACHMENT:
        return GPU_STATE_DEPTH_ATTACHMENT;
    case RENDER_STATE_SAMPLED:
        return GPU_STATE_SAMPLED;
    case RENDER_STATE_TRANSFER_SRC:
        return GPU_STATE_TRANSFER_SRC;
    case RENDER_STATE_TRANSFER_DST:
        return GPU_STATE_TRANSFER_DST;
    case RENDER_STATE_STORAGE:
        return GPU_STATE_STORAGE;
    case RENDER_STATE_EXTERNAL:
        return GPU_STATE_PRESENT;
    case RENDER_STATE_UNDEFINED:
        break;
    }
    return GPU_STATE_UNDEFINED;
}

static GpuLoadOp to_gpu(const RenderLoadOp op) {
    switch (op) {
    case RENDER_LOAD_CLEAR:
        return GPU_LOAD_CLEAR;
    case RENDER_LOAD_LOAD:
        return GPU_LOAD_LOAD;
    case RENDER_LOAD_DONT_CARE:
        break;
    }
    return GPU_LOAD_DONT_CARE;
}

static GpuStoreOp to_gpu(const RenderStoreOp op) {
    return op == RENDER_STORE_STORE ? GPU_STORE_STORE : GPU_STORE_DONT_CARE;
}

// Moves the image to `state`, always: the barrier also orders this use
// after the previous one when the state does not change.
static void transition(const FrameContext& frame, GraphImage& image, const GpuResourceState state) {
    if (!image.texture.is_valid() || state == GPU_STATE_UNDEFINED) {
        return;
    }
    GPU::cmd_barrier(frame.frame.cmd, image.texture, image.state, state);
    image.state = state;
}

// --- Realize -------------------------------------------------------------------

static bool create_image(Renderer& renderer, const PlannedResource& resource, GraphImage& out) {
    GpuTextureDesc desc;
    desc.format = static_cast<GpuFormat>(resource.format);
    desc.width = resource.width;
    desc.height = resource.height;
    desc.mip_levels = 1;
    desc.usage = texture_usage(resource.usage);
    out.texture = GPU::create_texture(renderer.gpu, desc);
    if (!out.texture.is_valid()) {
        RENDERER::log(renderer, RENDER_LOG_ERROR, "render graph: cannot create the image of resource %llu", static_cast<unsigned long long>(resource.handle.id));
        return false;
    }
    out.handle = resource.handle;
    out.format = resource.format;
    out.width = resource.width;
    out.height = resource.height;
    out.usage = resource.usage;
    out.state = GPU_STATE_UNDEFINED;
    out.external = false;
    return true;
}

bool GRAPH_EXECUTOR::realize(Renderer& renderer, const RenderGraphPlan& plan) {
    GraphExecutor& executor = renderer.executor;
    bool ok = true;

    // Keep the images whose resource still wants the same thing.
    DynamicArray<GraphImage> old_images = std::move(executor.images);
    executor.images = DynamicArray<GraphImage>();
    executor.images.reserve(plan.resources.count);
    for (const PlannedResource& resource : plan.resources) {
        GraphImage image;
        image.handle = resource.handle;
        if (resource.kind == RENDER_RESOURCE_BACKBUFFER) {
            image.external = true;
        } else if (resource.used) {
            bool reused = false;
            for (GraphImage& old : old_images) {
                if (old.handle == resource.handle && old.texture.is_valid() && old.format == resource.format && old.width == resource.width &&
                    old.height == resource.height && old.usage == resource.usage) {
                    image = old;
                    old = GraphImage{};
                    reused = true;
                    break;
                }
            }
            if (!reused && !create_image(renderer, resource, image)) {
                ok = false;
            }
        }
        executor.images.push(image);
    }
    for (GraphImage& old : old_images) {
        if (!old.external && old.texture.is_valid()) {
            GPU::release_texture(renderer.gpu, old.texture);
        }
    }
    old_images.free();
    return ok;
}

void GRAPH_EXECUTOR::free(Renderer& renderer) {
    for (GraphImage& image : renderer.executor.images) {
        if (!image.external && image.texture.is_valid()) {
            GPU::destroy_texture(renderer.gpu, image.texture);
        }
    }
    renderer.executor.images.free();
}

// --- Execute -------------------------------------------------------------------

GpuResourceState GRAPH_EXECUTOR::execute(Renderer& renderer, const FrameContext& frame, const RenderGraphPlan& plan) {
    GraphExecutor& executor = renderer.executor;
    if (plan.resources.count != executor.images.count) {
        RENDERER::log(renderer, RENDER_LOG_ERROR, "render graph: execute: the plan was not realized");
        return frame.target_state;
    }

    // The backbuffer resource is this frame's target.
    GraphImage* backbuffer = nullptr;
    for (GraphImage& image : executor.images) {
        if (image.external) {
            image.texture = frame.target;
            image.state = frame.target_state;
            image.width = frame.target.width;
            image.height = frame.target.height;
            backbuffer = &image;
        }
    }

    for (const PlannedPass& pass : plan.passes) {
        const RenderPassDesc* desc = renderer.graph.pass(pass.handle);
        if (desc == nullptr) {
            continue;
        }
        for (u32 i = 0; i < pass.before_count; ++i) {
            transition(frame, executor.images[pass.before[i].resource], to_gpu(pass.before[i].state));
        }

        RenderPassContext ctx;
        ctx.cmd = frame.frame.cmd;
        ctx.slot = frame.frame.slot;
        ctx.frame_index = frame.frame.frame_index;
        ctx.desc = desc;
        ctx.pass = &pass;
        ctx.frame_group = renderer.frame_group;
        for (u32 i = 0; i < pass.input_count; ++i) {
            ctx.inputs[ctx.input_count++] = executor.images[pass.inputs[i]].texture;
        }

        if (!pass.is_raster) {
            switch (pass.kind) {
            case RENDER_PASS_CLEAR: {
                if (pass.clear_target == RENDER_GRAPH::NO_RESOURCE) {
                    break;
                }
                GraphImage& image = executor.images[pass.clear_target];
                transition(frame, image, GPU_STATE_TRANSFER_DST);
                if (!image.texture.is_valid()) {
                    break;
                }
                if (GPU_FORMAT::is_depth(image.texture.format)) {
                    GPU::cmd_clear_depth(ctx.cmd, image.texture, desc->clear_depth, desc->clear_stencil);
                } else {
                    GPU::cmd_clear_color(ctx.cmd, image.texture, desc->clear_color);
                }
                break;
            }
            case RENDER_PASS_BLIT: {
                if (pass.blit_src == RENDER_GRAPH::NO_RESOURCE || pass.blit_dst == RENDER_GRAPH::NO_RESOURCE) {
                    break;
                }
                GraphImage& src = executor.images[pass.blit_src];
                GraphImage& dst = executor.images[pass.blit_dst];
                transition(frame, src, GPU_STATE_TRANSFER_SRC);
                transition(frame, dst, GPU_STATE_TRANSFER_DST);
                if (src.texture.is_valid() && dst.texture.is_valid()) {
                    GPU::cmd_blit(ctx.cmd, src.texture, dst.texture, desc->blit_linear);
                }
                break;
            }
            case RENDER_PASS_CUSTOM:
                RENDER_PASSES::custom(renderer, ctx);
                break;
            default:
                break;
            }
            continue;
        }

        // Attachments enter the pass in their attachment state.
        GpuRenderPassDesc begin;
        bool attachments_ok = true;
        for (u32 i = 0; i < pass.color_count; ++i) {
            GraphImage& image = executor.images[pass.color[i].resource];
            transition(frame, image, GPU_STATE_COLOR_ATTACHMENT);
            attachments_ok &= image.texture.is_valid();
            begin.color[i].texture = image.texture;
            begin.color[i].load = to_gpu(pass.color[i].load);
            begin.color[i].store = to_gpu(pass.color[i].store);
            for (u32 c = 0; c < 4 && i < desc->color_count; ++c) {
                begin.color[i].clear[c] = desc->color[i].clear[c];
            }
        }
        begin.color_count = pass.color_count;
        if (pass.has_depth) {
            GraphImage& image = executor.images[pass.depth.resource];
            transition(frame, image, GPU_STATE_DEPTH_ATTACHMENT);
            attachments_ok &= image.texture.is_valid();
            begin.depth.texture = image.texture;
            begin.depth.load = to_gpu(pass.depth.load);
            begin.depth.store = to_gpu(pass.depth.store);
            begin.depth.clear_depth = desc->depth.clear[0];
            begin.depth.clear_stencil = desc->depth.clear_stencil;
            begin.has_depth = true;
        }
        if (!attachments_ok) {
            continue;
        }
        begin.width = pass.width;
        begin.height = pass.height;
        begin.name = desc->name;

        ctx.formats = begin.formats();
        ctx.width = pass.width;
        ctx.height = pass.height;
        ctx.in_render_pass = true;
        GPU::cmd_begin_label(ctx.cmd, desc->name);
        GPU::cmd_begin_render_pass(ctx.cmd, begin);
        switch (pass.kind) {
        case RENDER_PASS_DRAW_SCENE:
            RENDER_PASSES::draw_scene(renderer, ctx);
            break;
        case RENDER_PASS_FULLSCREEN:
            RENDER_PASSES::fullscreen(renderer, ctx);
            break;
        case RENDER_PASS_CUSTOM:
            RENDER_PASSES::custom(renderer, ctx);
            break;
        default:
            break;
        }
        GPU::cmd_end_render_pass(ctx.cmd);
        GPU::cmd_end_label(ctx.cmd);
    }

    return backbuffer != nullptr ? backbuffer->state : frame.target_state;
}
