// spirv-reflect is confined to this file; the header exposes plain structs.

#include "engine/gpu/shaders/reflection.hpp"

#include "engine/gpu/shaders/module.hpp"

#include <spirv_reflect.h>

#include <cstdio>
#include <cstring>

// --- Helpers ---------------------------------------------------------------------

static void copy_name(char* out, const char* name) {
    if (name == nullptr) {
        out[0] = '\0';
        return;
    }
    strncpy(out, name, REFLECT_NAME_MAX - 1);
    out[REFLECT_NAME_MAX - 1] = '\0';
}

static bool set_error(char* error, const usz error_size, const char* message) {
    if (error != nullptr && error_size > 0) {
        snprintf(error, error_size, "%s", message);
    }
    return false;
}

static ReflectedScalar scalar_of(const SpvReflectTypeDescription* type) {
    if (type == nullptr) {
        return REFLECT_SCALAR_FLOAT;
    }
    if (type->type_flags & SPV_REFLECT_TYPE_FLAG_BOOL) {
        return REFLECT_SCALAR_BOOL;
    }
    if (type->type_flags & SPV_REFLECT_TYPE_FLAG_INT) {
        return type->traits.numeric.scalar.signedness != 0 ? REFLECT_SCALAR_INT : REFLECT_SCALAR_UINT;
    }
    return REFLECT_SCALAR_FLOAT;
}

// --- ShaderReflection ------------------------------------------------------------

const ReflectedBinding* ShaderReflection::find_binding(const u32 set, const char* name) const {
    if (name == nullptr) {
        return nullptr;
    }
    for (u32 i = 0; i < this->binding_count; ++i) {
        if (this->bindings[i].set == set && strcmp(this->bindings[i].name, name) == 0) {
            return &this->bindings[i];
        }
    }
    return nullptr;
}

const ReflectedBinding* ShaderReflection::find_binding(const u32 set, const u32 binding) const {
    for (u32 i = 0; i < this->binding_count; ++i) {
        if (this->bindings[i].set == set && this->bindings[i].binding == binding) {
            return &this->bindings[i];
        }
    }
    return nullptr;
}

const ReflectedMember* ShaderReflection::find_member(const ReflectedBinding& block, const char* name) const {
    if (name == nullptr) {
        return nullptr;
    }
    for (u32 i = 0; i < block.member_count; ++i) {
        const ReflectedMember& member = this->members[block.first_member + i];
        if (strcmp(member.name, name) == 0) {
            return &member;
        }
    }
    return nullptr;
}

u32 ShaderReflection::set_mask() const {
    u32 mask = 0;
    for (u32 i = 0; i < this->binding_count; ++i) {
        if (this->bindings[i].set < 32) {
            mask |= 1u << this->bindings[i].set;
        }
    }
    return mask;
}

u32 ShaderReflection::set_count() const {
    u32 count = 0;
    for (u32 i = 0; i < this->binding_count; ++i) {
        if (this->bindings[i].set + 1 > count) {
            count = this->bindings[i].set + 1;
        }
    }
    return count;
}

// --- SHADER_REFLECT ----------------------------------------------------------------

const char* SHADER_REFLECT::scalar_name(const ReflectedScalar scalar) {
    switch (scalar) {
    case REFLECT_SCALAR_FLOAT:
        return "float";
    case REFLECT_SCALAR_INT:
        return "int";
    case REFLECT_SCALAR_UINT:
        return "uint";
    case REFLECT_SCALAR_BOOL:
        return "bool";
    }
    return "unknown";
}

