#include "engine/render/materials.hpp"

#include "engine/asset/text_asset.hpp"
#include "engine/ecs/world.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/render/components.hpp"
#include "engine/render/renderer.hpp"
#include "engine/scene/scene.hpp"
#include "engine/templates/dynamic_array.hpp"

#include <cstdio>
#include <cstring>

// --- Sync ------------------------------------------------------------------------

void MATERIAL::sync(Material& material, const Shader& shader) {
    if (material.shader_generation == shader.generation && (material.params != nullptr || !shader.program.has_material_block())) {
        return;
    }
    const ShaderProgram& program = shader.program;
    MEMORY::heap_allocator()->free(material.params);
    material.params = nullptr;
    material.param_size = 0;
    material.shader_generation = shader.generation;
    if (program.has_material_block()) {
        material.param_size = program.material_block_size;
        material.params = MEMORY::heap_allocator()->allocate_array<u8>(material.param_size);
        memset(material.params, 0, material.param_size);
    }

    // Keep the texture and sampler assignments of slots that still exist.
    MaterialTextureSlot old_textures[MATERIAL_MAX_TEXTURES];
    memcpy(old_textures, material.textures, sizeof(old_textures));
    const u32 old_texture_count = material.texture_count;
    MaterialSamplerSlot old_samplers[MATERIAL_MAX_SAMPLERS];
    memcpy(old_samplers, material.samplers, sizeof(old_samplers));
    const u32 old_sampler_count = material.sampler_count;
    material.texture_count = 0;
    material.sampler_count = 0;

    const ShaderReflection& interface = program.material_interface;
    for (u32 b = 0; b < interface.binding_count; ++b) {
        const ReflectedBinding& binding = interface.bindings[b];
        if (binding.type == GPU_BINDING_TEXTURE && material.texture_count < MATERIAL_MAX_TEXTURES) {
            MaterialTextureSlot& slot = material.textures[material.texture_count++];
            slot = MaterialTextureSlot{};
            slot.binding = binding.binding;
            for (u32 i = 0; i < old_texture_count; ++i) {
                if (old_textures[i].binding == binding.binding) {
                    slot.texture = old_textures[i].texture;
                }
            }
        } else if (binding.type == GPU_BINDING_SAMPLER && material.sampler_count < MATERIAL_MAX_SAMPLERS) {
            MaterialSamplerSlot& slot = material.samplers[material.sampler_count++];
            slot = MaterialSamplerSlot{};
            slot.binding = binding.binding;
            for (u32 i = 0; i < old_sampler_count; ++i) {
                if (old_samplers[i].binding == binding.binding) {
                    slot.sampler = old_samplers[i].sampler;
                }
            }
        }
    }
}

// --- Create ----------------------------------------------------------------------

EntityId MATERIAL::create(Renderer& renderer, const EntityId shader_entity) {
    const Shader* shader = renderer.world != nullptr ? renderer.world->get<Shader>(shader_entity) : nullptr;
    if (shader == nullptr) {
        RENDERER::log(renderer, RENDER_LOG_ERROR, "create_material: entity %llu is not a shader", static_cast<unsigned long long>(shader_entity));
        return 0;
    }
    Material material;
    material.shader = shader_entity;
    material.shader_generation = shader->generation - 1;
    sync(material, *shader);

    char name[ENTITY_NAME_CAPACITY];
    snprintf(name, sizeof(name), "%s material", shader->name);
    const EntityId entity = SCENE::spawn(*renderer.world, name, 0);
    if (entity == 0) {
        MEMORY::heap_allocator()->free(material.params);
        return 0;
    }
    renderer.world->set(entity, material);
    return entity;
}

// --- Setters ---------------------------------------------------------------------

static bool resolve(Renderer& renderer, const EntityId entity, const char* what, const char* name, Material** material, const Shader** shader) {
    *material = renderer.world != nullptr ? renderer.world->get<Material>(entity) : nullptr;
    *shader = *material != nullptr ? renderer.world->get<Shader>((*material)->shader) : nullptr;
    if (*material == nullptr || *shader == nullptr) {
        RENDERER::log(renderer, RENDER_LOG_ERROR, "%s '%s': not a material entity", what, name != nullptr ? name : "");
        return false;
    }
    MATERIAL::sync(**material, **shader);
    return true;
}

