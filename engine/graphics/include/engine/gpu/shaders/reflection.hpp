#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/shaders/compilation.hpp"

#include <volk.h>

// What a shader declares, read back from its SPIR-V (spirv-reflect, confined
// to reflection.cpp): descriptor bindings with the members of their
// uniform blocks, the push constant range, and the vertex inputs. One
// ShaderReflection describes a whole program: merge() folds each stage in,
// so a binding used by both stages carries both stage bits.
//
// Fixed arrays throughout, so a reflection can sit inside a component.

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

// A top-level member of a uniform block. `columns` x `rows` gives the shape:
// a scalar is 1x1, vec3 is 1x3, mat4 is 4x4. `size` covers arrays.
struct ReflectedMember {
    char name[REFLECT_NAME_MAX] = {};
    u32 offset = 0;
    u32 size = 0;
    ReflectedScalar scalar = REFLECT_SCALAR_FLOAT;
    u8 columns = 1;
    u8 rows = 1;
};

struct ReflectedBinding {
    u32 set = 0;
    u32 binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    u32 count = 1;
    VkShaderStageFlags stages = 0;
    // The variable's name (for images) or the block's instance name.
    char name[REFLECT_NAME_MAX] = {};
    // Uniform blocks: std140 size and members (indices into
    // ShaderReflection::members).
    u32 block_size = 0;
    u32 first_member = 0;
    u32 member_count = 0;
};

struct ReflectedInput {
    u32 location = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    ReflectedScalar scalar = REFLECT_SCALAR_FLOAT;
    u8 components = 1;
    char name[REFLECT_NAME_MAX] = {};
};

struct ShaderReflection {
    ReflectedBinding bindings[REFLECT_MAX_BINDINGS] = {};
    u32 binding_count = 0;
    ReflectedMember members[REFLECT_MAX_MEMBERS] = {};
    u32 member_count = 0;
    // Vertex stage inputs, built-ins skipped.
    ReflectedInput inputs[REFLECT_MAX_INPUTS] = {};
    u32 input_count = 0;
    u32 input_location_mask = 0;
    u32 push_constant_offset = 0;
    u32 push_constant_size = 0;
    VkShaderStageFlags push_constant_stages = 0;

    const ReflectedBinding* find_binding(u32 set, const char* name) const;
    const ReflectedBinding* find_binding(u32 set, u32 binding) const;
    const ReflectedMember* find_member(const ReflectedBinding& block, const char* name) const;
    // Bit s set when the shader uses descriptor set s.
    u32 set_mask() const;
    // The highest set index used plus one, or 0.
    u32 set_count() const;
};

namespace SHADER_REFLECT {

// Reflects `shader` into `out`, merging with what is already there: a
// binding present in both must agree in type and count (false with
// `error` otherwise) and gains the stage; the push constant range grows to
// cover both. Vertex inputs come from the vertex stage only.
bool merge(const CompiledShader& shader, ShaderReflection& out, char* error, usz error_size);

// Writes the VkDescriptorSetLayoutBindings of set `set` into `out`, sorted
// by binding, at most `max` of them. Returns how many the set has.
u32 layout_bindings(const ShaderReflection& reflection, u32 set, VkDescriptorSetLayoutBinding* out, u32 max);

// Whether the bindings of `set` in `a` and `b` are the same interface:
// same bindings with the same types, counts, names and block layouts.
bool same_set(const ShaderReflection& a, const ShaderReflection& b, u32 set);

const char* scalar_name(ReflectedScalar scalar);

} // namespace SHADER_REFLECT
