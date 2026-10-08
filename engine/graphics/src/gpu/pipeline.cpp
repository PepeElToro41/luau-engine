#include "engine/gpu/pipeline.hpp"

#include "engine/gpu/shaders/module.hpp"
#include "gpu/vk_check.hpp"

#include <cstdio>

// --- GraphicsPipelineDesc ---------------------------------------------------------

u32 GraphicsPipelineDesc::add_binding(const u32 stride, const VkVertexInputRate rate) {
    if (this->binding_count >= PIPELINE_MAX_VERTEX_BINDINGS) {
        fprintf(stderr, "[pipeline] more than %u vertex bindings\n", PIPELINE_MAX_VERTEX_BINDINGS);
        return UINT32_MAX;
    }
    VkVertexInputBindingDescription& binding = this->bindings[this->binding_count];
    binding.binding = this->binding_count;
    binding.stride = stride;
    binding.inputRate = rate;
    return this->binding_count++;
}

bool GraphicsPipelineDesc::add_attribute(const u32 location, const VkFormat format, const u32 offset, const u32 binding) {
    if (this->attribute_count >= PIPELINE_MAX_VERTEX_ATTRIBUTES) {
        fprintf(stderr, "[pipeline] more than %u vertex attributes\n", PIPELINE_MAX_VERTEX_ATTRIBUTES);
        return false;
    }
    VkVertexInputAttributeDescription& attribute = this->attributes[this->attribute_count++];
    attribute.location = location;
    attribute.binding = binding;
    attribute.format = format;
    attribute.offset = offset;
    return true;
}

bool GraphicsPipelineDesc::add_push_constants(const VkShaderStageFlags stages, const u32 size, const u32 offset) {
    if (this->push_constant_count >= PIPELINE_MAX_PUSH_CONSTANT_RANGES) {
        fprintf(stderr, "[pipeline] more than %u push constant ranges\n", PIPELINE_MAX_PUSH_CONSTANT_RANGES);
        return false;
    }
    VkPushConstantRange& range = this->push_constants[this->push_constant_count++];
    range.stageFlags = stages;
    range.offset = offset;
    range.size = size;
    return true;
}

bool GraphicsPipelineDesc::add_set_layout(const VkDescriptorSetLayout layout) {
    if (this->set_layout_count >= PIPELINE_MAX_DESCRIPTOR_SET_LAYOUTS) {
        fprintf(stderr, "[pipeline] more than %u descriptor set layouts\n", PIPELINE_MAX_DESCRIPTOR_SET_LAYOUTS);
        return false;
    }
    this->set_layouts[this->set_layout_count++] = layout;
    return true;
}

// --- GraphicsPipeline --------------------------------------------------------------

