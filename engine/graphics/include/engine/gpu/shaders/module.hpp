#pragma once

#include "engine/gpu/device.hpp"
#include "engine/gpu/shaders/compilation.hpp"

#include <volk.h>

// CompiledShader -> VkShaderModule, the last step before a pipeline.
//
// A VkShaderModule is only needed while its pipelines are being created, so
// there is no deferred path: destroy it right after vkCreateGraphicsPipelines.

namespace SHADER {

VkShaderStageFlagBits to_vk_stage(ShaderStage stage);

// Wraps the SPIR-V in a shader module. VK_NULL_HANDLE if the shader is
// invalid or Vulkan refuses it.
VkShaderModule create_module(const GpuDevice* gpu, const CompiledShader& shader);
void destroy_module(const GpuDevice* gpu, VkShaderModule module);

// The VkPipelineShaderStageCreateInfo for `module` as `shader`'s stage and
// entry point. `shader` must outlive the pipeline creation that uses it,
// since the entry point name is referenced, not copied.
VkPipelineShaderStageCreateInfo stage_info(const CompiledShader& shader, VkShaderModule module);

} // namespace SHADER
