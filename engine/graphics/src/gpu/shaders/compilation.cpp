// CompiledShader, SHADER_COMPILER's dispatch, and the GLSL -> SPIR-V backend
// on shaderc. This is the only file that includes shaderc; the Slang backend
// is compilation_slang.cpp.

#include "engine/gpu/shaders/compilation.hpp"

#include "gpu/shaders/compilation_backends.hpp"

#include "engine/memory/heap_allocator.hpp"
#include "engine/memory/temporal_allocator.hpp"
#include "engine/platform/file.hpp"

#include <shaderc/shaderc.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

// --- Helpers ----------------------------------------------------------------------

void SHADER_BACKEND::append_log(CompiledShader& shader, const char* text, const usz length) {
    if (length == 0) {
        return;
    }
    const usz old_length = shader.log != nullptr ? strlen(shader.log) : 0;
    const bool needs_newline = old_length > 0 && shader.log[old_length - 1] != '\n';
    char* grown = shader.allocator->allocate_array<char>(old_length + (needs_newline ? 1 : 0) + length + 1);
    usz cursor = 0;
    if (old_length > 0) {
        memcpy(grown, shader.log, old_length);
        cursor = old_length;
        if (needs_newline) {
            grown[cursor++] = '\n';
        }
    }
    memcpy(grown + cursor, text, length);
    grown[cursor + length] = '\0';
    shader.allocator->free(shader.log);
    shader.log = grown;
}

CompiledShader SHADER_BACKEND::failed(const ShaderSource& source, BaseAllocator* allocator, const char* message) {
    CompiledShader shader;
    shader.stage = source.stage;
    shader.allocator = allocator;
    char text[512];
    const int n = snprintf(text, sizeof(text), "%s: %s", source.name != nullptr ? source.name : "shader", message);
    append_log(shader, text, n > 0 ? static_cast<usz>(n) : 0);
    return shader;
}

using SHADER_BACKEND::failed;

static shaderc_shader_kind to_shaderc_kind(const ShaderStage stage) {
    switch (stage) {
    case SHADER_STAGE_VERTEX:
        return shaderc_vertex_shader;
    case SHADER_STAGE_FRAGMENT:
        return shaderc_fragment_shader;
    case SHADER_STAGE_COMPUTE:
        return shaderc_compute_shader;
    }
    return shaderc_vertex_shader;
}

// --- Includes ---------------------------------------------------------------------

// Resolves `#include` through SHADER_PREPROCESSING::resolve_include and
// reads the file through the platform layer. Each result
// is one heap block holding the shaderc struct, the resolved path and the
// contents; ReleaseInclude frees it. A miss returns a result with an empty
// source name whose content is the error text, which is how shaderc
// reports include failures.
struct EngineIncluder final : shaderc::CompileOptions::IncluderInterface {
    const ShaderCompileOptions* options = nullptr;

    struct Block {
        shaderc_include_result result;
        char* path;
        char* content;
    };

    // Reads `path` whole into a heap buffer, null-terminated. Null if it
    // cannot be opened.
    static char* read_whole(const char* path, usz* out_size) {
        File file;
        if (!PLATFORM::file_open(&file, path, FILE_ACCESS_READ)) {
            return nullptr;
        }
        u64 size = 0;
        if (!PLATFORM::file_size(file, &size)) {
            PLATFORM::file_close(&file);
            return nullptr;
        }
        char* text = MEMORY::heap_allocator()->allocate_array<char>(static_cast<usz>(size) + 1);
        const bool read = size == 0 || PLATFORM::file_read(file, 0, text, static_cast<usz>(size));
        PLATFORM::file_close(&file);
        if (!read) {
            MEMORY::heap_allocator()->free(text);
            return nullptr;
        }
        text[size] = '\0';
        *out_size = static_cast<usz>(size);
        return text;
    }

    static Block* make_block(const char* path, char* content, const usz content_size) {
        Block* block = MEMORY::heap_allocator()->allocate_array<Block>(1);
        const usz path_length = strlen(path);
        block->path = MEMORY::heap_allocator()->allocate_array<char>(path_length + 1);
        memcpy(block->path, path, path_length + 1);
        block->content = content;
        block->result.source_name = block->path;
        block->result.source_name_length = path_length;
        block->result.content = content;
        block->result.content_length = content_size;
        block->result.user_data = block;
        return block;
    }

    shaderc_include_result* GetInclude(const char* requested, const shaderc_include_type type, const char* requesting, size_t) override {
        char candidate[1024];
        char* content = nullptr;
        usz content_size = 0;
        if (SHADER_PREPROCESSING::resolve_include(requested, type == shaderc_include_type_relative, requesting,
                this->options->include_dirs, this->options->include_dir_count, candidate, sizeof(candidate))) {
            content = read_whole(candidate, &content_size);
        }

        if (content == nullptr) {
            char message[1200];
            snprintf(message, sizeof(message), "cannot open include file '%s'", requested);
            const usz length = strlen(message);
            char* copy = MEMORY::heap_allocator()->allocate_array<char>(length + 1);
            memcpy(copy, message, length + 1);
            Block* block = make_block("", copy, length);
            block->result.source_name_length = 0;
            return &block->result;
        }
        if (this->options->on_include != nullptr) {
            this->options->on_include(candidate, this->options->on_include_user_data);
        }
        return &make_block(candidate, content, content_size)->result;
    }

    void ReleaseInclude(shaderc_include_result* result) override {
        Block* block = static_cast<Block*>(result->user_data);
        MEMORY::heap_allocator()->free(block->content);
        MEMORY::heap_allocator()->free(block->path);
        MEMORY::heap_allocator()->free(block);
    }
};

// --- GLSL backend -----------------------------------------------------------------

