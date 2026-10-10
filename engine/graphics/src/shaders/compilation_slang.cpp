// The Slang -> SPIR-V backend of SHADER_COMPILER, on libslang. This is the
// only file that includes Slang.
//
// One compile is one entry point: the module is loaded from the source text
// with the options' defines and search paths, the `[shader("...")]` function
// named by `source.entry_point` is linked against it, and that entry point's
// SPIR-V comes back named "main". Matrices: the session asks for
// column-major layout, so a `float4x4` in a block reads the engine's
// Matrix4x4 bytes as-is and `mul(m, v)` is GLSL's `m * v`. Slang's default
// (row-major) draws the cube as a sheared wedge; the command-line flag of
// the same name does not change the SPIR-V, but the session field does.

#include "shaders/compilation_backends.hpp"

#include "engine/memory/heap_allocator.hpp"
#include "engine/memory/temporal_allocator.hpp"

#include <slang/slang-com-ptr.h>
#include <slang/slang.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr usz MAX_DEFINES = 32;
constexpr usz MAX_OPTIONS = 4;

// Loading the core module is the expensive part of starting Slang; one
// global session per process amortizes it. Not thread-safe, like the
// shaderc compiler object.
slang::IGlobalSession* global_session() {
    static Slang::ComPtr<slang::IGlobalSession> session;
    if (!session) {
        SlangGlobalSessionDesc desc;
        if (SLANG_FAILED(slang_createGlobalSession2(&desc, session.writeRef()))) {
            session = nullptr;
        }
    }
    return session.get();
}

SlangStage to_slang_stage(const ShaderStage stage) {
    switch (stage) {
    case SHADER_STAGE_VERTEX:
        return SLANG_STAGE_VERTEX;
    case SHADER_STAGE_FRAGMENT:
        return SLANG_STAGE_FRAGMENT;
    case SHADER_STAGE_COMPUTE:
        return SLANG_STAGE_COMPUTE;
    }
    return SLANG_STAGE_NONE;
}

void append_blob(CompiledShader& shader, slang::IBlob* blob) {
    if (blob != nullptr && blob->getBufferSize() > 0) {
        SHADER_BACKEND::append_log(shader, static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize());
    }
}

void append_text(CompiledShader& shader, const char* text) {
    SHADER_BACKEND::append_log(shader, text, strlen(text));
}

// "unlit" for "render/unlit.slang": the module name Slang reports in
// diagnostics about the file itself.
void module_name_of(const char* path, char* out, const usz out_size) {
    const char* base = path;
    for (const char* c = path; *c != '\0'; ++c) {
        if (*c == '/' || *c == '\\') {
            base = c + 1;
        }
    }
    usz length = strlen(base);
    const char* dot = strrchr(base, '.');
    if (dot != nullptr && dot != base) {
        length = static_cast<usz>(dot - base);
    }
    if (length >= out_size) {
        length = out_size - 1;
    }
    memcpy(out, base, length);
    out[length] = '\0';
}

} // namespace

