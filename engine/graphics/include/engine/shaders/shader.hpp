#pragma once

#include "engine/defines.hpp"

// shaders/: a shader file's way onto the GPU, one header per step.
//
//   preprocessing.hpp  what the engine does to the text before any compiler
//                      sees it: the `#pragma pass` directives, the STAGE_* /
//                      PASS_* defines, `#include` support and resolution
//   compilation.hpp    ShaderSource -> SHADER_COMPILER::compile -> CompiledShader
//                      (SPIR-V words; libslang in compilation_slang.cpp,
//                      shaderc in compilation.cpp, nowhere else)
//   reflection.hpp     what the SPIR-V declares: bindings, uniform blocks,
//                      push constants, vertex inputs (spirv-reflect, confined
//                      to reflection.cpp)
//   program.hpp        SHADER_PROGRAM::load: one file, every pass it declares,
//                      compiled and reflected, with its descriptor set layouts
//   module.hpp         CompiledShader -> VkShaderModule, only needed while a
//                      pipeline is being created
//
// This header is the vocabulary they all share and pulls in no Vulkan, so
// the preprocessing step can be unit-tested without a GPU.

enum ShaderStage : u32 {
    SHADER_STAGE_VERTEX = 0,
    SHADER_STAGE_FRAGMENT = 1,
    SHADER_STAGE_COMPUTE = 2,
};

// Bit per stage, for a pass directive's stage list.
enum ShaderStageMask : u32 {
    SHADER_STAGE_MASK_VERTEX = 1u << 0,
    SHADER_STAGE_MASK_FRAGMENT = 1u << 1,
    SHADER_STAGE_MASK_COMPUTE = 1u << 2,
};

// The engine's shaders are Slang (.slang); GLSL (.glsl) still compiles for
// projects that bring it.
enum ShaderLanguage : u32 {
    SHADER_LANGUAGE_GLSL = 0,
    SHADER_LANGUAGE_SLANG = 1,
};

namespace SHADER {

inline const char* stage_name(const ShaderStage stage) {
    switch (stage) {
    case SHADER_STAGE_VERTEX:
        return "vertex";
    case SHADER_STAGE_FRAGMENT:
        return "fragment";
    case SHADER_STAGE_COMPUTE:
        return "compute";
    }
    return "unknown";
}

} // namespace SHADER