CompiledShader SHADER_BACKEND::compile_glsl(const ShaderSource& source, const ShaderCompileOptions& options, BaseAllocator* allocator) {
    // glslang initialises once per process; the compiler object is cheap after
    // that and safe to keep for the program's lifetime.
    static shaderc::Compiler compiler;

    shaderc::CompileOptions compile_options;
    // Vulkan 1.1 means SPIR-V 1.3 at most (see CLAUDE.md, Vulkan target).
    compile_options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
    compile_options.SetTargetSpirv(shaderc_spirv_version_1_3);
    compile_options.SetOptimizationLevel(options.optimize ? shaderc_optimization_level_performance : shaderc_optimization_level_zero);
    if (options.debug_info) {
        compile_options.SetGenerateDebugInfo();
    }
    if (options.warnings_as_errors) {
        compile_options.SetWarningsAsErrors();
    }
    for (usz i = 0; i < options.define_count; ++i) {
        const ShaderDefine& define = options.defines[i];
        if (define.name == nullptr) {
            continue;
        }
        compile_options.AddMacroDefinition(define.name, strlen(define.name),
            define.value, define.value != nullptr ? strlen(define.value) : 0);
    }
    if (options.include_dir_count > 0 || options.on_include != nullptr) {
        std::unique_ptr<EngineIncluder> includer(new EngineIncluder());
        includer->options = &options;
        compile_options.SetIncluder(std::move(includer));
    }

    const usz size = source.size != 0 ? source.size : strlen(source.text);
    const shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(
        source.text, size, to_shaderc_kind(source.stage), source.name, source.entry_point, compile_options);

    CompiledShader shader;
    shader.stage = source.stage;
    shader.allocator = allocator;
    strcpy(shader.entry_point, source.entry_point);

    const std::string& message = result.GetErrorMessage();
    append_log(shader, message.c_str(), message.size());

    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        return shader;
    }

    const usz word_count = static_cast<usz>(result.cend() - result.cbegin());
    u32* code = allocator->allocate_array<u32>(word_count);
    memcpy(code, result.cbegin(), word_count * sizeof(u32));
    shader.code = code;
    shader.word_count = word_count;
    return shader;
}

// --- CompiledShader ---------------------------------------------------------------

static constexpr u32 SPIRV_MAGIC = 0x07230203;

CompiledShader CompiledShader::from_spirv(const ShaderStage stage, const u32* words, const usz word_count, const char* entry_point, BaseAllocator* allocator) {
    CompiledShader shader;
    shader.stage = stage;
    shader.allocator = allocator != nullptr ? allocator : MEMORY::heap_allocator();

    if (words == nullptr || word_count == 0 || words[0] != SPIRV_MAGIC) {
        fprintf(stderr, "[shader] from_spirv: not SPIR-V (bad magic or empty)\n");
        return shader;
    }
    if (entry_point == nullptr || strlen(entry_point) >= sizeof(shader.entry_point)) {
        fprintf(stderr, "[shader] from_spirv: entry point name missing or longer than %zu\n", sizeof(shader.entry_point) - 1);
        return shader;
    }
    strcpy(shader.entry_point, entry_point);

    u32* copy = shader.allocator->allocate_array<u32>(word_count);
    memcpy(copy, words, word_count * sizeof(u32));
    shader.code = copy;
    shader.word_count = word_count;
    return shader;
}

void CompiledShader::free() {
    if (this->allocator != nullptr) {
        this->allocator->free(const_cast<u32*>(this->code));
        this->allocator->free(this->log);
    }
    this->code = nullptr;
    this->word_count = 0;
    this->log = nullptr;
}

// --- SHADER_COMPILER ---------------------------------------------------------------

CompiledShader SHADER_COMPILER::compile(const ShaderSource& source, const ShaderCompileOptions& options, BaseAllocator* allocator) {
    if (allocator == nullptr) {
        allocator = MEMORY::heap_allocator();
    }
    if (source.text == nullptr) {
        return failed(source, allocator, "no source text");
    }
    if (source.entry_point == nullptr || strlen(source.entry_point) >= sizeof(CompiledShader::entry_point)) {
        return failed(source, allocator, "entry point name missing or too long");
    }
    switch (source.language) {
    case SHADER_LANGUAGE_GLSL:
        return SHADER_BACKEND::compile_glsl(source, options, allocator);
    case SHADER_LANGUAGE_SLANG:
        return SHADER_BACKEND::compile_slang(source, options, allocator);
    }
    return failed(source, allocator, "unknown shader language");
}

CompiledShader SHADER_COMPILER::compile_file(const char* path, const ShaderStage stage, const ShaderCompileOptions& options, const ShaderLanguage language, BaseAllocator* allocator) {
    ShaderSource source;
    source.name = path;
    source.stage = stage;
    source.language = language;
    if (allocator == nullptr) {
        allocator = MEMORY::heap_allocator();
    }

    File file;
    if (!PLATFORM::file_open(&file, path, FILE_ACCESS_READ)) {
        return failed(source, allocator, "cannot open file");
    }
    u64 size = 0;
    if (!PLATFORM::file_size(file, &size) || size == 0) {
        PLATFORM::file_close(&file);
        return failed(source, allocator, "empty file or size unavailable");
    }

    TemporalAllocator temp = TemporalAllocator::create();
    char* text = temp.allocate_array<char>(static_cast<usz>(size) + 1);
    const bool read = PLATFORM::file_read(file, 0, text, static_cast<usz>(size));
    PLATFORM::file_close(&file);
    if (!read) {
        return failed(source, allocator, "read failed");
    }
    text[size] = '\0';

    source.text = text;
    source.size = static_cast<usz>(size);
    return compile(source, options, allocator);
}
