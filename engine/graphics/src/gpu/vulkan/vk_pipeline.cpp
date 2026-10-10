#include "gpu/vulkan/vk_check.hpp"
#include "gpu/vulkan/vk_context.hpp"

// --- Template render passes ------------------------------------------------------

// Render pass compatibility depends on attachment formats and sample counts
// only, so one pass per GpuTargetFormats serves every pipeline drawing into
// those formats, whatever the real pass's load/store ops are.
VkRenderPass VK_PIPELINE::template_render_pass(VulkanContext& vk, const GpuTargetFormats& formats) {
    const u64 key = formats.hash();
    if (VkRenderPass* found = vk.template_render_passes.find(key)) {
        return *found;
    }

    VkAttachmentDescription attachments[GPU_MAX_COLOR_ATTACHMENTS + 1] = {};
    VkAttachmentReference color_refs[GPU_MAX_COLOR_ATTACHMENTS] = {};
    VkAttachmentReference depth_ref{};
    u32 count = 0;
    for (u32 i = 0; i < formats.color_count; ++i) {
        VkAttachmentDescription& a = attachments[count];
        a.format = VK_FORMATS::to_vk(formats.color[i]);
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_refs[i].attachment = count;
        color_refs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        count += 1;
    }
    const bool has_depth = formats.depth != GPU_FORMAT_UNDEFINED;
    if (has_depth) {
        VkAttachmentDescription& a = attachments[count];
        a.format = VK_FORMATS::to_vk(formats.depth);
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_ref.attachment = count;
        depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        count += 1;
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = formats.color_count;
    subpass.pColorAttachments = color_refs;
    subpass.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;

    // Compatibility also requires identical dependencies.
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
    if (!vk_check(vkCreateRenderPass(vk.device, &info, nullptr, &render_pass), "vkCreateRenderPass (template)")) {
        return VK_NULL_HANDLE;
    }
    vk.template_render_passes.insert(key, render_pass);
    return render_pass;
}

void VK_PIPELINE::shutdown(VulkanContext& vk) {
    for (usz i = 0; i < vk.pipelines.alive_count; ++i) {
        const SparseId id = vk.pipelines.get_alive_id(i);
        if (id != GPU_NULL_ID) {
            const VkPipelineEntry* entry = vk.pipelines.get_element_alive(id);
            vkDestroyPipeline(vk.device, entry->pipeline, nullptr);
            vkDestroyPipelineLayout(vk.device, entry->layout, nullptr);
        }
    }
    vk.pipelines.free();
    for (const auto& entry : vk.template_render_passes) {
        vkDestroyRenderPass(vk.device, entry.value, nullptr);
    }
    vk.template_render_passes.free();
}

// --- Pipelines --------------------------------------------------------------------

static VkShaderModule create_module(VulkanContext& vk, const GpuShaderStageDesc& stage, const char* what) {
    if (stage.kind != GPU_BYTECODE_SPIRV) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] create_pipeline: the %s stage is not SPIR-V", what);
        return VK_NULL_HANDLE;
    }
    if ((stage.size % 4) != 0) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] create_pipeline: the %s stage's SPIR-V size is not a multiple of 4", what);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = stage.size;
    info.pCode = static_cast<const u32*>(stage.bytes);
    VkShaderModule module = VK_NULL_HANDLE;
    if (!vk_check(vkCreateShaderModule(vk.device, &info, nullptr, &module), "vkCreateShaderModule")) {
        return VK_NULL_HANDLE;
    }
    return module;
}

static VkPipelineColorBlendAttachmentState blend_state(const GpuBlend blend) {
    VkPipelineColorBlendAttachmentState state{};
    state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    switch (blend) {
    case GPU_BLEND_OPAQUE:
        state.blendEnable = VK_FALSE;
        break;
    case GPU_BLEND_ALPHA:
        state.blendEnable = VK_TRUE;
        state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        state.colorBlendOp = VK_BLEND_OP_ADD;
        state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        state.alphaBlendOp = VK_BLEND_OP_ADD;
        break;
    case GPU_BLEND_ADDITIVE:
        state.blendEnable = VK_TRUE;
        state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        state.colorBlendOp = VK_BLEND_OP_ADD;
        state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        state.alphaBlendOp = VK_BLEND_OP_ADD;
        break;
    }
    return state;
}

