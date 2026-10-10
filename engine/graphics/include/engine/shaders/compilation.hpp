#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/gpu_types.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/shaders/preprocessing.hpp"
#include "engine/shaders/shader.hpp"

// Step two of the shader path: source text -> bytecode. One backend per
// language (GLSL through shaderc, Slang through libslang), both private to
// src/shaders/. The output is API-neutral: bytes plus the kind of bytecode
// they are (SPIR-V today; DXIL and Metal libraries when those backends
// exist), ready for GpuPipelineDesc through CompiledShader::stage_desc().
// Nothing here prints; the log goes to the caller.

struct ShaderSource {
    const char* text = nullptr;
    // 0 = strlen(text).
    usz size = 0;
    // For diagnostics and GLSL's relative includes.
    const char* name = "shader";
    ShaderStage stage = SHADER_STAGE_VERTEX;
    ShaderLanguage language = SHADER_LANGUAGE_GLSL;
    // Slang: the [shader("...")] function to link; GLSL: always "main".
    const char* entry_point = "main";
};

struct ShaderCompileOptions {
    const ShaderDefine* defines = nullptr;
    usz define_count = 0;
    bool optimize = true;
    bool debug_info = false;
    bool warnings_as_errors = false;
    // Searched after the including file's directory (preprocessing.hpp).
    const char* const* include_dirs = nullptr;
    usz include_dir_count = 0;
    // Called with the resolved path of every file the compile included.
    void (*on_include)(const char* path, void* user_data) = nullptr;
    void* on_include_user_data = nullptr;
    // What to emit. Only SPIR-V is implemented; the backends refuse the rest.
    GpuShaderBytecode target = GPU_BYTECODE_SPIRV;
};

struct CompiledShader {
    ShaderStage stage = SHADER_STAGE_VERTEX;
    GpuShaderBytecode kind = GPU_BYTECODE_SPIRV;
    // Allocator-owned. SPIR-V bytes are 4-byte aligned.
    const u8* bytes = nullptr;
    usz size = 0;
    // The entry point the bytecode exposes: "main" for every backend.
    char entry_point[64] = "main";
    // Errors or warnings from the compiler, allocator-owned; nullptr when
    // there were none. A failed compile has a log and no bytes.
    char* log = nullptr;
    BaseAllocator* allocator = nullptr;

    bool is_valid() const { return this->bytes != nullptr && this->size > 0; }
    // What GpuPipelineDesc wants for this stage.
    GpuShaderStageDesc stage_desc() const {
        GpuShaderStageDesc desc;
        desc.bytes = this->bytes;
        desc.size = this->size;
        desc.entry_point = this->entry_point;
        desc.kind = this->kind;
        return desc;
    }

    // Wraps precompiled bytecode (copied into `allocator`, heap when null).
    // SPIR-V is checked for its magic number and word alignment.
    static CompiledShader from_bytecode(ShaderStage stage, GpuShaderBytecode kind, const void* bytes, usz size, const char* entry_point = "main", BaseAllocator* allocator = nullptr);
    void free();
};

namespace SHADER_COMPILER {

CompiledShader compile(const ShaderSource& source, const ShaderCompileOptions& options = {}, BaseAllocator* allocator = nullptr);
// Reads the file and compiles it; the file's directory is the first include root.
CompiledShader compile_file(const char* path, ShaderStage stage, const ShaderCompileOptions& options = {}, ShaderLanguage language = SHADER_LANGUAGE_GLSL, BaseAllocator* allocator = nullptr);

} // namespace SHADER_COMPILER