static bool set_param(Renderer& renderer, const EntityId entity, const char* name, const void* data, const u32 size, const ReflectedScalar scalar, const u8 columns, const u8 rows) {
    Material* material = nullptr;
    const Shader* shader = nullptr;
    if (!resolve(renderer, entity, "set", name, &material, &shader)) {
        return false;
    }
    const ShaderProgram& program = shader->program;
    const ReflectedBinding* block = program.has_material_block() ? program.material_interface.find_binding(2, program.material_block_binding) : nullptr;
    const ReflectedMember* member = block != nullptr ? program.material_interface.find_member(*block, name) : nullptr;
    if (member == nullptr) {
        RENDERER::log(renderer, RENDER_LOG_ERROR, "material of %s has no block member '%s'", shader->name, name != nullptr ? name : "");
        return false;
    }
    if (member->scalar != scalar || member->columns != columns || member->rows != rows || member->size < size) {
        RENDERER::log(renderer, RENDER_LOG_ERROR, "material of %s: member '%s' is a %ux%u %s, not what set_* was given", shader->name, name, member->columns, member->rows,
            SHADER_REFLECT::scalar_name(member->scalar));
        return false;
    }
    if (material->params == nullptr || member->offset + size > material->param_size) {
        return false;
    }
    memcpy(material->params + member->offset, data, size);
    return true;
}

bool MATERIAL::set_float(Renderer& renderer, const EntityId material, const char* name, const f32 value) {
    return set_param(renderer, material, name, &value, sizeof(value), REFLECT_SCALAR_FLOAT, 1, 1);
}

bool MATERIAL::set_int(Renderer& renderer, const EntityId material, const char* name, const i32 value) {
    return set_param(renderer, material, name, &value, sizeof(value), REFLECT_SCALAR_INT, 1, 1);
}

bool MATERIAL::set_vec2(Renderer& renderer, const EntityId material, const char* name, const Vector2 value) {
    const f32 xy[2] = {value.x, value.y};
    return set_param(renderer, material, name, xy, sizeof(xy), REFLECT_SCALAR_FLOAT, 1, 2);
}

bool MATERIAL::set_vec3(Renderer& renderer, const EntityId material, const char* name, const Vector3 value) {
    f32 xyz[3];
    value.store(xyz);
    return set_param(renderer, material, name, xyz, sizeof(xyz), REFLECT_SCALAR_FLOAT, 1, 3);
}

bool MATERIAL::set_vec4(Renderer& renderer, const EntityId material, const char* name, const Vector4 value) {
    return set_param(renderer, material, name, &value, 16, REFLECT_SCALAR_FLOAT, 1, 4);
}

bool MATERIAL::set_mat4(Renderer& renderer, const EntityId material, const char* name, const Matrix4x4& value) {
    return set_param(renderer, material, name, &value, sizeof(value), REFLECT_SCALAR_FLOAT, 4, 4);
}

bool MATERIAL::set_texture(Renderer& renderer, const EntityId entity, const char* name, const AssetGuid& texture) {
    Material* material = nullptr;
    const Shader* shader = nullptr;
    if (!resolve(renderer, entity, "set_texture", name, &material, &shader)) {
        return false;
    }
    const ReflectedBinding* binding = shader->program.material_interface.find_binding(2, name);
    if (binding == nullptr || binding->type != GPU_BINDING_TEXTURE) {
        RENDERER::log(renderer, RENDER_LOG_ERROR, "material of %s has no texture '%s' in its material set", shader->name, name != nullptr ? name : "");
        return false;
    }
    for (u32 i = 0; i < material->texture_count; ++i) {
        if (material->textures[i].binding == binding->binding) {
            material->textures[i].texture = texture;
            return true;
        }
    }
    return false;
}

bool MATERIAL::set_sampler(Renderer& renderer, const EntityId entity, const char* name, const GpuSamplerDesc& sampler) {
    Material* material = nullptr;
    const Shader* shader = nullptr;
    if (!resolve(renderer, entity, "set_sampler", name, &material, &shader)) {
        return false;
    }
    const ReflectedBinding* binding = shader->program.material_interface.find_binding(2, name);
    if (binding == nullptr || binding->type != GPU_BINDING_SAMPLER) {
        RENDERER::log(renderer, RENDER_LOG_ERROR, "material of %s has no sampler '%s' in its material set", shader->name, name != nullptr ? name : "");
        return false;
    }
    for (u32 i = 0; i < material->sampler_count; ++i) {
        if (material->samplers[i].binding == binding->binding) {
            material->samplers[i].sampler = sampler;
            return true;
        }
    }
    return false;
}

// --- Files -------------------------------------------------------------------------