static VkPrimitiveTopology to_vk(const GpuTopology topology) {
    switch (topology) {
    case GPU_TOPOLOGY_TRIANGLE_LIST:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case GPU_TOPOLOGY_TRIANGLE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case GPU_TOPOLOGY_LINE_LIST:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case GPU_TOPOLOGY_POINT_LIST:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

static VkCullModeFlags to_vk(const GpuCullMode cull) {
    switch (cull) {
    case GPU_CULL_NONE:
        return VK_CULL_MODE_NONE;
    case GPU_CULL_BACK:
        return VK_CULL_MODE_BACK_BIT;
    case GPU_CULL_FRONT:
        return VK_CULL_MODE_FRONT_BIT;
    }
    return VK_CULL_MODE_BACK_BIT;
}

static VkCompareOp to_vk(const GpuCompare compare) {
    switch (compare) {
    case GPU_COMPARE_NEVER:
        return VK_COMPARE_OP_NEVER;
    case GPU_COMPARE_LESS:
        return VK_COMPARE_OP_LESS;
    case GPU_COMPARE_EQUAL:
        return VK_COMPARE_OP_EQUAL;
    case GPU_COMPARE_LESS_EQUAL:
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    case GPU_COMPARE_GREATER:
        return VK_COMPARE_OP_GREATER;
    case GPU_COMPARE_NOT_EQUAL:
        return VK_COMPARE_OP_NOT_EQUAL;
    case GPU_COMPARE_GREATER_EQUAL:
        return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case GPU_COMPARE_ALWAYS:
        return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_LESS;
}

static GpuPipeline vk_create_pipeline(GpuContext* gpu, const GpuPipelineDesc& desc) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    GpuPipeline result;
    const char* name = desc.name != nullptr ? desc.name : "pipeline";
    if (!desc.vertex.is_valid()) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] create_pipeline (%s): a vertex stage is required", name);
        return result;
    }
    if (desc.targets.color_count > GPU_MAX_COLOR_ATTACHMENTS) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] create_pipeline (%s): more than %u color attachments", name, GPU_MAX_COLOR_ATTACHMENTS);
        return result;
    }
    if (desc.push_constant_size > gpu->info.limits.max_push_constant_size) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] create_pipeline (%s): %u bytes of push constants, the limit is %u", name, desc.push_constant_size, gpu->info.limits.max_push_constant_size);
        return result;
    }
    if (desc.bind_layout_count > GPU_MAX_BIND_GROUPS) {
        GPU::log(GPU::LOG_ERROR, "[vulkan] create_pipeline (%s): more than %u bind groups", name, GPU_MAX_BIND_GROUPS);
        return result;
    }
    const VkRenderPass render_pass = VK_PIPELINE::template_render_pass(vk, desc.targets);
    if (render_pass == VK_NULL_HANDLE) {
        return result;
    }

    // Layout: the groups, gaps filled with the empty layout, plus one push
    // constant block for both stages.
    VkDescriptorSetLayout set_layouts[GPU_MAX_BIND_GROUPS] = {};
    for (u32 i = 0; i < desc.bind_layout_count; ++i) {
        GpuBindLayout id = desc.bind_layouts[i];
        if (!id.is_valid()) {
            id = VULKAN_BACKEND::table()->bind_layout(gpu, GpuBindLayoutDesc{});
        }
        const VkBindLayoutEntry* entry = VK_CONTEXT::bind_layout(vk, id.id);
        if (entry == nullptr) {
            GPU::log(GPU::LOG_ERROR, "[vulkan] create_pipeline (%s): bind layout %u is invalid", name, i);
            return result;
        }
        set_layouts[i] = entry->layout;
    }
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = desc.push_constant_size;
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = desc.bind_layout_count;
    layout_info.pSetLayouts = set_layouts;
    layout_info.pushConstantRangeCount = desc.push_constant_size > 0 ? 1 : 0;
    layout_info.pPushConstantRanges = &push_range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (!vk_check(vkCreatePipelineLayout(vk.device, &layout_info, nullptr, &layout), "vkCreatePipelineLayout")) {
        return result;
    }

    // Shader stages. Modules live only until the pipeline exists.
    VkShaderModule vertex_module = create_module(vk, desc.vertex, "vertex");
    VkShaderModule fragment_module = desc.fragment.is_valid() ? create_module(vk, desc.fragment, "fragment") : VK_NULL_HANDLE;
    if (vertex_module == VK_NULL_HANDLE || (desc.fragment.is_valid() && fragment_module == VK_NULL_HANDLE)) {
        if (vertex_module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(vk.device, vertex_module, nullptr);
        }
        if (fragment_module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(vk.device, fragment_module, nullptr);
        }
        vkDestroyPipelineLayout(vk.device, layout, nullptr);
        return result;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {};
    u32 stage_count = 0;
    stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stage_count].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[stage_count].module = vertex_module;
    stages[stage_count].pName = desc.vertex.entry_point;
    stage_count += 1;
    if (fragment_module != VK_NULL_HANDLE) {
        stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[stage_count].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[stage_count].module = fragment_module;
        stages[stage_count].pName = desc.fragment.entry_point;
        stage_count += 1;
    }

    // Vertex input.
    VkVertexInputBindingDescription bindings[GPU_MAX_VERTEX_BINDINGS] = {};
    for (u32 i = 0; i < desc.vertex_input.binding_count; ++i) {
        bindings[i].binding = i;
        bindings[i].stride = desc.vertex_input.bindings[i].stride;
        bindings[i].inputRate = desc.vertex_input.bindings[i].rate == GPU_VERTEX_RATE_INSTANCE ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
    }
    VkVertexInputAttributeDescription attributes[GPU_MAX_VERTEX_ATTRIBUTES] = {};
    for (u32 i = 0; i < desc.vertex_input.attribute_count; ++i) {
        const GpuVertexAttribute& a = desc.vertex_input.attributes[i];
        attributes[i].location = a.location;
        attributes[i].binding = a.binding;
        attributes[i].format = VK_FORMATS::to_vk(a.format);
        attributes[i].offset = a.offset;
    }
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = desc.vertex_input.binding_count;
    vertex_input.pVertexBindingDescriptions = bindings;
    vertex_input.vertexAttributeDescriptionCount = desc.vertex_input.attribute_count;
    vertex_input.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = to_vk(desc.raster.topology);

    // Dynamic: counts only, the values come from cmd_set_viewport.
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = desc.raster.wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rasterization.cullMode = to_vk(desc.raster.cull);
    rasterization.frontFace = desc.raster.front_face_clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = desc.depth.test ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = desc.depth.write ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp = to_vk(desc.depth.compare);

    // One blend state per color attachment, all the same.
    VkPipelineColorBlendAttachmentState blend_attachments[GPU_MAX_COLOR_ATTACHMENTS];
    const VkPipelineColorBlendAttachmentState blend_attachment = blend_state(desc.blend);
    for (u32 i = 0; i < desc.targets.color_count; ++i) {
        blend_attachments[i] = blend_attachment;
    }
    VkPipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = desc.targets.color_count;
    color_blend.pAttachments = blend_attachments;

    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = stage_count;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_stencil;
    info.pColorBlendState = &color_blend;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    info.renderPass = render_pass;
    info.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const bool created = vk_check(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "vkCreateGraphicsPipelines");
    vkDestroyShaderModule(vk.device, vertex_module, nullptr);
    if (fragment_module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(vk.device, fragment_module, nullptr);
    }
    if (!created) {
        vkDestroyPipelineLayout(vk.device, layout, nullptr);
        return result;
    }

    result.id = vk.pipelines.new_element();
    VkPipelineEntry& entry = *vk.pipelines.get_element_alive(result.id);
    entry.pipeline = pipeline;
    entry.layout = layout;
    entry.push_constant_size = desc.push_constant_size;
    return result;
}