static VkPipelineColorBlendAttachmentState blend_state(const PipelineBlend blend) {
    VkPipelineColorBlendAttachmentState state{};
    state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    switch (blend) {
    case PIPELINE_BLEND_OPAQUE:
        state.blendEnable = VK_FALSE;
        break;
    case PIPELINE_BLEND_ALPHA:
        state.blendEnable = VK_TRUE;
        state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        state.colorBlendOp = VK_BLEND_OP_ADD;
        state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        state.alphaBlendOp = VK_BLEND_OP_ADD;
        break;
    case PIPELINE_BLEND_ADDITIVE:
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

bool GraphicsPipeline::init(GpuDevice* gpu, const RenderTarget& target, const GraphicsPipelineDesc& desc) {
    if (target.render_pass == VK_NULL_HANDLE) {
        fprintf(stderr, "[pipeline] the target has no render pass\n");
        return false;
    }
    return this->init(gpu, target.render_pass, desc);
}

bool GraphicsPipeline::init(GpuDevice* gpu, const VkRenderPass render_pass, const GraphicsPipelineDesc& desc) {
    if (desc.vertex == nullptr || !desc.vertex->is_valid()) {
        fprintf(stderr, "[pipeline] a valid vertex shader is required\n");
        return false;
    }
    if (desc.fragment != nullptr && !desc.fragment->is_valid()) {
        fprintf(stderr, "[pipeline] the fragment shader is invalid\n");
        return false;
    }
    if (render_pass == VK_NULL_HANDLE) {
        fprintf(stderr, "[pipeline] no render pass\n");
        return false;
    }
    if (desc.color_attachment_count > PIPELINE_MAX_COLOR_ATTACHMENTS) {
        fprintf(stderr, "[pipeline] more than %u color attachments\n", PIPELINE_MAX_COLOR_ATTACHMENTS);
        return false;
    }
    VkDevice device = gpu->device;

    // Layout.
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = desc.set_layout_count;
    layout_info.pSetLayouts = desc.set_layouts;
    layout_info.pushConstantRangeCount = desc.push_constant_count;
    layout_info.pPushConstantRanges = desc.push_constants;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (!vk_check(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout), "vkCreatePipelineLayout")) {
        return false;
    }

    // Shader stages. Modules live only until the pipeline exists.
    VkShaderModule vertex_module = SHADER::create_module(gpu, *desc.vertex);
    VkShaderModule fragment_module = desc.fragment != nullptr ? SHADER::create_module(gpu, *desc.fragment) : VK_NULL_HANDLE;
    if (vertex_module == VK_NULL_HANDLE || (desc.fragment != nullptr && fragment_module == VK_NULL_HANDLE)) {
        SHADER::destroy_module(gpu, vertex_module);
        SHADER::destroy_module(gpu, fragment_module);
        vkDestroyPipelineLayout(device, layout, nullptr);
        return false;
    }
    
    VkPipelineShaderStageCreateInfo stages[2];
    u32 stage_count = 0;
    stages[stage_count++] = SHADER::stage_info(*desc.vertex, vertex_module);
    if (desc.fragment != nullptr) {
        stages[stage_count++] = SHADER::stage_info(*desc.fragment, fragment_module);
    }

    // Fixed function.
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = desc.binding_count;
    vertex_input.pVertexBindingDescriptions = desc.bindings;
    vertex_input.vertexAttributeDescriptionCount = desc.attribute_count;
    vertex_input.pVertexAttributeDescriptions = desc.attributes;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = desc.topology;
    input_assembly.primitiveRestartEnable = VK_FALSE;

    // Dynamic: counts only, the values come from vkCmdSetViewport/Scissor.
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.depthClampEnable = VK_FALSE;
    rasterization.rasterizerDiscardEnable = VK_FALSE;
    rasterization.polygonMode = desc.polygon_mode;
    rasterization.cullMode = desc.cull_mode;
    rasterization.frontFace = desc.front_face;
    rasterization.depthBiasEnable = VK_FALSE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisample.sampleShadingEnable = VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = desc.depth_write ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp = desc.depth_compare;
    depth_stencil.depthBoundsTestEnable = VK_FALSE;
    depth_stencil.stencilTestEnable = VK_FALSE;

    // One blend state per color attachment of the subpass, all the same.
    VkPipelineColorBlendAttachmentState blend_attachments[PIPELINE_MAX_COLOR_ATTACHMENTS];
    const VkPipelineColorBlendAttachmentState blend_attachment = blend_state(desc.blend);
    for (u32 i = 0; i < desc.color_attachment_count; ++i) {
        blend_attachments[i] = blend_attachment;
    }
    VkPipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.logicOpEnable = VK_FALSE;
    color_blend.attachmentCount = desc.color_attachment_count;
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
    const bool created = vk_check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "vkCreateGraphicsPipelines");

    SHADER::destroy_module(gpu, vertex_module);
    SHADER::destroy_module(gpu, fragment_module);
    if (!created) {
        vkDestroyPipelineLayout(device, layout, nullptr);
        return false;
    }

    this->gpu = gpu;
    this->layout = layout;
    this->pipeline = pipeline;
    return true;
}

void GraphicsPipeline::shutdown() {
    if (this->gpu == nullptr) {
        return;
    }
    VkDevice device = this->gpu->device;
    if (this->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, this->pipeline, nullptr);
        this->pipeline = VK_NULL_HANDLE;
    }
    if (this->layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, this->layout, nullptr);
        this->layout = VK_NULL_HANDLE;
    }
    this->gpu = nullptr;
}

void GraphicsPipeline::release(GpuResourceManager& resources) {
    resources.release_pipeline(this->pipeline, this->layout);
    this->pipeline = VK_NULL_HANDLE;
    this->layout = VK_NULL_HANDLE;
    this->gpu = nullptr;
}

void GraphicsPipeline::bind(const VkCommandBuffer cmd) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, this->pipeline);
}

void GraphicsPipeline::push_constants(const VkCommandBuffer cmd, const VkShaderStageFlags stages, const void* data, const u32 size, const u32 offset) const {
    vkCmdPushConstants(cmd, this->layout, stages, offset, size, data);
}
