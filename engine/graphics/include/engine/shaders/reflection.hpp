#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/gpu_types.hpp"
#include "engine/shaders/compilation.hpp"

// Step three of the shader path: what a compiled stage binds and reads,
// merged across the stages of a pass into one ShaderReflection. Plain
// fixed-size arrays, API-neutral: binding types are GpuBindingType, vertex
// input formats GpuFormat, stage masks ShaderStageMask bits (the same bit
// values as GpuShaderStage). spirv-reflect is confined to reflection.cpp;
// other bytecode kinds get their own reflection behind the same merge().
//
// Bindings carry their uniform block members (name, offset, size, scalar
// type and shape) so that values can be set by name (the material system)
// and a set 0 can be checked against the frame interface. A combined image
// sampler (Slang's Sampler2D) is reported as a TEXTURE with
// `combined_sampler` set: D3D12 and Metal have no such thing, so the
// engine's convention is Texture2D + SamplerState and layout_bindings()
// refuses combined ones.

static constexpr u32 REFLECT_NAME_MAX = 48;
static constexpr u32 REFLECT_MAX_BINDINGS = 24;
static constexpr u32 REFLECT_MAX_MEMBERS = 64;
static constexpr u32 REFLECT_MAX_INPUTS = 16;
static constexpr u32 REFLECT_MAX_SETS = 4;

enum ReflectedScalar : u8 {
    REFLECT_SCALAR_FLOAT = 0,
    REFLECT_SCALAR_INT = 1,
    REFLECT_SCALAR_UINT = 2,
    REFLECT_SCALAR_BOOL = 3,
};

struct ReflectedMember {
    char name[REFLECT_NAME_MAX] = {};
    u32 offset = 0;
    u32 size = 0;
    ReflectedScalar scalar = REFLECT_SCALAR_FLOAT;
    // 1x1 scalar, 1xN vector, MxN matrix (columns x rows).
    u8 columns = 1;
    u8 rows = 1;
};

struct ReflectedBinding {
    u32 set = 0;
    u32 binding = 0;
    GpuBindingType type = GPU_BINDING_UNIFORM_BUFFER;
    // A combined image sampler declared as one binding (see above).
    bool combined_sampler = false;
    u32 count = 1;
    // ShaderStageMask bits of the stages that declare it.
    u8 stages = 0;
    char name[REFLECT_NAME_MAX] = {};
    // Uniform / storage blocks: size and members[first_member, +member_count).
    u32 block_size = 0;
    u32 first_member = 0;
    u32 member_count = 0;
};

struct ReflectedInput {
    u32 location = 0;
    // The format the input is declared as, UNDEFINED when it has no GpuFormat.
    GpuFormat format = GPU_FORMAT_UNDEFINED;
    ReflectedScalar scalar = REFLECT_SCALAR_FLOAT;
    u8 components = 1;
    char name[REFLECT_NAME_MAX] = {};
};

struct ShaderReflection {
    ReflectedBinding bindings[REFLECT_MAX_BINDINGS] = {};
    u32 binding_count = 0;
    ReflectedMember members[REFLECT_MAX_MEMBERS] = {};
    u32 member_count = 0;
    // Vertex stage only.
    ReflectedInput inputs[REFLECT_MAX_INPUTS] = {};
    u32 input_count = 0;
    u32 input_location_mask = 0;
    // One range covering every stage's push constant block.
    u32 push_constant_offset = 0;
    u32 push_constant_size = 0;
    u8 push_constant_stages = 0;

    const ReflectedBinding* find_binding(u32 set, const char* name) const;
    const ReflectedBinding* find_binding(u32 set, u32 binding) const;
    const ReflectedMember* find_member(const ReflectedBinding& block, const char* name) const;
    // Bit s set when any binding uses set s.
    u32 set_mask() const;
    // Highest used set + 1.
    u32 set_count() const;
};

namespace SHADER_REFLECT {

// Folds `shader`'s bindings, push constants and (vertex stage) inputs into
// `out`. A binding declared by two stages must agree on its type and count.
bool merge(const CompiledShader& shader, ShaderReflection& out, char* error, usz error_size);
// The bind layout of `set` as `reflection` declares it, every slot visible
// to the vertex and fragment stages so that a set bound for a vertex-only
// pass and for a full pass is the same layout. False (with `error`) for a
// combined image sampler or too many slots.
bool layout_bindings(const ShaderReflection& reflection, u32 set, GpuBindLayoutDesc& out, char* error, usz error_size);
// Whether `a` and `b` declare the same bindings (types, counts, block
// members) in `set`.
bool same_set(const ShaderReflection& a, const ShaderReflection& b, u32 set);
const char* scalar_name(ReflectedScalar scalar);

} // namespace SHADER_REFLECT
