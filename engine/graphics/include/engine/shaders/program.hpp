#pragma once

#include "engine/defines.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/shaders/compilation.hpp"
#include "engine/shaders/preprocessing.hpp"
#include "engine/shaders/reflection.hpp"

// Step four of the shader path: a shader file -> ShaderProgram, the unit a
// Shader component holds. Per `#pragma pass <tag> [vertex] [fragment]` it
// compiles the stages with STAGE_* and PASS_<TAG> defined (Slang: the
// `vertex` / `fragment` functions of the file; GLSL: `main` per #ifdef
// section), reflects them, and checks the set convention:
//
//     set 0  the frame block, a subset of `frame_interface`
//     set 1  the pass's inputs (binding i = graph input i)
//     set 2  the material: one uniform block plus textures and samplers,
//            identical in every pass that declares it
//     set 3  reserved
//
// Loading needs no device: a program is bytecode plus reflection, a pure
// function of the file and the options. The frontend asks the backend for
// bind layouts (GPU::bind_layout from SHADER_REFLECT::layout_bindings) and
// pipelines when it first draws with it. Everything a program owns is
// released by free(); includes list the files the compile read, for
// reload-on-change.

static constexpr u32 SHADER_PROGRAM_MAX_INCLUDES = 32;
static constexpr u32 SHADER_PROGRAM_PATH_MAX = 512;
static constexpr u32 SHADER_PROGRAM_MAX_PUSH_CONSTANTS = GPU_MAX_PUSH_CONSTANT_SIZE;

struct ShaderPassProgram {
    u64 tag = 0;
    char tag_name[SHADER_PREPROCESSING::PASS_NAME_MAX] = {};
    CompiledShader vertex;
    // Invalid for a vertex-only (depth) pass.
    CompiledShader fragment;
    ShaderReflection reflection;

    bool uses_material() const { return (this->reflection.set_mask() & 4u) != 0; }
    bool has_push_constants() const { return this->reflection.push_constant_size > 0; }
};

struct ShaderProgram {
    char path[SHADER_PROGRAM_PATH_MAX] = {};
    ShaderLanguage language = SHADER_LANGUAGE_SLANG;
    ShaderPassProgram passes[SHADER_PREPROCESSING::MAX_PASSES] = {};
    u32 pass_count = 0;
    // The set-2 bindings (and their members) shared by the passes that
    // declare a material; empty when none does.
    ShaderReflection material_interface;
    u32 material_block_binding = 0xffffffffu;
    u32 material_block_size = 0;
    // Every file the compile read, allocator-owned paths.
    char* includes[SHADER_PROGRAM_MAX_INCLUDES] = {};
    u32 include_count = 0;
    // Compiler and validation messages, allocator-owned; kept on failure.
    char* log = nullptr;
    BaseAllocator* allocator = nullptr;

    bool is_valid() const { return this->pass_count > 0; }
    const ShaderPassProgram* find_pass(u64 tag) const;
    bool has_material_block() const { return this->material_block_binding != 0xffffffffu; }
    void free();
};

struct ShaderProgramLoadDesc {
    const char* path = nullptr;
    const char* const* include_dirs = nullptr;
    usz include_dir_count = 0;
    // What set 0 may contain (the renderer's frame block). nullptr skips the check.
    const ShaderReflection* frame_interface = nullptr;
    // Optimization strips member names, which the material system needs.
    bool optimize = false;
    bool debug_info = false;
    GpuShaderBytecode target = GPU_BYTECODE_SPIRV;
};

namespace SHADER_PROGRAM {

// False with the reasons in out.log. `allocator` (heap when null) owns the
// bytecode, the include paths and the log.
bool load(const ShaderProgramLoadDesc& desc, BaseAllocator* allocator, ShaderProgram& out);

} // namespace SHADER_PROGRAM