bool SHADER_REFLECT::merge(const CompiledShader& shader, ShaderReflection& out, char* error, const usz error_size) {
    if (!shader.is_valid()) {
        return set_error(error, error_size, "shader is not compiled");
    }
    SpvReflectShaderModule module;
    if (spvReflectCreateShaderModule(shader.byte_size(), shader.code, &module) != SPV_REFLECT_RESULT_SUCCESS) {
        return set_error(error, error_size, "spirv-reflect could not parse the SPIR-V");
    }
    const VkShaderStageFlags stage = SHADER::to_vk_stage(shader.stage);
    bool ok = true;

    // Descriptor bindings.
    u32 binding_count = 0;
    spvReflectEnumerateDescriptorBindings(&module, &binding_count, nullptr);
    SpvReflectDescriptorBinding* bindings[REFLECT_MAX_BINDINGS * 2];
    if (binding_count > REFLECT_MAX_BINDINGS * 2) {
        binding_count = REFLECT_MAX_BINDINGS * 2;
    }
    spvReflectEnumerateDescriptorBindings(&module, &binding_count, bindings);
    for (u32 i = 0; i < binding_count && ok; ++i) {
        const SpvReflectDescriptorBinding& b = *bindings[i];
        const VkDescriptorType type = static_cast<VkDescriptorType>(b.descriptor_type);
        u32 count = 1;
        for (u32 d = 0; d < b.array.dims_count; ++d) {
            count *= b.array.dims[d];
        }
        if (b.set >= REFLECT_MAX_SETS) {
            ok = set_error(error, error_size, "descriptor set index is above the engine's limit of 4 sets");
            break;
        }

        ReflectedBinding* existing = nullptr;
        for (u32 e = 0; e < out.binding_count; ++e) {
            if (out.bindings[e].set == b.set && out.bindings[e].binding == b.binding) {
                existing = &out.bindings[e];
            }
        }
        if (existing != nullptr) {
            if (existing->type != type || existing->count != count) {
                ok = set_error(error, error_size, "a binding is declared with different types in two stages");
                break;
            }
            existing->stages |= stage;
            continue;
        }
        if (out.binding_count >= REFLECT_MAX_BINDINGS) {
            ok = set_error(error, error_size, "too many descriptor bindings");
            break;
        }
        ReflectedBinding& entry = out.bindings[out.binding_count];
        entry = ReflectedBinding{};
        entry.set = b.set;
        entry.binding = b.binding;
        entry.type = type;
        entry.count = count;
        entry.stages = stage;
        // Blocks are named by their instance ("material"), images by their
        // variable; the type name is the fallback for anonymous instances.
        copy_name(entry.name, b.name != nullptr && b.name[0] != '\0' ? b.name : (b.type_description != nullptr ? b.type_description->type_name : nullptr));
        if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            entry.block_size = b.block.size;
            entry.first_member = out.member_count;
            for (u32 m = 0; m < b.block.member_count; ++m) {
                if (out.member_count >= REFLECT_MAX_MEMBERS) {
                    ok = set_error(error, error_size, "too many uniform block members");
                    break;
                }
                const SpvReflectBlockVariable& v = b.block.members[m];
                ReflectedMember& member = out.members[out.member_count++];
                member = ReflectedMember{};
                copy_name(member.name, v.name);
                member.offset = v.offset;
                member.size = v.size;
                member.scalar = scalar_of(v.type_description);
                member.columns = 1;
                member.rows = 1;
                if (v.type_description != nullptr) {
                    if (v.type_description->type_flags & SPV_REFLECT_TYPE_FLAG_MATRIX) {
                        member.columns = static_cast<u8>(v.numeric.matrix.column_count);
                        member.rows = static_cast<u8>(v.numeric.matrix.row_count);
                    } else if (v.type_description->type_flags & SPV_REFLECT_TYPE_FLAG_VECTOR) {
                        member.rows = static_cast<u8>(v.numeric.vector.component_count);
                    }
                }
                entry.member_count += 1;
            }
        }
        out.binding_count += 1;
    }

    // Push constants: one range covering every stage's block.
    if (ok) {
        u32 block_count = 0;
        spvReflectEnumeratePushConstantBlocks(&module, &block_count, nullptr);
        SpvReflectBlockVariable* blocks[4];
        if (block_count > 4) {
            block_count = 4;
        }
        spvReflectEnumeratePushConstantBlocks(&module, &block_count, blocks);
        for (u32 i = 0; i < block_count; ++i) {
            const u32 begin = blocks[i]->offset;
            const u32 end = blocks[i]->offset + blocks[i]->size;
            if (out.push_constant_size == 0) {
                out.push_constant_offset = begin;
                out.push_constant_size = end - begin;
            } else {
                const u32 old_end = out.push_constant_offset + out.push_constant_size;
                out.push_constant_offset = begin < out.push_constant_offset ? begin : out.push_constant_offset;
                out.push_constant_size = (end > old_end ? end : old_end) - out.push_constant_offset;
            }
            out.push_constant_stages |= stage;
        }
    }

    // Vertex inputs.
    if (ok && shader.stage == SHADER_STAGE_VERTEX) {
        u32 input_count = 0;
        spvReflectEnumerateInputVariables(&module, &input_count, nullptr);
        SpvReflectInterfaceVariable* inputs[REFLECT_MAX_INPUTS * 4];
        if (input_count > REFLECT_MAX_INPUTS * 4) {
            input_count = REFLECT_MAX_INPUTS * 4;
        }
        spvReflectEnumerateInputVariables(&module, &input_count, inputs);
        for (u32 i = 0; i < input_count; ++i) {
            const SpvReflectInterfaceVariable& v = *inputs[i];
            if (v.built_in != -1 || (v.decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) != 0 || v.location == 0xffffffffu) {
                continue;
            }
            if (out.input_count >= REFLECT_MAX_INPUTS) {
                ok = set_error(error, error_size, "too many vertex inputs");
                break;
            }
            ReflectedInput& input = out.inputs[out.input_count++];
            input = ReflectedInput{};
            input.location = v.location;
            input.format = static_cast<VkFormat>(v.format);
            input.scalar = scalar_of(v.type_description);
            input.components = static_cast<u8>(v.numeric.vector.component_count != 0 ? v.numeric.vector.component_count : 1);
            copy_name(input.name, v.name);
            if (v.location < 32) {
                out.input_location_mask |= 1u << v.location;
            }
        }
    }

    spvReflectDestroyShaderModule(&module);
    return ok;
}

