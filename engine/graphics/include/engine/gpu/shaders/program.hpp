#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/descriptor.hpp"
#include "engine/gpu/shaders/compilation.hpp"
#include "engine/gpu/shaders/preprocessing.hpp"
#include "engine/gpu/shaders/reflection.hpp"
#include "engine/memory/base_allocator.hpp"

#include <volk.h>

// A shader file loaded for every pass it declares: compiled stages,
// reflection and descriptor set layouts per pass, plus the material
// interface shared by all of them. ECS-free plain data; the runtime wraps
// it in a Shader component. The language is the file's extension: `.slang`
// (the engine's shaders; one file holds the `vertex` and `fragment`
// functions) or `.glsl` (one `main` per #ifdef STAGE_* section). See
// preprocessing.hpp for the pass directives both share.
//
//     ShaderProgramLoadDesc desc;
//     desc.path = "render/unlit.slang";
//     desc.include_dirs = roots; desc.include_dir_count = 2;
//     desc.layouts = &layout_cache;
//     desc.frame_layout = frame_set_layout;
//     ShaderProgram program;
//     if (!SHADER_PROGRAM::load(desc, allocator, program)) { print(program.log); }
//
// The set convention every shader follows (sets above 3 are refused; in
// Slang the binding is `[[vk::binding(b, set)]]`, see render/engine/bindings.slang):
//
//     set 0  per frame: the renderer's FrameUniforms block
//            (engine/frame.slang); what a shader declares here must be a
//            subset of it
//     set 1  per pass: the graph's inputs as combined image samplers,
//            binding i = input i
//     set 2  per material: one uniform block plus samplers, the same in
//            every pass of the shader that declares it (a depth-only pass
//            may leave it out); Renderer::set_* writes it by member name
//     set 3  reserved
//
// Set layouts are built with every binding visible to the vertex and
// fragment stages, so one layout serves a pass that reads a binding in both
// stages and one that reads it in the vertex stage only.
//
// Per-object data is one push constant block (engine/object.slang).

static constexpr u32 SHADER_PROGRAM_MAX_INCLUDES = 32;
static constexpr u32 SHADER_PROGRAM_PATH_MAX = 512;
static constexpr u32 SHADER_PROGRAM_MAX_PUSH_CONSTANTS = 128;

struct ShaderPassProgram {
    // HASH::fnv1a_str of the pass tag ("forward").
    u64 tag = 0;
    char tag_name[SHADER_PREPROCESSING::PASS_NAME_MAX] = {};
    // Allocator-owned words, freed by ShaderProgram::free.
    CompiledShader vertex;
    // Invalid (no code) for a vertex-only pass.
    CompiledShader fragment;
    ShaderReflection reflection;
    // One per set index up to the highest the pass uses, gaps filled with
    // the cache's empty layout; owned by the DescriptorLayoutCache. Index 2
    // is the program's material_layout when the pass declares set 2.
    VkDescriptorSetLayout set_layouts[REFLECT_MAX_SETS] = {};
    u32 set_layout_count = 0;

    bool uses_material() const { return this->set_layout_count > 2 && (this->reflection.set_mask() & 4u) != 0; }
    VkPushConstantRange push_constants = {};
    bool has_push_constants = false;
};

struct ShaderProgram {
    char path[SHADER_PROGRAM_PATH_MAX] = {};
    // From the extension of `path`.
    ShaderLanguage language = SHADER_LANGUAGE_SLANG;
    ShaderPassProgram passes[SHADER_PREPROCESSING::MAX_PASSES] = {};
    u32 pass_count = 0;

    // Set 2 of the first pass (every pass has the same): the bindings a
    // material fills and the block members it sets by name.
    ShaderReflection material_interface;
    VkDescriptorSetLayout material_layout = VK_NULL_HANDLE;
    // The uniform block in set 2, or UINT32_MAX when the shader has none.
    u32 material_block_binding = 0xffffffffu;
    u32 material_block_size = 0;

    // Resolved paths of every file the preprocessor opened, for
    // reload-on-change. Heap copies on `allocator`.
    char* includes[SHADER_PROGRAM_MAX_INCLUDES] = {};
    u32 include_count = 0;
    // Everything the compiler and the loader had to say, or nullptr.
    char* log = nullptr;
    BaseAllocator* allocator = nullptr;

    bool is_valid() const { return this->pass_count > 0; }
    const ShaderPassProgram* find_pass(u64 tag) const;
    bool has_material_block() const { return this->material_block_binding != 0xffffffffu; }

    // Releases the compiled stages, the include list and the log. Layouts
    // belong to the cache.
    void free();
};

struct ShaderProgramLoadDesc {
    // The file to load, as resolved by the caller.
    const char* path = nullptr;
    const char* const* include_dirs = nullptr;
    usz include_dir_count = 0;
    // Where set layouts come from.
    DescriptorLayoutCache* layouts = nullptr;
    // The renderer's set 0 layout and its reflected interface. Every pass
    // gets `frame_layout` at index 0; a shader's own set 0 declarations
    // must match bindings in `frame_interface` (may be null to skip).
    VkDescriptorSetLayout frame_layout = VK_NULL_HANDLE;
    const ShaderReflection* frame_interface = nullptr;
    // Off by default: optimization may strip the names reflection needs.
    bool optimize = false;
    bool debug_info = false;
};

namespace SHADER_PROGRAM {

// Reads, compiles and reflects the file. True on success, with `out.log`
// holding any warnings; false leaves `out` empty except for `log`, which
// says what went wrong. Nothing is printed.
bool load(const ShaderProgramLoadDesc& desc, BaseAllocator* allocator, ShaderProgram& out);

} // namespace SHADER_PROGRAM