GpuSamplerDesc MATERIAL::sampler_desc(const MaterialSamplerDesc& sampler) {
    const auto filter = [](const MaterialFilter f) { return f == MATERIAL_FILTER_NEAREST ? GPU_FILTER_NEAREST : GPU_FILTER_LINEAR; };
    const auto address = [](const MaterialAddress a) {
        switch (a) {
        case MATERIAL_ADDRESS_CLAMP:
            return GPU_ADDRESS_CLAMP;
        case MATERIAL_ADDRESS_MIRROR:
            return GPU_ADDRESS_MIRROR;
        default:
            return GPU_ADDRESS_REPEAT;
        }
    };
    GpuSamplerDesc desc;
    desc.min_filter = filter(sampler.min_filter);
    desc.mag_filter = filter(sampler.mag_filter);
    desc.mip_filter = filter(sampler.mip_filter);
    desc.address_u = address(sampler.address_u);
    desc.address_v = address(sampler.address_v);
    desc.address_w = address(sampler.address_w);
    desc.max_anisotropy = sampler.max_anisotropy;
    return desc;
}

MaterialSamplerDesc MATERIAL::sampler_asset(const GpuSamplerDesc& sampler) {
    const auto filter = [](const GpuFilter f) { return f == GPU_FILTER_NEAREST ? MATERIAL_FILTER_NEAREST : MATERIAL_FILTER_LINEAR; };
    const auto address = [](const GpuAddressMode a) {
        switch (a) {
        case GPU_ADDRESS_CLAMP:
            return MATERIAL_ADDRESS_CLAMP;
        case GPU_ADDRESS_MIRROR:
            return MATERIAL_ADDRESS_MIRROR;
        default:
            return MATERIAL_ADDRESS_REPEAT;
        }
    };
    MaterialSamplerDesc desc;
    desc.min_filter = filter(sampler.min_filter);
    desc.mag_filter = filter(sampler.mag_filter);
    desc.mip_filter = filter(sampler.mip_filter);
    desc.address_u = address(sampler.address_u);
    desc.address_v = address(sampler.address_v);
    desc.address_w = address(sampler.address_w);
    desc.max_anisotropy = sampler.max_anisotropy;
    return desc;
}

// The file name without directories or extension: what the entity is named.
static void file_stem(const char* path, char* out, const usz capacity) {
    const char* start = path;
    for (const char* c = path; *c != '\0'; ++c) {
        if (*c == '/' || *c == '\\') {
            start = c + 1;
        }
    }
    const char* end = start + strlen(start);
    for (const char* c = end; c > start; --c) {
        if (c[-1] == '.') {
            end = c - 1;
            break;
        }
    }
    TEXT_ASSET::copy_span(start, static_cast<usz>(end - start), out, capacity);
}

// Writes one file param into the block by the member's reflected shape:
// the count must be the member's component count, and every number is
// converted to its scalar type. False (reported) when the member is absent
// or of another shape.
static bool apply_param(Renderer& renderer, Material& material, const Shader& shader, const MaterialParam& param, const char* label) {
    const ShaderProgram& program = shader.program;
    const ReflectedBinding* block = program.has_material_block() ? program.material_interface.find_binding(2, program.material_block_binding) : nullptr;
    const ReflectedMember* member = block != nullptr ? program.material_interface.find_member(*block, param.name) : nullptr;
    if (member == nullptr) {
        RENDERER::log(renderer, RENDER_LOG_WARNING, "material %s: shader %s has no block member '%s'", label, shader.name, param.name);
        return false;
    }
    const u32 components = static_cast<u32>(member->columns) * member->rows;
    if (param.count != components) {
        RENDERER::log(renderer, RENDER_LOG_WARNING, "material %s: '%s' is a %ux%u %s and takes %u numbers, the file gives %u", label, param.name, member->columns,
            member->rows, SHADER_REFLECT::scalar_name(member->scalar), components, param.count);
        return false;
    }
    if (material.params == nullptr || member->offset + member->size > material.param_size) {
        return false;
    }
    // Columns of a matrix sit at the block's column stride; vectors and
    // scalars are contiguous.
    const u32 column_stride = member->columns > 1 ? member->size / member->columns : 0;
    for (u32 i = 0; i < components; ++i) {
        const u32 column = member->columns > 1 ? i / member->rows : 0;
        const u32 row = member->columns > 1 ? i % member->rows : i;
        u8* dst = material.params + member->offset + column * column_stride + row * 4;
        const f64 value = param.values[i];
        switch (member->scalar) {
        case REFLECT_SCALAR_FLOAT: {
            const f32 f = static_cast<f32>(value);
            memcpy(dst, &f, sizeof(f));
            break;
        }
        case REFLECT_SCALAR_INT: {
            const i32 n = static_cast<i32>(value);
            memcpy(dst, &n, sizeof(n));
            break;
        }
        case REFLECT_SCALAR_UINT: {
            const u32 n = value > 0.0 ? static_cast<u32>(value) : 0u;
            memcpy(dst, &n, sizeof(n));
            break;
        }
        case REFLECT_SCALAR_BOOL: {
            const u32 n = value != 0.0 ? 1u : 0u;
            memcpy(dst, &n, sizeof(n));
            break;
        }
        }
    }
    return true;
}