u32 SHADER_REFLECT::layout_bindings(const ShaderReflection& reflection, const u32 set, VkDescriptorSetLayoutBinding* out, const u32 max) {
    u32 count = 0;
    for (u32 i = 0; i < reflection.binding_count; ++i) {
        const ReflectedBinding& b = reflection.bindings[i];
        if (b.set != set) {
            continue;
        }
        if (count < max) {
            // Insert sorted by binding index.
            u32 at = count;
            while (at > 0 && out[at - 1].binding > b.binding) {
                out[at] = out[at - 1];
                --at;
            }
            out[at] = VkDescriptorSetLayoutBinding{};
            out[at].binding = b.binding;
            out[at].descriptorType = b.type;
            out[at].descriptorCount = b.count;
            out[at].stageFlags = b.stages;
        }
        count += 1;
    }
    return count;
}

bool SHADER_REFLECT::same_set(const ShaderReflection& a, const ShaderReflection& b, const u32 set) {
    u32 count_a = 0;
    for (u32 i = 0; i < a.binding_count; ++i) {
        const ReflectedBinding& x = a.bindings[i];
        if (x.set != set) {
            continue;
        }
        count_a += 1;
        const ReflectedBinding* y = b.find_binding(set, x.binding);
        if (y == nullptr || y->type != x.type || y->count != x.count || y->block_size != x.block_size ||
            y->member_count != x.member_count || strcmp(x.name, y->name) != 0) {
            return false;
        }
        for (u32 m = 0; m < x.member_count; ++m) {
            const ReflectedMember& mx = a.members[x.first_member + m];
            const ReflectedMember& my = b.members[y->first_member + m];
            if (mx.offset != my.offset || mx.size != my.size || mx.scalar != my.scalar || strcmp(mx.name, my.name) != 0) {
                return false;
            }
        }
    }
    u32 count_b = 0;
    for (u32 i = 0; i < b.binding_count; ++i) {
        count_b += b.bindings[i].set == set;
    }
    return count_a == count_b;
}