CompiledShader SHADER_BACKEND::compile_slang(const ShaderSource& source, const ShaderCompileOptions& options, BaseAllocator* allocator) {
    slang::IGlobalSession* global = global_session();
    if (global == nullptr) {
        return failed(source, allocator, "could not start the Slang compiler");
    }
    if (options.define_count > MAX_DEFINES) {
        return failed(source, allocator, "too many defines");
    }
    if (options.target != GPU_BYTECODE_SPIRV) {
        return failed(source, allocator, "only SPIR-V output is implemented for Slang yet");
    }

    // Vulkan 1.1 means SPIR-V 1.3 at most (see CLAUDE.md, Vulkan target).
    slang::TargetDesc target;
    target.format = SLANG_SPIRV;
    target.profile = global->findProfile("spirv_1_3");

    slang::CompilerOptionEntry entries[MAX_OPTIONS];
    u32 entry_count = 0;
    const auto add_int = [&](const slang::CompilerOptionName name, const i32 value) {
        entries[entry_count].name = name;
        entries[entry_count].value.kind = slang::CompilerOptionValueKind::Int;
        entries[entry_count].value.intValue0 = value;
        entry_count += 1;
    };
    add_int(slang::CompilerOptionName::Optimization, options.optimize ? SLANG_OPTIMIZATION_LEVEL_DEFAULT : SLANG_OPTIMIZATION_LEVEL_NONE);
    add_int(slang::CompilerOptionName::DebugInformation, options.debug_info ? SLANG_DEBUG_INFO_LEVEL_STANDARD : SLANG_DEBUG_INFO_LEVEL_NONE);
    if (options.warnings_as_errors) {
        entries[entry_count].name = slang::CompilerOptionName::WarningsAsErrors;
        entries[entry_count].value.kind = slang::CompilerOptionValueKind::String;
        entries[entry_count].value.stringValue0 = "all";
        entry_count += 1;
    }

    slang::PreprocessorMacroDesc macros[MAX_DEFINES];
    usz macro_count = 0;
    for (usz i = 0; i < options.define_count; ++i) {
        const ShaderDefine& define = options.defines[i];
        if (define.name == nullptr) {
            continue;
        }
        macros[macro_count].name = define.name;
        macros[macro_count].value = define.value != nullptr ? define.value : "";
        macro_count += 1;
    }

    slang::SessionDesc desc;
    desc.targets = &target;
    desc.targetCount = 1;
    desc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
    desc.searchPaths = options.include_dirs;
    desc.searchPathCount = static_cast<SlangInt>(options.include_dir_count);
    desc.preprocessorMacros = macros;
    desc.preprocessorMacroCount = static_cast<SlangInt>(macro_count);
    desc.compilerOptionEntries = entries;
    desc.compilerOptionEntryCount = entry_count;

    Slang::ComPtr<slang::ISession> session;
    if (SLANG_FAILED(global->createSession(desc, session.writeRef()))) {
        return failed(source, allocator, "could not create a Slang session");
    }

    // Slang wants a C string; honour a sized source by copying it.
    TemporalAllocator temp = TemporalAllocator::create();
    const char* text = source.text;
    if (source.size != 0) {
        char* copy = temp.allocate_array<char>(source.size + 1);
        memcpy(copy, source.text, source.size);
        copy[source.size] = '\0';
        text = copy;
    }

    CompiledShader shader;
    shader.stage = source.stage;
    shader.allocator = allocator;
    strcpy(shader.entry_point, "main");

    char module_name[128];
    module_name_of(source.name != nullptr ? source.name : "shader", module_name, sizeof(module_name));
    Slang::ComPtr<slang::IBlob> diagnostics;
    slang::IModule* module = session->loadModuleFromSourceString(module_name, source.name, text, diagnostics.writeRef());
    append_blob(shader, diagnostics);
    if (module == nullptr) {
        if (shader.log == nullptr) {
            append_text(shader, "Slang could not load the module");
        }
        return shader;
    }

    // Every file the module pulled in, for reload-on-change; the module's
    // own file is listed too and skipped.
    if (options.on_include != nullptr) {
        const SlangInt32 dependency_count = module->getDependencyFileCount();
        for (SlangInt32 i = 0; i < dependency_count; ++i) {
            const char* path = module->getDependencyFilePath(i);
            if (path != nullptr && (source.name == nullptr || strcmp(path, source.name) != 0)) {
                options.on_include(path, options.on_include_user_data);
            }
        }
    }

    Slang::ComPtr<slang::IEntryPoint> entry_point;
    if (SLANG_FAILED(module->findEntryPointByName(source.entry_point, entry_point.writeRef())) || !entry_point) {
        char message[256];
        snprintf(message, sizeof(message), "%s: no [shader(\"%s\")] function named '%s'",
            source.name != nullptr ? source.name : "shader", SHADER::stage_name(source.stage), source.entry_point);
        append_text(shader, message);
        return shader;
    }

    slang::IComponentType* parts[2] = {module, entry_point.get()};
    Slang::ComPtr<slang::IComponentType> composite;
    diagnostics = nullptr;
    const SlangResult composed = session->createCompositeComponentType(parts, 2, composite.writeRef(), diagnostics.writeRef());
    append_blob(shader, diagnostics);
    if (SLANG_FAILED(composed)) {
        return shader;
    }

    Slang::ComPtr<slang::IComponentType> linked;
    diagnostics = nullptr;
    const SlangResult linked_result = composite->link(linked.writeRef(), diagnostics.writeRef());
    append_blob(shader, diagnostics);
    if (SLANG_FAILED(linked_result)) {
        return shader;
    }

    // The function must be the stage the caller asked for.
    diagnostics = nullptr;
    slang::ProgramLayout* layout = linked->getLayout(0, diagnostics.writeRef());
    append_blob(shader, diagnostics);
    if (layout != nullptr && layout->getEntryPointCount() > 0) {
        const SlangStage actual = layout->getEntryPointByIndex(0)->getStage();
        if (actual != to_slang_stage(source.stage)) {
            char message[256];
            snprintf(message, sizeof(message), "%s: '%s' is not a %s entry point",
                source.name != nullptr ? source.name : "shader", source.entry_point, SHADER::stage_name(source.stage));
            append_text(shader, message);
            return shader;
        }
    }

    Slang::ComPtr<slang::IBlob> code;
    diagnostics = nullptr;
    const SlangResult coded = linked->getEntryPointCode(0, 0, code.writeRef(), diagnostics.writeRef());
    append_blob(shader, diagnostics);
    if (SLANG_FAILED(coded) || !code || code->getBufferSize() < sizeof(u32)) {
        if (shader.log == nullptr) {
            append_text(shader, "Slang produced no code");
        }
        return shader;
    }

    const usz word_count = code->getBufferSize() / sizeof(u32);
    u32* words = allocator->allocate_array<u32>(word_count);
    memcpy(words, code->getBufferPointer(), word_count * sizeof(u32));
    shader.kind = GPU_BYTECODE_SPIRV;
    shader.bytes = reinterpret_cast<const u8*>(words);
    shader.size = word_count * sizeof(u32);
    return shader;
}