// Applies every value, texture and sampler of `asset` to the entity. Names
// the shader does not declare are reported and skipped.
static void apply_asset(Renderer& renderer, const EntityId entity, const MaterialAsset& asset, const char* label) {
    for (u32 i = 0; i < asset.param_count; ++i) {
        Material* material = nullptr;
        const Shader* shader = nullptr;
        if (!resolve(renderer, entity, "load", asset.params[i].name, &material, &shader)) {
            return;
        }
        apply_param(renderer, *material, *shader, asset.params[i], label);
    }
    for (u32 i = 0; i < asset.texture_count; ++i) {
        MATERIAL::set_texture(renderer, entity, asset.textures[i].name, asset.textures[i].texture);
    }
    for (u32 i = 0; i < asset.sampler_count; ++i) {
        MATERIAL::set_sampler(renderer, entity, asset.samplers[i].name, MATERIAL::sampler_desc(asset.samplers[i].sampler));
    }
}

// Reads and parses the .material asset through the provider, then streams
// its text out again. `out_path` is the resource's path, owned by the
// provider.
static bool read_asset(Renderer& renderer, const AssetGuid& guid, MaterialAsset& out, const char** out_path) {
    char text_guid[TEXT_ASSET::GUID_TEXT_CAPACITY];
    TEXT_ASSET::format_guid(guid, text_guid);
    if (renderer.provider == nullptr) {
        RENDERER::log(renderer, RENDER_LOG_ERROR, "material %s: the renderer has no asset provider", text_guid);
        return false;
    }
    AssetResource* resource = renderer.provider->get(guid);
    if (resource == nullptr) {
        RENDERER::log(renderer, RENDER_LOG_ERROR, "material %s: not registered with the asset provider, or its file could not be read", text_guid);
        return false;
    }
    if (resource->view.header->type != ASSET_TYPE::MATERIAL || !resource->is_text()) {
        RENDERER::log(renderer, RENDER_LOG_ERROR, "%s: not a .material asset", resource->path);
        return false;
    }
    const ChunkEntry* entry = nullptr;
    const u8* text = resource->find_payload(CHUNK_TYPE::TEXT, &entry);
    u32 line = 0;
    const MaterialParseError error = MATERIAL_ASSET::parse(resource->view, text, entry != nullptr ? entry->size : 0, out, &line);
    renderer.provider->unload(resource);
    if (error != MATERIAL_PARSE_OK) {
        if (line != 0) {
            RENDERER::log(renderer, RENDER_LOG_ERROR, "%s:%u: %s", resource->path, line, MATERIAL_ASSET::parse_error_name(error));
        } else {
            RENDERER::log(renderer, RENDER_LOG_ERROR, "%s: %s", resource->path, MATERIAL_ASSET::parse_error_name(error));
        }
        return false;
    }
    *out_path = resource->path;
    return true;
}

EntityId MATERIAL::load(Renderer& renderer, const AssetGuid& asset) {
    if (const EntityId* existing = renderer.materials.find(asset)) {
        return *existing;
    }
    MaterialAsset parsed;
    const char* path = nullptr;
    if (!read_asset(renderer, asset, parsed, &path)) {
        return 0;
    }
    const EntityId shader = SHADER_LIBRARY::load(renderer, parsed.shader);
    if (shader == 0) {
        RENDERER::log(renderer, RENDER_LOG_ERROR, "%s: shader '%s' did not load", path, parsed.shader);
        return 0;
    }
    const EntityId entity = create(renderer, shader);
    if (entity == 0) {
        return 0;
    }
    char name[ENTITY_NAME_CAPACITY];
    file_stem(path, name, sizeof(name));
    SCENE::set_name(*renderer.world, entity, name);
    renderer.world->get<Material>(entity)->asset = asset;
    apply_asset(renderer, entity, parsed, name);
    renderer.materials.insert(asset, entity);
    return entity;
}

