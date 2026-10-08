#include "engine/gpu/render_graph_backend.hpp"

#include "engine/gpu/format_utils.hpp"
#include "engine/utils/hash.hpp"
#include "gpu/vk_check.hpp"

#include <cstdio>
#include <utility>

// --- State table ---------------------------------------------------------------

namespace {

struct StateInfo {
    VkImageLayout layout;
    VkPipelineStageFlags stage;
    VkAccessFlags access;
};

StateInfo state_info(const RenderResourceState state) {
    switch (state) {
    case RENDER_STATE_COLOR_ATTACHMENT:
        return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    case RENDER_STATE_DEPTH_ATTACHMENT:
        return {VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    case RENDER_STATE_SAMPLED:
        return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT};
    case RENDER_STATE_TRANSFER_SRC:
        return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    case RENDER_STATE_TRANSFER_DST:
        return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
    case RENDER_STATE_STORAGE:
        return {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    case RENDER_STATE_UNDEFINED:
    case RENDER_STATE_EXTERNAL:
        break;
    }
    return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0};
}

VkAttachmentLoadOp to_vk(const RenderLoadOp op) {
    switch (op) {
    case RENDER_LOAD_CLEAR:
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case RENDER_LOAD_LOAD:
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    case RENDER_LOAD_DONT_CARE:
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

VkAttachmentStoreOp to_vk(const RenderStoreOp op) {
    return op == RENDER_STORE_STORE ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

VkImageUsageFlags usage_bits(const u32 usage) {
    VkImageUsageFlags bits = 0;
    if (usage & RENDER_USAGE_COLOR) {
        bits |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (usage & RENDER_USAGE_DEPTH) {
        bits |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (usage & RENDER_USAGE_SAMPLED) {
        bits |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (usage & RENDER_USAGE_TRANSFER_SRC) {
        bits |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (usage & RENDER_USAGE_TRANSFER_DST) {
        bits |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (usage & RENDER_USAGE_STORAGE) {
        bits |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    return bits;
}

u64 description_hash(const PlannedPass& pass) {
    u64 hash = HASH::fnv1a(&pass.color_count, sizeof(pass.color_count));
    auto fold = [&](const PlannedAttachment& a) {
        const u32 words[6] = {a.format, a.load, a.store, a.initial, a.final, a.previous};
        hash = HASH::fnv1a_append(hash, words, sizeof(words));
    };
    for (u32 i = 0; i < pass.color_count; ++i) {
        fold(pass.color[i]);
    }
    const u32 has_depth = pass.has_depth ? 1 : 0;
    hash = HASH::fnv1a_append(hash, &has_depth, sizeof(has_depth));
    if (pass.has_depth) {
        fold(pass.depth);
    }
    return hash;
}

// Every attachment of a pass, colors then depth.
u32 gather_attachments(const PlannedPass& pass, const PlannedAttachment** out) {
    u32 count = 0;
    for (u32 i = 0; i < pass.color_count; ++i) {
        out[count++] = &pass.color[i];
    }
    if (pass.has_depth) {
        out[count++] = &pass.depth;
    }
    return count;
}

} // namespace

// --- Lifetime ------------------------------------------------------------------

bool RenderGraphBackend::init(GpuDevice* gpu, GpuResourceManager* resources, SamplerCache* samplers) {
    this->gpu = gpu;
    this->resources = resources;

    // Pass inputs are read whole and unscaled most of the time, and transient
    // images have a single level, so linear and clamped is the right default.
    SamplerDesc desc;
    desc.mag_filter = VK_FILTER_LINEAR;
    desc.min_filter = VK_FILTER_LINEAR;
    desc.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    desc.address_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    desc.address_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    desc.address_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    this->input_sampler = samplers != nullptr ? samplers->get(desc) : VK_NULL_HANDLE;
    return this->input_sampler != VK_NULL_HANDLE;
}

void RenderGraphBackend::shutdown() {
    if (this->gpu == nullptr) {
        return;
    }
    VkDevice device = this->gpu->device;
    for (RenderPassInstance& instance : this->instances) {
        if (instance.borrowed) {
            continue;
        }
        if (instance.framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, instance.framebuffer, nullptr);
        }
        if (instance.render_pass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, instance.render_pass, nullptr);
        }
    }
    this->instances.free();
    for (RenderGraphImage& image : this->images) {
        if (image.texture.is_valid()) {
            this->resources->destroy_texture(image.texture);
        }
    }
    this->images.free();
    this->keys.free();
    this->input_sampler = VK_NULL_HANDLE;
    this->gpu = nullptr;
    this->resources = nullptr;
}

// --- Realize -------------------------------------------------------------------

bool RenderGraphBackend::create_image(const PlannedResource& resource, RenderGraphImage& out) {
    GpuTextureDesc desc;
    desc.format = static_cast<VkFormat>(resource.format);
    desc.width = resource.width;
    desc.height = resource.height;
    desc.mip_levels = 1;
    desc.usage = usage_bits(resource.usage);
    GpuTexture texture;
    if (!this->resources->create_texture(desc, texture)) {
        fprintf(stderr, "[render_graph] cannot create image for resource %llu\n", static_cast<unsigned long long>(resource.handle.id));
        return false;
    }
    out.handle = resource.handle;
    out.texture = texture;
    out.format = resource.format;
    out.width = resource.width;
    out.height = resource.height;
    out.usage = resource.usage;
    out.state = RENDER_STATE_UNDEFINED;
    return true;
}

VkRenderPass RenderGraphBackend::create_render_pass(const PlannedPass& pass) {
    const PlannedAttachment* planned[RENDER_GRAPH::MAX_COLOR_ATTACHMENTS + 1];
    const u32 count = gather_attachments(pass, planned);

    VkAttachmentDescription attachments[RENDER_GRAPH::MAX_COLOR_ATTACHMENTS + 1] = {};
    VkAttachmentReference color_refs[RENDER_GRAPH::MAX_COLOR_ATTACHMENTS] = {};
    VkAttachmentReference depth_ref{};

    // Before: whatever last touched each image (the previous use in this
    // frame, or the previous frame's) and this pass's own writes last frame,
    // since frames in flight share the images.
    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    // After: make the writes visible to the next use of each image.
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstStageMask = 0;
    dependencies[1].dstAccessMask = 0;

    for (u32 i = 0; i < count; ++i) {
        const PlannedAttachment& a = *planned[i];
        const bool is_depth = pass.has_depth && planned[i] == &pass.depth;
        VkAttachmentDescription& description = attachments[i];
        description.format = static_cast<VkFormat>(a.format);
        description.samples = VK_SAMPLE_COUNT_1_BIT;
        description.loadOp = to_vk(a.load);
        description.storeOp = to_vk(a.store);
        description.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        description.initialLayout = state_info(a.initial).layout;
        description.finalLayout = state_info(a.final).layout;

        const StateInfo previous = state_info(a.previous);
        dependencies[0].srcStageMask |= previous.stage;
        dependencies[0].srcAccessMask |= previous.access;
        const StateInfo final = state_info(a.final);
        dependencies[1].dstStageMask |= final.stage;
        dependencies[1].dstAccessMask |= final.access;

        if (is_depth) {
            depth_ref.attachment = i;
            depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        } else {
            color_refs[i].attachment = i;
            color_refs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }
    if (dependencies[1].dstStageMask == 0) {
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = pass.color_count;
    subpass.pColorAttachments = color_refs;
    subpass.pDepthStencilAttachment = pass.has_depth ? &depth_ref : nullptr;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = count;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;

    VkRenderPass render_pass = VK_NULL_HANDLE;
    if (!vk_check(vkCreateRenderPass(this->gpu->device, &info, nullptr, &render_pass), "vkCreateRenderPass (graph)")) {
        return VK_NULL_HANDLE;
    }
    return render_pass;
}

VkFramebuffer RenderGraphBackend::create_framebuffer(const PlannedPass& pass, const VkRenderPass render_pass) {
    const PlannedAttachment* planned[RENDER_GRAPH::MAX_COLOR_ATTACHMENTS + 1];
    const u32 count = gather_attachments(pass, planned);
    VkImageView views[RENDER_GRAPH::MAX_COLOR_ATTACHMENTS + 1];
    for (u32 i = 0; i < count; ++i) {
        const RenderGraphImage& image = this->images[planned[i]->resource];
        if (!image.texture.is_valid()) {
            fprintf(stderr, "[render_graph] pass attachment has no image\n");
            return VK_NULL_HANDLE;
        }
        views[i] = image.texture.view;
    }

    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = render_pass;
    info.attachmentCount = count;
    info.pAttachments = views;
    info.width = pass.width;
    info.height = pass.height;
    info.layers = 1;

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (!vk_check(vkCreateFramebuffer(this->gpu->device, &info, nullptr, &framebuffer), "vkCreateFramebuffer (graph)")) {
        return VK_NULL_HANDLE;
    }
    return framebuffer;
}

void RenderGraphBackend::release_image(RenderGraphImage& image) {
    if (image.texture.is_valid()) {
        this->resources->release_texture(image.texture);
    }
    image = RenderGraphImage{};
}

void RenderGraphBackend::release_instance(RenderPassInstance& instance) {
    if (!instance.borrowed) {
        this->resources->release_framebuffer(instance.framebuffer);
        this->resources->release_render_pass(instance.render_pass);
    }
    instance = RenderPassInstance{};
}

bool RenderGraphBackend::realize(const RenderGraphPlan& plan) {
    bool ok = true;

    // Images: keep the ones whose resource still wants the same thing.
    DynamicArray<RenderGraphImage> old_images = std::move(this->images);
    this->images = DynamicArray<RenderGraphImage>();
    this->images.reserve(plan.resources.count);
    for (const PlannedResource& resource : plan.resources) {
        RenderGraphImage image;
        image.handle = resource.handle;
        if (resource.kind == RENDER_RESOURCE_TRANSIENT && resource.used) {
            bool reused = false;
            for (RenderGraphImage& old : old_images) {
                if (old.handle == resource.handle && old.texture.is_valid() && old.format == resource.format &&
                    old.width == resource.width && old.height == resource.height && old.usage == resource.usage) {
                    image = old;
                    old = RenderGraphImage{};
                    reused = true;
                    break;
                }
            }
            if (!reused && !this->create_image(resource, image)) {
                ok = false;
            }
        }
        this->images.push(image);
    }
    for (RenderGraphImage& old : old_images) {
        this->release_image(old);
    }
    old_images.free();

    // Render passes and framebuffers. A render pass is reused when its
    // attachment descriptions are unchanged; framebuffers are rebuilt, since
    // the views may be new and they are cheap.
    DynamicArray<RenderPassInstance> old_instances = std::move(this->instances);
    this->instances = DynamicArray<RenderPassInstance>();
    this->instances.reserve(plan.passes.count);
    for (const PlannedPass& pass : plan.passes) {
        RenderPassInstance instance;
        instance.handle = pass.handle;
        instance.compat_key = pass.compat_key;
        instance.extent = {pass.width, pass.height};
        instance.attachment_count = pass.color_count + (pass.has_depth ? 1 : 0);
        if (pass.is_raster && pass.writes_backbuffer) {
            instance.borrowed = true;
        } else if (pass.is_raster && ok) {
            instance.description_hash = description_hash(pass);
            for (RenderPassInstance& old : old_instances) {
                if (old.handle == pass.handle && !old.borrowed && old.render_pass != VK_NULL_HANDLE && old.description_hash == instance.description_hash) {
                    instance.render_pass = old.render_pass;
                    old.render_pass = VK_NULL_HANDLE;
                    break;
                }
            }
            if (instance.render_pass == VK_NULL_HANDLE) {
                instance.render_pass = this->create_render_pass(pass);
            }
            if (instance.render_pass != VK_NULL_HANDLE) {
                instance.framebuffer = this->create_framebuffer(pass, instance.render_pass);
            }
            if (instance.render_pass == VK_NULL_HANDLE || instance.framebuffer == VK_NULL_HANDLE) {
                ok = false;
            }
        }
        this->instances.push(instance);
    }
    for (RenderPassInstance& old : old_instances) {
        this->release_instance(old);
    }
    old_instances.free();

    // Compatibility keys. Owned render passes win over the borrowed one,
    // which is only known once execute() has seen a frame.
    this->keys.clear();
    for (const RenderPassInstance& instance : this->instances) {
        if (instance.compat_key == 0) {
            continue;
        }
        CompatEntry* entry = this->keys.find(instance.compat_key);
        if (entry == nullptr) {
            CompatEntry fresh;
            fresh.render_pass = instance.render_pass;
            fresh.color_count = instance.attachment_count - (instance.borrowed ? 1 : 0);
            // The borrowed pass always has color + depth in its target.
            if (instance.borrowed) {
                fresh.color_count = 1;
            } else {
                u32 color_count = 0;
                for (const PlannedPass& pass : plan.passes) {
                    if (pass.handle == instance.handle) {
                        color_count = pass.color_count;
                    }
                }
                fresh.color_count = color_count;
            }
            this->keys.insert(instance.compat_key, fresh);
        } else if (entry->render_pass == VK_NULL_HANDLE) {
            entry->render_pass = instance.render_pass;
        }
    }
    return ok;
}

VkRenderPass RenderGraphBackend::render_pass_for_key(const u64 key) const {
    const CompatEntry* entry = this->keys.find(key);
    return entry != nullptr ? entry->render_pass : VK_NULL_HANDLE;
}

u32 RenderGraphBackend::color_count_for_key(const u64 key) const {
    const CompatEntry* entry = this->keys.find(key);
    return entry != nullptr ? entry->color_count : 0;
}

// --- Execute -------------------------------------------------------------------

void RenderGraphBackend::transition(const VkCommandBuffer cmd, RenderGraphImage& image, const RenderResourceState state) {
    if (!image.texture.is_valid() || image.state == state) {
        return;
    }
    const StateInfo from = state_info(image.state);
    const StateInfo to = state_info(state);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = from.access;
    barrier.dstAccessMask = to.access;
    barrier.oldLayout = from.layout;
    barrier.newLayout = to.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.texture.image;
    barrier.subresourceRange.aspectMask = GPU_FORMAT::aspect(image.texture.format);
    barrier.subresourceRange.levelCount = image.texture.mip_levels;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, from.stage, to.stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    image.state = state;
    image.texture.layout = to.layout;
}

void RenderGraphBackend::record_clear(const VkCommandBuffer cmd, const PlannedPass& pass, const RenderPassDesc& desc) {
    RenderGraphImage& image = this->images[pass.clear_target];
    if (!image.texture.is_valid()) {
        return;
    }
    VkImageSubresourceRange range{};
    range.aspectMask = GPU_FORMAT::aspect(image.texture.format);
    range.levelCount = image.texture.mip_levels;
    range.layerCount = 1;
    if (range.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
        VkClearDepthStencilValue value{};
        value.depth = desc.clear_depth;
        value.stencil = desc.clear_stencil;
        vkCmdClearDepthStencilImage(cmd, image.texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
    } else {
        VkClearColorValue value{};
        for (u32 i = 0; i < 4; ++i) {
            value.float32[i] = desc.clear_color[i];
        }
        vkCmdClearColorImage(cmd, image.texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
    }
}

void RenderGraphBackend::record_blit(const VkCommandBuffer cmd, const PlannedPass& pass, const RenderPassDesc& desc) {
    RenderGraphImage& src = this->images[pass.blit_src];
    RenderGraphImage& dst = this->images[pass.blit_dst];
    if (!src.texture.is_valid() || !dst.texture.is_valid()) {
        return;
    }
    VkImageBlit region{};
    region.srcSubresource.aspectMask = GPU_FORMAT::aspect(src.texture.format);
    region.srcSubresource.layerCount = 1;
    region.srcOffsets[1] = {static_cast<i32>(src.texture.extent.width), static_cast<i32>(src.texture.extent.height), 1};
    region.dstSubresource.aspectMask = GPU_FORMAT::aspect(dst.texture.format);
    region.dstSubresource.layerCount = 1;
    region.dstOffsets[1] = {static_cast<i32>(dst.texture.extent.width), static_cast<i32>(dst.texture.extent.height), 1};
    // Depth images may only be blitted with NEAREST.
    const bool depth = (region.srcSubresource.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    vkCmdBlitImage(cmd, src.texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &region, desc.blit_linear && !depth ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

void RenderGraphBackend::execute(const FrameContext& frame, const RenderGraphPlan& plan, const RenderGraph& graph, RenderPassExecutor& executor) {
    if (plan.passes.count != this->instances.count) {
        fprintf(stderr, "[render_graph] execute: the plan was not realized\n");
        return;
    }
    const VkCommandBuffer cmd = frame.cmd;

    // The backbuffer pass takes this frame's target.
    if (plan.backbuffer_pass < this->instances.count) {
        RenderPassInstance& borrowed = this->instances[plan.backbuffer_pass];
        borrowed.render_pass = frame.target.render_pass;
        borrowed.framebuffer = frame.target.framebuffer;
        borrowed.extent = frame.target.extent;
        borrowed.attachment_count = RENDER_TARGET_ATTACHMENT_COUNT;
        if (CompatEntry* entry = this->keys.find(borrowed.compat_key)) {
            if (entry->render_pass == VK_NULL_HANDLE) {
                entry->render_pass = frame.target.render_pass;
            }
        }
    }

    for (usz p = 0; p < plan.passes.count; ++p) {
        const PlannedPass& pass = plan.passes[p];
        RenderPassInstance& instance = this->instances[p];
        const RenderPassDesc* desc = graph.pass(pass.handle);
        if (desc == nullptr) {
            continue;
        }

        for (u32 i = 0; i < pass.before_count; ++i) {
            this->transition(cmd, this->images[pass.before[i].resource], pass.before[i].state);
        }

        RenderPassContext ctx;
        ctx.cmd = cmd;
        ctx.slot = frame.slot;
        ctx.frame_index = frame.frame_index;
        ctx.desc = desc;
        ctx.pass = &pass;
        ctx.compat_key = pass.compat_key;
        ctx.color_count = pass.writes_backbuffer ? 1 : pass.color_count;
        ctx.input_sampler = this->input_sampler;
        for (u32 i = 0; i < pass.input_count; ++i) {
            ctx.input_views[ctx.input_count++] = this->images[pass.inputs[i]].texture.view;
        }

        if (!pass.is_raster) {
            switch (pass.kind) {
            case RENDER_PASS_CLEAR:
                this->record_clear(cmd, pass, *desc);
                break;
            case RENDER_PASS_BLIT:
                this->record_blit(cmd, pass, *desc);
                break;
            case RENDER_PASS_CUSTOM:
                executor.custom(ctx);
                break;
            default:
                break;
            }
            continue;
        }
        if (instance.render_pass == VK_NULL_HANDLE || instance.framebuffer == VK_NULL_HANDLE) {
            continue;
        }

        // Attachments that load need their image in the planned state; the
        // first frame after creation gets there with a barrier here.
        const PlannedAttachment* attachments[RENDER_GRAPH::MAX_COLOR_ATTACHMENTS + 1];
        const u32 attachment_count = gather_attachments(pass, attachments);
        if (!instance.borrowed) {
            for (u32 i = 0; i < attachment_count; ++i) {
                if (attachments[i]->load == RENDER_LOAD_LOAD) {
                    this->transition(cmd, this->images[attachments[i]->resource], attachments[i]->initial);
                }
            }
        }

        VkClearValue clears[RENDER_GRAPH::MAX_COLOR_ATTACHMENTS + 1] = {};
        u32 clear_count = 0;
        for (u32 i = 0; i < desc->color_count && i < pass.color_count; ++i) {
            for (u32 c = 0; c < 4; ++c) {
                clears[clear_count].color.float32[c] = desc->color[i].clear[c];
            }
            clear_count += 1;
        }
        if (pass.has_depth) {
            clears[clear_count].depthStencil.depth = desc->depth.clear[0];
            clears[clear_count].depthStencil.stencil = desc->depth.clear_stencil;
            clear_count += 1;
        } else if (instance.borrowed) {
            // The target's render pass always clears its depth attachment.
            clears[clear_count].depthStencil.depth = 1.0f;
            clear_count += 1;
        }

        VkRenderPassBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass = instance.render_pass;
        begin.framebuffer = instance.framebuffer;
        begin.renderArea.extent = instance.extent;
        begin.clearValueCount = clear_count;
        begin.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.width = static_cast<f32>(instance.extent.width);
        viewport.height = static_cast<f32>(instance.extent.height);
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{};
        scissor.extent = instance.extent;
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        ctx.render_pass = instance.render_pass;
        ctx.extent = instance.extent;
        switch (pass.kind) {
        case RENDER_PASS_DRAW_SCENE:
            executor.draw_scene(ctx);
            break;
        case RENDER_PASS_FULLSCREEN:
            executor.fullscreen(ctx);
            break;
        case RENDER_PASS_CUSTOM:
            executor.custom(ctx);
            break;
        default:
            break;
        }

        vkCmdEndRenderPass(cmd);

        if (!instance.borrowed) {
            for (u32 i = 0; i < attachment_count; ++i) {
                RenderGraphImage& image = this->images[attachments[i]->resource];
                image.state = attachments[i]->final;
                image.texture.layout = state_info(image.state).layout;
            }
        }
    }
}
