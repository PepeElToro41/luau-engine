#include "engine/utils/hash.hpp"
#include "gpu/vulkan/vk_check.hpp"
#include "gpu/vulkan/vk_context.hpp"

// --- Render passes ----------------------------------------------------------------

static u64 pass_key(const GpuRenderPassDesc& desc) {
    u64 hash = desc.formats().hash();
    for (u32 i = 0; i < desc.color_count; ++i) {
        const u32 ops[2] = {desc.color[i].load, desc.color[i].store};
        hash = HASH::fnv1a_append(hash, ops, sizeof(ops));
    }
    if (desc.has_depth) {
        const u32 ops[2] = {desc.depth.load, desc.depth.store};
        hash = HASH::fnv1a_append(hash, ops, sizeof(ops));
    }
    return hash;
}

// The two external dependencies make the previous use of the attachments
// (last frame's pass on the same images, a pass that sampled them, a
// transfer) visible to this pass and this pass's writes visible to whatever
// comes next, whatever the explicit barriers already covered.
void VK_RENDER_PASS::standard_dependencies(VkSubpassDependency out[2]) {
    constexpr VkPipelineStageFlags attachment_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    constexpr VkAccessFlags attachment_access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    constexpr VkPipelineStageFlags other_stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    constexpr VkAccessFlags other_access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;

    out[0] = VkSubpassDependency{};
    out[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    out[0].dstSubpass = 0;
    out[0].srcStageMask = attachment_stages | other_stages;
    out[0].srcAccessMask = attachment_access | other_access;
    out[0].dstStageMask = attachment_stages;
    out[0].dstAccessMask = attachment_access;
    out[1] = VkSubpassDependency{};
    out[1].srcSubpass = 0;
    out[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    out[1].srcStageMask = attachment_stages;
    out[1].srcAccessMask = attachment_access;
    out[1].dstStageMask = attachment_stages | other_stages;
    out[1].dstAccessMask = attachment_access | other_access;
}

// Attachments enter and leave in their attachment layout; the frontend's
// barriers do every transition.
static VkRenderPass create_render_pass(VulkanContext& vk, const GpuRenderPassDesc& desc, u32* attachment_count) {
    VkAttachmentDescription attachments[GPU_MAX_COLOR_ATTACHMENTS + 1] = {};
    VkAttachmentReference color_refs[GPU_MAX_COLOR_ATTACHMENTS] = {};
    VkAttachmentReference depth_ref{};
    u32 count = 0;
    for (u32 i = 0; i < desc.color_count; ++i) {
        VkAttachmentDescription& a = attachments[count];
        a.format = VK_FORMATS::to_vk(desc.color[i].texture.format);
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_FORMATS::to_vk(desc.color[i].load);
        a.storeOp = VK_FORMATS::to_vk(desc.color[i].store);
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_refs[i].attachment = count;
        color_refs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        count += 1;
    }
    if (desc.has_depth) {
        VkAttachmentDescription& a = attachments[count];
        a.format = VK_FORMATS::to_vk(desc.depth.texture.format);
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_FORMATS::to_vk(desc.depth.load);
        a.storeOp = VK_FORMATS::to_vk(desc.depth.store);
        a.stencilLoadOp = GPU_FORMAT::has_stencil(desc.depth.texture.format) ? a.loadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = GPU_FORMAT::has_stencil(desc.depth.texture.format) ? a.storeOp : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_ref.attachment = count;
        depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        count += 1;
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = desc.color_count;
    subpass.pColorAttachments = color_refs;
    subpass.pDepthStencilAttachment = desc.has_depth ? &depth_ref : nullptr;

    VkSubpassDependency dependencies[2];
    VK_RENDER_PASS::standard_dependencies(dependencies);

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = count;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;

    VkRenderPass render_pass = VK_NULL_HANDLE;
    if (!vk_check(vkCreateRenderPass(vk.device, &info, nullptr, &render_pass), "vkCreateRenderPass")) {
        return VK_NULL_HANDLE;
    }
    *attachment_count = count;
    return render_pass;
}

static VkRenderPass get_render_pass(VulkanContext& vk, const GpuRenderPassDesc& desc) {
    const u64 key = pass_key(desc);
    if (const VkRenderPassEntry* found = vk.render_passes.find(key)) {
        return found->render_pass;
    }
    VkRenderPassEntry entry;
    entry.render_pass = create_render_pass(vk, desc, &entry.attachment_count);
    if (entry.render_pass == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    vk.render_passes.insert(key, entry);
    return entry.render_pass;
}

// --- Framebuffers -------------------------------------------------------------------

static VkFramebuffer get_framebuffer(VulkanContext& vk, const VkRenderPass render_pass, const SparseId* textures, const VkImageView* views, const u32 count, const u32 width, const u32 height) {
    const u64 pass_bits = reinterpret_cast<u64>(render_pass);
    u64 key = HASH::fnv1a(&pass_bits, sizeof(pass_bits));
    key = HASH::fnv1a_append(key, textures, count * sizeof(SparseId));
    const u32 extent[2] = {width, height};
    key = HASH::fnv1a_append(key, extent, sizeof(extent));
    if (const VkFramebufferEntry* found = vk.framebuffers.find(key)) {
        return found->framebuffer;
    }

    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = render_pass;
    info.attachmentCount = count;
    info.pAttachments = views;
    info.width = width;
    info.height = height;
    info.layers = 1;
    VkFramebufferEntry entry;
    if (!vk_check(vkCreateFramebuffer(vk.device, &info, nullptr, &entry.framebuffer), "vkCreateFramebuffer")) {
        return VK_NULL_HANDLE;
    }
    entry.render_pass = render_pass;
    for (u32 i = 0; i < count; ++i) {
        entry.textures[i] = textures[i];
    }
    entry.texture_count = count;
    entry.width = width;
    entry.height = height;
    vk.framebuffers.insert(key, entry);
    return entry.framebuffer;
}

void VK_RENDER_PASS::shutdown(VulkanContext& vk) {
    for (const auto& entry : vk.framebuffers) {
        vkDestroyFramebuffer(vk.device, entry.value.framebuffer, nullptr);
    }
    vk.framebuffers.free();
    for (const auto& entry : vk.render_passes) {
        vkDestroyRenderPass(vk.device, entry.value.render_pass, nullptr);
    }
    vk.render_passes.free();
}

// --- Commands -------------------------------------------------------------------------

static void vk_cmd_begin_render_pass(const GpuCommandList cmd, const GpuRenderPassDesc& desc) {
    VkCommandListEntry* list = nullptr;
    VulkanContext& vk = VK_CONTEXT::of_command_list(cmd, &list);
    if (list == nullptr) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] begin_render_pass: invalid command list");
        return;
    }
    const char* name = desc.name != nullptr ? desc.name : "render pass";
    if (desc.color_count > GPU_MAX_COLOR_ATTACHMENTS || (desc.color_count == 0 && !desc.has_depth)) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] begin_render_pass (%s): needs 1..%u color attachments or a depth attachment", name, GPU_MAX_COLOR_ATTACHMENTS);
        return;
    }

    SparseId texture_ids[GPU_MAX_COLOR_ATTACHMENTS + 1];
    VkImageView views[GPU_MAX_COLOR_ATTACHMENTS + 1];
    VkClearValue clears[GPU_MAX_COLOR_ATTACHMENTS + 1] = {};
    u32 count = 0;
    u32 width = desc.width;
    u32 height = desc.height;
    for (u32 i = 0; i < desc.color_count; ++i) {
        const VkTextureEntry* texture = VK_CONTEXT::texture(vk, desc.color[i].texture.id);
        if (texture == nullptr) {
            GPU::log(GPU::LOG_ERROR, "[vulkan] begin_render_pass (%s): color attachment %u is not a valid texture", name, i);
            return;
        }
        texture_ids[count] = desc.color[i].texture.id;
        views[count] = texture->view;
        for (u32 c = 0; c < 4; ++c) {
            clears[count].color.float32[c] = desc.color[i].clear[c];
        }
        if (width == 0) {
            width = texture->width;
            height = texture->height;
        }
        count += 1;
    }
    if (desc.has_depth) {
        const VkTextureEntry* texture = VK_CONTEXT::texture(vk, desc.depth.texture.id);
        if (texture == nullptr) {
            GPU::log(GPU::LOG_ERROR, "[vulkan] begin_render_pass (%s): the depth attachment is not a valid texture", name);
            return;
        }
        texture_ids[count] = desc.depth.texture.id;
        views[count] = texture->view;
        clears[count].depthStencil.depth = desc.depth.clear_depth;
        clears[count].depthStencil.stencil = desc.depth.clear_stencil;
        if (width == 0) {
            width = texture->width;
            height = texture->height;
        }
        count += 1;
    }

    const VkRenderPass render_pass = get_render_pass(vk, desc);
    if (render_pass == VK_NULL_HANDLE) {
        return;
    }
    const VkFramebuffer framebuffer = get_framebuffer(vk, render_pass, texture_ids, views, count, width, height);
    if (framebuffer == VK_NULL_HANDLE) {
        return;
    }

    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = render_pass;
    begin.framebuffer = framebuffer;
    begin.renderArea.extent = {width, height};
    begin.clearValueCount = count;
    begin.pClearValues = clears;
    vkCmdBeginRenderPass(list->cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);

    // A full viewport by default; cmd_set_viewport overrides it.
    VkViewport viewport{};
    viewport.width = static_cast<f32>(width);
    viewport.height = static_cast<f32>(height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(list->cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {width, height};
    vkCmdSetScissor(list->cmd, 0, 1, &scissor);
}

static void vk_cmd_end_render_pass(const GpuCommandList cmd) {
    VkCommandListEntry* list = nullptr;
    VK_CONTEXT::of_command_list(cmd, &list);
    if (list != nullptr) {
        vkCmdEndRenderPass(list->cmd);
    }
}

namespace VK_RENDER_PASS_TABLE {
void fill(GpuBackend& table) {
    table.cmd_begin_render_pass = vk_cmd_begin_render_pass;
    table.cmd_end_render_pass = vk_cmd_end_render_pass;
}
} // namespace VK_RENDER_PASS_TABLE
