#pragma once

#include "engine/shaders/compilation.hpp"

// Private to the compilation sources: one backend per ShaderLanguage, each in
// its own file so that only that file sees its compiler library, plus the
// result helpers they share. SHADER_COMPILER::compile dispatches on
// ShaderSource::language.

namespace SHADER_BACKEND {

// GLSL through shaderc (compilation.cpp).
CompiledShader compile_glsl(const ShaderSource& source, const ShaderCompileOptions& options, BaseAllocator* allocator);

// Slang through libslang (compilation_slang.cpp). `source.entry_point` names
// the `[shader("...")]` function to compile; the SPIR-V entry point is "main".
CompiledShader compile_slang(const ShaderSource& source, const ShaderCompileOptions& options, BaseAllocator* allocator);

// Copies `length` bytes of `text` into `shader.log` through its allocator,
// appending to what is there. Empty text leaves the log alone.
void append_log(CompiledShader& shader, const char* text, usz length);

// An invalid result for `source` whose log is "<name>: <message>".
CompiledShader failed(const ShaderSource& source, BaseAllocator* allocator, const char* message);

} // namespace SHADER_BACKEND
