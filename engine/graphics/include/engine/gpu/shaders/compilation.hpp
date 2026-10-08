#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/shaders/preprocessing.hpp"
#include "engine/gpu/shaders/shader.hpp"
#include "engine/memory/base_allocator.hpp"

// Text in some language -> SPIR-V:
//
//   ShaderSource      text (Slang or GLSL) + stage + entry point
//        |  SHADER_COMPILER::compile          (one backend per language)
//   CompiledShader    SPIR-V words + stage + entry point, allocator-owned
//
// The backends are compilation_slang.cpp (libslang; `entry_point` names the
// `[shader("...")]` function, the SPIR-V entry point is then "main") and
// compilation.cpp (shaderc, GLSL, entry point "main"); nothing else sees
// either library. A CompiledShader is also what a shader asset would hold
// once shaders are cooked offline: CompiledShader::from_spirv wraps
// precompiled words without a compiler. module.hpp turns one into a
// VkShaderModule.

// Text to compile. Nothing is copied: the strings must outlive the call.
struct ShaderSource {
    const char* text = nullptr;
    // Bytes in `text`; 0 means null-terminated.
    usz size = 0;
    // Shown in error messages, typically the file name.
    const char* name = "shader";
    ShaderStage stage = SHADER_STAGE_VERTEX;
    ShaderLanguage language = SHADER_LANGUAGE_GLSL;
    // GLSL requires "main"; for Slang this is the [shader("...")] function
    // to compile (SHADER_PREPROCESSING::stage_entry_point by convention).
    const char* entry_point = "main";
};

struct ShaderCompileOptions {
    const ShaderDefine* defines = nullptr;
    usz define_count = 0;
    // Run the optimizer. Off keeps the SPIR-V readable in tools and keeps
    // every declared name and binding, which reflection relies on.
    bool optimize = true;
    // Embed source-level debug info (names, lines) for RenderDoc and friends.
    bool debug_info = false;
    // Treat warnings as errors.
    bool warnings_as_errors = false;

    // Directories searched for `#include <x>`, and for `#include "x"` after
    // the including file's own directory (SHADER_PREPROCESSING::
    // resolve_include). With none set, and no `on_include`, includes are an
    // error. `ShaderSource::name` must then be the including file's path so
    // relative includes resolve.
    const char* const* include_dirs = nullptr;
    usz include_dir_count = 0;
    // Called with the resolved path of every file the preprocessor opens,
    // so a loader can watch them for changes.
    void (*on_include)(const char* path, void* user_data) = nullptr;
    void* on_include_user_data = nullptr;
};

// SPIR-V for one stage. Owns `code` and `log` through `allocator`; free()
// releases both. Copyable as a value, but free exactly one copy.
struct CompiledShader {
    ShaderStage stage = SHADER_STAGE_VERTEX;
    // SPIR-V words, 4-byte aligned, or nullptr if compilation failed.
    const u32* code = nullptr;
    usz word_count = 0;
    char entry_point[64] = "main";
    // Compiler diagnostics (warnings on success, errors on failure),
    // null-terminated, or nullptr when there were none.
    char* log = nullptr;
    BaseAllocator* allocator = nullptr;

    bool is_valid() const { return this->code != nullptr && this->word_count > 0; }
    usz byte_size() const { return this->word_count * sizeof(u32); }

    // Copies `word_count` precompiled SPIR-V words into a new CompiledShader.
    // Fails (invalid result) on a bad magic number or an entry point name
    // that does not fit.
    static CompiledShader from_spirv(ShaderStage stage, const u32* words, usz word_count, const char* entry_point = "main", BaseAllocator* allocator = nullptr);

    void free();
};

namespace SHADER_COMPILER {

// Compiles `source` to SPIR-V for Vulkan 1.1. On failure the result is
// invalid and its `log` holds the compiler's error text; on success `log`
// holds warnings, if any. Nothing is printed: the caller decides where the
// log goes. `allocator` defaults to the heap allocator.
CompiledShader compile(const ShaderSource& source, const ShaderCompileOptions& options = {}, BaseAllocator* allocator = nullptr);

// Compiles the whole file at `path` as `stage` in `language`; the file name
// is used as `name`. Same result contract as compile(); a missing file gives
// an invalid result whose log says so.
CompiledShader compile_file(const char* path, ShaderStage stage, const ShaderCompileOptions& options = {}, ShaderLanguage language = SHADER_LANGUAGE_GLSL, BaseAllocator* allocator = nullptr);

} // namespace SHADER_COMPILER
