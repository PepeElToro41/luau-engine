#include "engine/gpu/shaders/module.hpp"

#include "gpu/vk_check.hpp"

#include <cstdio>

VkShaderStageFlagBits SHADER::to_vk_stage(const ShaderStage stage) {
    switch (stage) {
    case SHADER_STAGE_VERTEX:
        return VK_SHADER_STAGE_VERTEX_BIT;
    case SHADER_STAGE_FRAGMENT:
        return VK_SHADER_STAGE_FRAGMENT_BIT;
    case SHADER_STAGE_COMPUTE:
        return VK_SHADER_STAGE_COMPUTE_BIT;
    }
    return VK_SHADER_STAGE_VERTEX_BIT;
}

VkShaderModule SHADER::create_module(const GpuDevice* gpu, const CompiledShader& shader) {
    if (!shader.is_valid()) {
        fprintf(stderr, "[shader] create_module: invalid %s shader\n", stage_name(shader.stage));
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = shader.byte_size();
    info.pCode = shader.code;

    VkShaderModule module = VK_NULL_HANDLE;
    if (!vk_check(vkCreateShaderModule(gpu->device, &info, nullptr, &module), "vkCreateShaderModule")) {
        return VK_NULL_HANDLE;
    }
    return module;
}

void SHADER::destroy_module(const GpuDevice* gpu, const VkShaderModule module) {
    if (module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(gpu->device, module, nullptr);
    }
}

VkPipelineShaderStageCreateInfo SHADER::stage_info(const CompiledShader& shader, const VkShaderModule module) {
    VkPipelineShaderStageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage = to_vk_stage(shader.stage);
    info.module = module;
    info.pName = shader.entry_point;
    return info;
}
