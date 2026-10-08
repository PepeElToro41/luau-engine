#include "engine/gpu/shaders/program.hpp"

#include "engine/memory/heap_allocator.hpp"
#include "engine/platform/file.hpp"
#include "engine/utils/hash.hpp"

#include <cstdio>
#include <cstring>

// --- Log ----------------------------------------------------------------------------

namespace {

// Appends lines to `program.log`, growing it through the allocator.
void append_log(ShaderProgram& program, const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    const usz old_length = program.log != nullptr ? strlen(program.log) : 0;
    const usz add_length = strlen(text);
    const bool needs_newline = old_length > 0 && program.log[old_length - 1] != '\n';
    char* grown = program.allocator->allocate_array<char>(old_length + (needs_newline ? 1 : 0) + add_length + 1);
    usz cursor = 0;
    if (old_length > 0) {
        memcpy(grown, program.log, old_length);
        cursor = old_length;
        if (needs_newline) {
            grown[cursor++] = '\n';
        }
    }
    memcpy(grown + cursor, text, add_length);
    grown[cursor + add_length] = '\0';
    program.allocator->free(program.log);
    program.log = grown;
}

void append_logf(ShaderProgram& program, const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    append_log(program, text);
}

void record_include(const char* path, void* user_data) {
    ShaderProgram& program = *static_cast<ShaderProgram*>(user_data);
    for (u32 i = 0; i < program.include_count; ++i) {
        if (strcmp(program.includes[i], path) == 0) {
            return;
        }
    }
    if (program.include_count >= SHADER_PROGRAM_MAX_INCLUDES) {
        return;
    }
    const usz length = strlen(path);
    char* copy = program.allocator->allocate_array<char>(length + 1);
    memcpy(copy, path, length + 1);
    program.includes[program.include_count++] = copy;
}

// ".slang" is Slang, anything else GLSL.
ShaderLanguage language_of(const char* path) {
    const usz length = strlen(path);
    static constexpr const char* SLANG = ".slang";
    const usz suffix = strlen(SLANG);
    return length >= suffix && strcmp(path + length - suffix, SLANG) == 0 ? SHADER_LANGUAGE_SLANG : SHADER_LANGUAGE_GLSL;
}

// Reads the whole file into an allocator buffer, null-terminated; for GLSL
// with `#include` enabled (SHADER_PREPROCESSING::enable_includes).
char* read_source(const char* path, BaseAllocator* allocator, usz* out_size, ShaderProgram& program) {
    File file;
    if (!PLATFORM::file_open(&file, path, FILE_ACCESS_READ)) {
        append_logf(program, "%s: cannot open file", path);
        return nullptr;
    }
    u64 size = 0;
    if (!PLATFORM::file_size(file, &size) || size == 0) {
        PLATFORM::file_close(&file);
        append_logf(program, "%s: empty file", path);
        return nullptr;
    }
    char* text = allocator->allocate_array<char>(static_cast<usz>(size) + SHADER_PREPROCESSING::INCLUDE_EXTENSION_LENGTH + 1);
    const bool read = PLATFORM::file_read(file, 0, text, static_cast<usz>(size));
    PLATFORM::file_close(&file);
    if (!read) {
        allocator->free(text);
        append_logf(program, "%s: read failed", path);
        return nullptr;
    }
    text[size] = '\0';
    *out_size = program.language == SHADER_LANGUAGE_GLSL ? SHADER_PREPROCESSING::enable_includes(text, static_cast<usz>(size)) : static_cast<usz>(size);
    return text;
}

bool compile_stage(const ShaderProgramLoadDesc& desc, ShaderProgram& program, const char* text, const usz size, const ShaderStage stage, const char* pass_name, CompiledShader& out) {
    char pass_define[SHADER_PREPROCESSING::PASS_DEFINE_MAX];
    SHADER_PREPROCESSING::pass_define(pass_name, pass_define, sizeof(pass_define));
    const ShaderDefine defines[2] = {{SHADER_PREPROCESSING::stage_define(stage), "1"}, {pass_define, "1"}};

    ShaderCompileOptions options;
    options.defines = defines;
    options.define_count = 2;
    options.optimize = desc.optimize;
    options.debug_info = desc.debug_info;
    options.include_dirs = desc.include_dirs;
    options.include_dir_count = desc.include_dir_count;
    options.on_include = record_include;
    options.on_include_user_data = &program;

    ShaderSource source;
    source.text = text;
    source.size = size;
    source.name = program.path;
    source.stage = stage;
    source.language = program.language;
    source.entry_point = program.language == SHADER_LANGUAGE_SLANG ? SHADER_PREPROCESSING::stage_entry_point(stage) : "main";

    out = SHADER_COMPILER::compile(source, options, program.allocator);
    if (out.log != nullptr) {
        append_logf(program, "[%s/%s]", pass_name, SHADER::stage_name(stage));
        append_log(program, out.log);
    }
    return out.is_valid();
}

// The layout of `set` as `reflection` declares it, with every binding
// visible to both stages: a set bound for a vertex-only pass and for a
// full pass must be the same layout, whatever each pass's stages read.
VkDescriptorSetLayout layout_for_set(const ShaderProgramLoadDesc& desc, const ShaderReflection& reflection, const u32 set) {
    DescriptorLayoutDesc layout;
    layout.count = SHADER_REFLECT::layout_bindings(reflection, set, layout.bindings, DESCRIPTOR_LAYOUT_MAX_BINDINGS);
    if (layout.count > DESCRIPTOR_LAYOUT_MAX_BINDINGS) {
        return VK_NULL_HANDLE;
    }
    for (u32 i = 0; i < layout.count; ++i) {
        layout.bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    return desc.layouts->get(layout);
}

bool declares_set(const ShaderReflection& reflection, const u32 set) {
    return (reflection.set_mask() & (1u << set)) != 0;
}

} // namespace

// --- ShaderProgram ------------------------------------------------------------------

const ShaderPassProgram* ShaderProgram::find_pass(const u64 tag) const {
    for (u32 i = 0; i < this->pass_count; ++i) {
        if (this->passes[i].tag == tag) {
            return &this->passes[i];
        }
    }
    return nullptr;
}

void ShaderProgram::free() {
    for (u32 i = 0; i < this->pass_count; ++i) {
        this->passes[i].vertex.free();
        this->passes[i].fragment.free();
    }
    this->pass_count = 0;
    if (this->allocator != nullptr) {
        for (u32 i = 0; i < this->include_count; ++i) {
            this->allocator->free(this->includes[i]);
        }
        this->allocator->free(this->log);
    }
    this->include_count = 0;
    this->log = nullptr;
    this->material_layout = VK_NULL_HANDLE;
    this->material_block_binding = 0xffffffffu;
    this->material_block_size = 0;
    this->material_interface = ShaderReflection{};
}

// --- SHADER_PROGRAM ------------------------------------------------------------------

bool SHADER_PROGRAM::load(const ShaderProgramLoadDesc& desc, BaseAllocator* allocator, ShaderProgram& out) {
    if (allocator == nullptr) {
        allocator = MEMORY::heap_allocator();
    }
    out = ShaderProgram{};
    out.allocator = allocator;
    if (desc.path == nullptr || strlen(desc.path) >= SHADER_PROGRAM_PATH_MAX) {
        append_log(out, "shader path missing or too long");
        return false;
    }
    strcpy(out.path, desc.path);
    out.language = language_of(desc.path);
    if (desc.layouts == nullptr || desc.frame_layout == VK_NULL_HANDLE) {
        append_log(out, "load needs a layout cache and the frame layout");
        return false;
    }

    usz size = 0;
    char* text = read_source(desc.path, allocator, &size, out);
    if (text == nullptr) {
        return false;
    }

    bool ok = true;
    ShaderDirectives directives;
    if (!SHADER_PREPROCESSING::parse_directives(text, size, directives)) {
        append_logf(out, "%s: %s", desc.path, directives.error);
        ok = false;
    }
    SHADER_PREPROCESSING::strip_directives(text, size);

    for (u32 p = 0; ok && p < directives.pass_count; ++p) {
        const ShaderPassDirective& directive = directives.passes[p];
        ShaderPassProgram& pass = out.passes[out.pass_count];
        pass = ShaderPassProgram{};
        strcpy(pass.tag_name, directive.name);
        pass.tag = HASH::fnv1a_str(directive.name);

        if (directive.stages & SHADER_STAGE_MASK_COMPUTE) {
            append_logf(out, "%s: pass '%s': compute stages are not supported yet", desc.path, directive.name);
            ok = false;
            break;
        }
        if ((directive.stages & SHADER_STAGE_MASK_VERTEX) == 0) {
            append_logf(out, "%s: pass '%s': a pass needs a vertex stage", desc.path, directive.name);
            ok = false;
            break;
        }
        if (!compile_stage(desc, out, text, size, SHADER_STAGE_VERTEX, directive.name, pass.vertex)) {
            ok = false;
        }
        if (ok && (directive.stages & SHADER_STAGE_MASK_FRAGMENT) != 0 &&
            !compile_stage(desc, out, text, size, SHADER_STAGE_FRAGMENT, directive.name, pass.fragment)) {
            ok = false;
        }
        if (!ok) {
            pass.vertex.free();
            pass.fragment.free();
            break;
        }

        // Reflection.
        char error[256];
        if (!SHADER_REFLECT::merge(pass.vertex, pass.reflection, error, sizeof(error)) ||
            (pass.fragment.is_valid() && !SHADER_REFLECT::merge(pass.fragment, pass.reflection, error, sizeof(error)))) {
            append_logf(out, "%s: pass '%s': %s", desc.path, directive.name, error);
            pass.vertex.free();
            pass.fragment.free();
            ok = false;
            break;
        }
        for (u32 b = 0; b < pass.reflection.binding_count && ok; ++b) {
            const ReflectedBinding& binding = pass.reflection.bindings[b];
            if (binding.set == 0 && desc.frame_interface != nullptr) {
                const ReflectedBinding* frame_binding = desc.frame_interface->find_binding(0, binding.binding);
                if (frame_binding == nullptr || frame_binding->type != binding.type) {
                    append_logf(out, "%s: pass '%s': set 0 binding %u is not part of the frame interface (see engine/frame.glsl)",
                        desc.path, directive.name, binding.binding);
                    ok = false;
                }
            }
            if (binding.set == 3) {
                append_logf(out, "%s: pass '%s': set 3 is reserved", desc.path, directive.name);
                ok = false;
            }
        }
        if (ok && pass.reflection.push_constant_size > SHADER_PROGRAM_MAX_PUSH_CONSTANTS) {
            append_logf(out, "%s: pass '%s': push constants exceed %u bytes", desc.path, directive.name, SHADER_PROGRAM_MAX_PUSH_CONSTANTS);
            ok = false;
        }
        if (!ok) {
            pass.vertex.free();
            pass.fragment.free();
            break;
        }

        if (pass.reflection.push_constant_size > 0) {
            pass.push_constants.stageFlags = pass.reflection.push_constant_stages;
            pass.push_constants.offset = pass.reflection.push_constant_offset;
            pass.push_constants.size = pass.reflection.push_constant_size;
            pass.has_push_constants = true;
        }
        out.pass_count += 1;
    }

    // The material interface: set 2 of the first pass that declares it,
    // identical in every other pass that does. A pass may leave it out (a
    // depth-only pass needs no material) and then gets no material set.
    const ShaderReflection* material_pass = nullptr;
    if (ok) {
        for (u32 p = 0; p < out.pass_count; ++p) {
            if (!declares_set(out.passes[p].reflection, 2)) {
                continue;
            }
            if (material_pass == nullptr) {
                material_pass = &out.passes[p].reflection;
            } else if (!SHADER_REFLECT::same_set(*material_pass, out.passes[p].reflection, 2)) {
                append_logf(out, "%s: pass '%s' declares a different material set (set 2) than an earlier pass", desc.path, out.passes[p].tag_name);
                ok = false;
            }
        }
        if (ok && material_pass != nullptr) {
            const ShaderReflection& first = *material_pass;
            // Copy the set-2 bindings and their members.
            for (u32 b = 0; b < first.binding_count; ++b) {
                const ReflectedBinding& binding = first.bindings[b];
                if (binding.set != 2) {
                    continue;
                }
                ReflectedBinding copy = binding;
                copy.first_member = out.material_interface.member_count;
                for (u32 m = 0; m < binding.member_count; ++m) {
                    const ReflectedMember& member = first.members[binding.first_member + m];
                    if (member.name[0] == '\0') {
                        append_logf(out, "%s: the material block has an unnamed member; compile without optimization", desc.path);
                        ok = false;
                    }
                    out.material_interface.members[out.material_interface.member_count++] = member;
                }
                if (binding.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                    if (out.has_material_block()) {
                        append_logf(out, "%s: the material set may hold only one uniform block", desc.path);
                        ok = false;
                    }
                    out.material_block_binding = binding.binding;
                    out.material_block_size = binding.block_size;
                } else if (binding.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                    append_logf(out, "%s: material set bindings must be a uniform block or combined image samplers", desc.path);
                    ok = false;
                }
                out.material_interface.bindings[out.material_interface.binding_count++] = copy;
            }
            if (ok && out.material_interface.binding_count > 0) {
                out.material_layout = layout_for_set(desc, first, 2);
                if (out.material_layout == VK_NULL_HANDLE) {
                    append_logf(out, "%s: could not create the material set layout", desc.path);
                    ok = false;
                }
            }
        }
    }

    // Layouts per pass: set 0 is always the frame's, set 1 the pass's
    // inputs, set 2 the shared material layout; empty layouts fill gaps.
    for (u32 p = 0; ok && p < out.pass_count; ++p) {
        ShaderPassProgram& pass = out.passes[p];
        const u32 used = pass.reflection.set_count();
        pass.set_layout_count = used > 1 ? used : 1;
        pass.set_layouts[0] = desc.frame_layout;
        for (u32 s = 1; s < pass.set_layout_count; ++s) {
            if (s == 2 && declares_set(pass.reflection, 2)) {
                pass.set_layouts[s] = out.material_layout;
            } else if (declares_set(pass.reflection, s)) {
                pass.set_layouts[s] = layout_for_set(desc, pass.reflection, s);
            } else {
                pass.set_layouts[s] = desc.layouts->empty_layout();
            }
            if (pass.set_layouts[s] == VK_NULL_HANDLE) {
                append_logf(out, "%s: pass '%s': could not create the layout for set %u", desc.path, pass.tag_name, s);
                ok = false;
            }
        }
    }

    allocator->free(text);
    if (!ok) {
        // Keep the log, drop the rest.
        for (u32 i = 0; i < out.pass_count; ++i) {
            out.passes[i].vertex.free();
            out.passes[i].fragment.free();
        }
        out.pass_count = 0;
        out.material_interface = ShaderReflection{};
        out.material_layout = VK_NULL_HANDLE;
        out.material_block_binding = 0xffffffffu;
        out.material_block_size = 0;
        return false;
    }
    return true;
}