bool MATERIAL::reload(Renderer& renderer, const EntityId entity) {
    Material* material = renderer.world != nullptr ? renderer.world->get<Material>(entity) : nullptr;
    if (material == nullptr || material->asset.is_null()) {
        RENDERER::log(renderer, RENDER_LOG_ERROR, "reload: entity %llu is not a material loaded from a file", static_cast<unsigned long long>(entity));
        return false;
    }
    MaterialAsset parsed;
    const char* path = nullptr;
    if (!read_asset(renderer, material->asset, parsed, &path)) {
        return false;
    }
    const EntityId shader_entity = SHADER_LIBRARY::load(renderer, parsed.shader);
    if (shader_entity == 0) {
        RENDERER::log(renderer, RENDER_LOG_ERROR, "%s: shader '%s' did not load", path, parsed.shader);
        return false;
    }
    // Loading may have spawned a Shader entity: take the pointers again.
    material = renderer.world->get<Material>(entity);
    const Shader* shader = renderer.world->get<Shader>(shader_entity);
    if (material == nullptr || shader == nullptr) {
        return false;
    }
    // Start over: a zeroed block for the file's shader and no slot kept.
    material->shader = shader_entity;
    material->shader_generation = shader->generation - 1;
    material->texture_count = 0;
    material->sampler_count = 0;
    sync(*material, *shader);

    char name[ENTITY_NAME_CAPACITY];
    file_stem(path, name, sizeof(name));
    apply_asset(renderer, entity, parsed, name);
    renderer.world->modified<Material>(entity);
    return true;
}

void MATERIAL::reload_all(Renderer& renderer) {
    DynamicArray<EntityId> entities;
    for (auto& entry : renderer.materials) {
        entities.push(entry.value);
    }
    for (const EntityId entity : entities) {
        reload(renderer, entity);
    }
    entities.free();
}

bool MATERIAL::to_asset(Renderer& renderer, const EntityId entity, MaterialAsset& out) {
    Material* material = nullptr;
    const Shader* shader = nullptr;
    if (!resolve(renderer, entity, "to_asset", nullptr, &material, &shader)) {
        return false;
    }
    out.clear();
    out.guid = material->asset;
    strncpy(out.shader, shader->name, sizeof(out.shader) - 1);

    const ShaderProgram& program = shader->program;
    const ShaderReflection& interface = program.material_interface;
    const ReflectedBinding* block = program.has_material_block() ? interface.find_binding(2, program.material_block_binding) : nullptr;
    if (block != nullptr && material->params != nullptr) {
        for (u32 m = 0; m < block->member_count; ++m) {
            const ReflectedMember& member = interface.members[block->first_member + m];
            const u32 components = static_cast<u32>(member.columns) * member.rows;
            if (components == 0 || components > MATERIAL_ASSET::MAX_VALUES || member.offset + member.size > material->param_size) {
                continue;
            }
            const u32 column_stride = member.columns > 1 ? member.size / member.columns : 0;
            f64 values[MATERIAL_ASSET::MAX_VALUES];
            for (u32 i = 0; i < components; ++i) {
                const u32 column = member.columns > 1 ? i / member.rows : 0;
                const u32 row = member.columns > 1 ? i % member.rows : i;
                const u8* src = material->params + member.offset + column * column_stride + row * 4;
                switch (member.scalar) {
                case REFLECT_SCALAR_FLOAT: {
                    f32 f;
                    memcpy(&f, src, sizeof(f));
                    values[i] = f;
                    break;
                }
                case REFLECT_SCALAR_INT: {
                    i32 n;
                    memcpy(&n, src, sizeof(n));
                    values[i] = n;
                    break;
                }
                default: {
                    u32 n;
                    memcpy(&n, src, sizeof(n));
                    values[i] = n;
                    break;
                }
                }
            }
            out.set_param(member.name, values, components);
        }
    }
    for (u32 i = 0; i < material->texture_count; ++i) {
        if (const ReflectedBinding* binding = interface.find_binding(2, material->textures[i].binding)) {
            out.set_texture(binding->name, material->textures[i].texture);
        }
    }
    for (u32 i = 0; i < material->sampler_count; ++i) {
        if (const ReflectedBinding* binding = interface.find_binding(2, material->samplers[i].binding)) {
            out.set_sampler(binding->name, sampler_asset(material->samplers[i].sampler));
        }
    }
    return true;
}

bool MATERIAL::save(Renderer& renderer, const EntityId entity, const char* path) {
    MaterialAsset asset;
    if (!to_asset(renderer, entity, asset)) {
        return false;
    }
    if (asset.guid.is_null()) {
        RENDERER::log(renderer, RENDER_LOG_ERROR, "save %s: the material has no asset guid; assign Material::asset first", path != nullptr ? path : "");
        return false;
    }
    if (!MATERIAL_ASSET::write_file(asset, path)) {
        RENDERER::log(renderer, RENDER_LOG_ERROR, "save %s: could not write the file", path != nullptr ? path : "");
        return false;
    }
    return true;
}