static void vk_destroy_pipeline(GpuContext* gpu, const GpuPipeline pipeline) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    const VkPipelineEntry* entry = VK_CONTEXT::pipeline(vk, pipeline.id);
    if (entry == nullptr) {
        return;
    }
    vkDestroyPipeline(vk.device, entry->pipeline, nullptr);
    vkDestroyPipelineLayout(vk.device, entry->layout, nullptr);
    vk.pipelines.delete_element(pipeline.id);
}

static void vk_release_pipeline(GpuContext* gpu, const GpuPipeline pipeline) {
    VulkanContext& vk = VK_CONTEXT::of(gpu);
    const VkPipelineEntry* entry = VK_CONTEXT::pipeline(vk, pipeline.id);
    if (entry == nullptr) {
        return;
    }
    VkPending pending;
    pending.pipeline = entry->pipeline;
    pending.pipeline_layout = entry->layout;
    VK_FRAME::release(vk, pending);
    vk.pipelines.delete_element(pipeline.id);
}

namespace VK_PIPELINE_TABLE {
void fill(GpuBackend& table) {
    table.create_pipeline = vk_create_pipeline;
    table.destroy_pipeline = vk_destroy_pipeline;
    table.release_pipeline = vk_release_pipeline;
}
} // namespace VK_PIPELINE_TABLE
