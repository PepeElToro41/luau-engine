#include "engine/render/renderer.hpp"

#include "engine/ecs/world.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/platform/file.hpp"
#include "engine/utils/hash.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

static constexpr u64 FULLSCREEN_TAG = HASH::fnv1a_str("fullscreen");

void Renderer::log(const RenderLogLevel level, const char* format, ...) const {
    char text[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (this->log_sink.fn != nullptr) {
        this->log_sink.fn(level, text, this->log_sink.user_data);
        return;
    }
    fprintf(stderr, "[renderer] %s%s\n", level == RENDER_LOG_ERROR ? "error: " : level == RENDER_LOG_WARNING ? "warning: " : "", text);
}

usz Renderer::PipelineKeyHash::operator()(const PipelineKey& key) const {
    return static_cast<usz>(HASH::fnv1a(&key, sizeof(key)));
}

static u32 align_up(const u32 value, const u32 alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

// --- Lifetime ------------------------------------------------------------------

bool Renderer::init(GpuDevice* gpu, GpuResourceManager* resources, World* world, AssetResourceProvider* provider, const char* engine_render_dir) {
    this->gpu = gpu;
    this->resources = resources;
    this->world = world;
    this->provider = provider;
    if (engine_render_dir != nullptr) {
        strncpy(this->engine_render_dir, engine_render_dir, RENDERER_PATH_MAX - 1);
    }
    this->graph.init();
    this->plan.init();
    this->next_plan.init();
    // The backend takes its input sampler from the cache, so the cache comes first.
    if (!this->layouts.init(gpu) || !this->samplers.init(gpu) || !this->backend.init(gpu, resources, &this->samplers) || !this->assets.init(resources, provider)) {
        return false;
    }

    // Material sets live as long as their material; frame sets are reset
    // every time their slot comes around.
    const VkDescriptorPoolSize material_sizes[2] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024},
    };
    if (!this->material_sets.init(gpu, material_sizes, 2, 256, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)) {
        return false;
    }
    const VkDescriptorPoolSize frame_sizes[2] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256},
    };
    for (DescriptorAllocator& allocator : this->frame_sets) {
        if (!allocator.init(gpu, frame_sizes, 2, 64, 0)) {
            return false;
        }
    }

    // Set 0: the frame block, what engine/frame.slang declares.
    DescriptorLayoutDesc frame_desc;
    frame_desc.add(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    this->frame_layout = this->layouts.get(frame_desc);
    if (this->frame_layout == VK_NULL_HANDLE) {
        return false;
    }
    ReflectedBinding& frame_binding = this->frame_interface.bindings[0];
    frame_binding.set = 0;
    frame_binding.binding = 0;
    frame_binding.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    frame_binding.count = 1;
    frame_binding.stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    strcpy(frame_binding.name, "frame");
    frame_binding.block_size = sizeof(FrameUniforms);
    this->frame_interface.binding_count = 1;

    const u32 alignment = static_cast<u32>(gpu->properties.limits.minUniformBufferOffsetAlignment);
    this->frame_stride = align_up(sizeof(FrameUniforms), alignment > 16 ? alignment : 16);
    GpuBufferDesc uniforms_desc;
    uniforms_desc.size = static_cast<VkDeviceSize>(this->frame_stride) * FRAMES_IN_FLIGHT;
    uniforms_desc.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    uniforms_desc.memory = GPU_MEMORY_HOST_VISIBLE;
    if (!resources->create_buffer(uniforms_desc, this->frame_uniforms)) {
        return false;
    }

    // What unset texture slots sample.
    const u8 white_pixel[4] = {255, 255, 255, 255};
    GpuTextureDesc white_desc;
    white_desc.format = VK_FORMAT_R8G8B8A8_UNORM;
    white_desc.width = 1;
    white_desc.height = 1;
    if (!resources->create_texture(white_desc, white_pixel, sizeof(white_pixel), this->white)) {
        return false;
    }

    this->build_default_graph();
    return true;
}

void Renderer::shutdown() {
    if (this->gpu == nullptr) {
        return;
    }
    for (auto& entry : this->pipelines) {
        entry.value.shutdown();
    }
    this->pipelines.free();
    this->backend.shutdown();
    this->assets.shutdown();
    if (this->white.is_valid()) {
        this->resources->destroy_texture(this->white);
    }
    if (this->frame_uniforms.is_valid()) {
        this->resources->destroy_buffer(this->frame_uniforms);
    }
    for (DescriptorAllocator& allocator : this->frame_sets) {
        allocator.shutdown();
    }
    this->material_sets.shutdown();
    for (DynamicArray<DescriptorSetHandle>& retired : this->retired_sets) {
        retired.free();
    }
    this->samplers.shutdown();
    // Layouts last: pipelines and sets were built on them.
    this->layouts.shutdown();
    this->plan.free();
    this->next_plan.free();
    this->graph.free();
    this->diagnostics.free();
    this->custom_passes.free();
    this->plan_valid = false;
    this->gpu = nullptr;
    this->resources = nullptr;
    this->world = nullptr;
    this->provider = nullptr;
}

void Renderer::set_project_render_dir(const char* path) {
    this->project_render_dir[0] = '\0';
    if (path != nullptr) {
        strncpy(this->project_render_dir, path, RENDERER_PATH_MAX - 1);
    }
}

void Renderer::build_default_graph() {
    this->graph.init(this->graph.allocator);
    this->default_forward = this->graph.add_pass("forward", RENDER_PASS_DRAW_SCENE);
    this->graph.set_draw_scene_tag(this->default_forward, "forward");
    this->graph.set_color_attachment(this->default_forward, 0, this->graph.backbuffer, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
    this->graph.set_clear_color(this->default_forward, 0, this->clear_color);
    this->graph.set_depth_attachment(this->default_forward, this->graph.backbuffer_depth, RENDER_LOAD_CLEAR, RENDER_STORE_DONT_CARE);
}

// --- Shaders -------------------------------------------------------------------

bool Renderer::resolve_shader_path(const char* name, char* out, const usz out_size) const {
    const char* roots[2] = {this->project_render_dir, this->engine_render_dir};
    const char* extensions[2] = {"slang", "glsl"};
    for (const char* root : roots) {
        if (root[0] == '\0') {
            continue;
        }
        for (const char* extension : extensions) {
            snprintf(out, out_size, "%s/%s.%s", root, name, extension);
            File file;
            if (PLATFORM::file_open(&file, out, FILE_ACCESS_READ)) {
                PLATFORM::file_close(&file);
                return true;
            }
        }
    }
    return false;
}

bool Renderer::load_program(const char* path, ShaderProgram& out) {
    const char* include_dirs[2];
    usz include_dir_count = 0;
    if (this->project_render_dir[0] != '\0') {
        include_dirs[include_dir_count++] = this->project_render_dir;
    }
    if (this->engine_render_dir[0] != '\0') {
        include_dirs[include_dir_count++] = this->engine_render_dir;
    }
    ShaderProgramLoadDesc desc;
    desc.path = path;
    desc.include_dirs = include_dirs;
    desc.include_dir_count = include_dir_count;
    desc.layouts = &this->layouts;
    desc.frame_layout = this->frame_layout;
    desc.frame_interface = &this->frame_interface;
    const bool ok = SHADER_PROGRAM::load(desc, MEMORY::heap_allocator(), out);
    if (out.log != nullptr && out.log[0] != '\0') {
        this->log(ok ? RENDER_LOG_WARNING : RENDER_LOG_ERROR, "shader %s:\n%s", path, out.log);
    }
    return ok;
}

EntityId Renderer::load_shader(const char* name) {
    if (name == nullptr || name[0] == '\0' || this->world == nullptr) {
        return 0;
    }
    // Already loaded under this name?
    EntityId existing = 0;
    this->world->query<Shader>().each([&](const EntityId entity, Shader& shader) {
        if (existing == 0 && strcmp(shader.name, name) == 0) {
            existing = entity;
        }
    });
    if (existing != 0) {
        return existing;
    }

    char path[RENDERER_PATH_MAX];
    if (!this->resolve_shader_path(name, path, sizeof(path))) {
        this->log(RENDER_LOG_ERROR, "no %s.slang (or .glsl) in the project or engine render directories", name);
        return 0;
    }
    Shader shader;
    strncpy(shader.name, name, sizeof(shader.name) - 1);
    if (!this->load_program(path, shader.program)) {
        shader.program.free();
        return 0;
    }
    const EntityId entity = this->world->new_entity();
    this->world->set(entity, shader);
    return entity;
}

bool Renderer::reload_shader(const EntityId entity) {
    Shader* shader = this->world != nullptr ? this->world->get<Shader>(entity) : nullptr;
    if (shader == nullptr) {
        return false;
    }
    char path[RENDERER_PATH_MAX];
    if (!this->resolve_shader_path(shader->name, path, sizeof(path))) {
        this->log(RENDER_LOG_ERROR, "shader %s: file disappeared", shader->name);
        return false;
    }
    ShaderProgram fresh;
    if (!this->load_program(path, fresh)) {
        fresh.free();
        return false;
    }
    this->release_shader_pipelines(entity);
    shader->program.free();
    shader->program = fresh;
    shader->generation += 1;
    this->world->modified<Shader>(entity);
    return true;
}

void Renderer::reload_all_shaders() {
    if (this->world == nullptr) {
        return;
    }
    DynamicArray<EntityId> entities;
    this->world->query<Shader>().each([&](const EntityId entity, Shader&) { entities.push(entity); });
    for (const EntityId entity : entities) {
        this->reload_shader(entity);
    }
    entities.free();
}

void Renderer::release_shader_pipelines(const EntityId shader) {
    DynamicArray<PipelineKey> stale;
    for (auto& entry : this->pipelines) {
        if (entry.key.shader == shader) {
            if (entry.value.is_valid()) {
                entry.value.release(*this->resources);
            }
            stale.push(entry.key);
        }
    }
    for (const PipelineKey& key : stale) {
        this->pipelines.remove(key);
    }
    stale.free();
}

void Renderer::on_shader_removed(const EntityId entity, Shader& shader) {
    this->release_shader_pipelines(entity);
    shader.program.free();
}

// --- Materials -----------------------------------------------------------------

void Renderer::retire_material_sets(Material& material) {
    for (DescriptorSetHandle& handle : material.sets) {
        if (handle.is_valid()) {
            this->retired_sets[this->current_slot].push(handle);
            handle = DescriptorSetHandle{};
        }
    }
}

bool Renderer::rebuild_material(Material& material, const Shader& shader) {
    const ShaderProgram& program = shader.program;
    this->retire_material_sets(material);
    if (material.uniform.is_valid()) {
        this->resources->release_buffer(material.uniform);
    }
    MEMORY::heap_allocator()->free(material.params);
    material.params = nullptr;
    material.param_size = 0;
    material.texture_count = 0;
    material.shader_generation = shader.generation;

    if (program.has_material_block()) {
        material.param_size = program.material_block_size;
        material.params = MEMORY::heap_allocator()->allocate_array<u8>(material.param_size);
        memset(material.params, 0, material.param_size);
        const u32 alignment = static_cast<u32>(this->gpu->properties.limits.minUniformBufferOffsetAlignment);
        material.slot_stride = align_up(material.param_size, alignment > 16 ? alignment : 16);
        GpuBufferDesc desc;
        desc.size = static_cast<VkDeviceSize>(material.slot_stride) * FRAMES_IN_FLIGHT;
        desc.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        desc.memory = GPU_MEMORY_HOST_VISIBLE;
        if (!this->resources->create_buffer(desc, material.uniform)) {
            return false;
        }
    }
    const ShaderReflection& interface = program.material_interface;
    for (u32 b = 0; b < interface.binding_count; ++b) {
        const ReflectedBinding& binding = interface.bindings[b];
        if (binding.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && material.texture_count < MATERIAL_MAX_TEXTURES) {
            MaterialTextureSlot& slot = material.textures[material.texture_count++];
            slot = MaterialTextureSlot{};
            slot.binding = binding.binding;
        }
    }
    if (program.material_layout != VK_NULL_HANDLE) {
        for (u32 s = 0; s < FRAMES_IN_FLIGHT; ++s) {
            material.sets[s] = this->material_sets.allocate(program.material_layout);
            if (!material.sets[s].is_valid()) {
                return false;
            }
        }
    }
    material.dirty_mask = (1u << FRAMES_IN_FLIGHT) - 1;
    return true;
}

EntityId Renderer::create_material(const EntityId shader_entity) {
    const Shader* shader = this->world != nullptr ? this->world->get<Shader>(shader_entity) : nullptr;
    if (shader == nullptr || !shader->program.is_valid()) {
        this->log(RENDER_LOG_ERROR, "create_material: not a loaded shader entity");
        return 0;
    }
    Material material;
    material.shader = shader_entity;
    if (!this->rebuild_material(material, *shader)) {
        this->log(RENDER_LOG_ERROR, "create_material: could not allocate the material's GPU objects");
        return 0;
    }
    const EntityId entity = this->world->new_entity();
    this->world->set(entity, material);
    return entity;
}

void Renderer::on_material_removed(const EntityId, Material& material) {
    this->retire_material_sets(material);
    if (material.uniform.is_valid()) {
        this->resources->release_buffer(material.uniform);
    }
    MEMORY::heap_allocator()->free(material.params);
    material.params = nullptr;
    material.param_size = 0;
}

bool Renderer::set_param(const EntityId entity, const char* name, const void* data, const u32 size, const ReflectedScalar scalar, const u8 columns, const u8 rows) {
    Material* material = this->world != nullptr ? this->world->get<Material>(entity) : nullptr;
    const Shader* shader = material != nullptr ? this->world->get<Shader>(material->shader) : nullptr;
    if (material == nullptr || shader == nullptr) {
        this->log(RENDER_LOG_ERROR, "set '%s': not a material entity", name != nullptr ? name : "");
        return false;
    }
    const ShaderProgram& program = shader->program;
    const ReflectedBinding* block = program.has_material_block() ? program.material_interface.find_binding(2, program.material_block_binding) : nullptr;
    const ReflectedMember* member = block != nullptr ? program.material_interface.find_member(*block, name) : nullptr;
    if (member == nullptr) {
        this->log(RENDER_LOG_ERROR, "material of %s has no block member '%s'", shader->name, name != nullptr ? name : "");
        return false;
    }
    if (member->scalar != scalar || member->columns != columns || member->rows != rows || member->size < size) {
        this->log(RENDER_LOG_ERROR, "material of %s: member '%s' is a %ux%u %s, not what set_* was given", shader->name, name, member->columns,
            member->rows, SHADER_REFLECT::scalar_name(member->scalar));
        return false;
    }
    if (material->params == nullptr || member->offset + size > material->param_size) {
        return false;
    }
    memcpy(material->params + member->offset, data, size);
    material->dirty_mask = (1u << FRAMES_IN_FLIGHT) - 1;
    return true;
}

bool Renderer::set_float(const EntityId material, const char* name, const f32 value) {
    return this->set_param(material, name, &value, sizeof(value), REFLECT_SCALAR_FLOAT, 1, 1);
}

bool Renderer::set_int(const EntityId material, const char* name, const i32 value) {
    return this->set_param(material, name, &value, sizeof(value), REFLECT_SCALAR_INT, 1, 1);
}

bool Renderer::set_vec2(const EntityId material, const char* name, const Vector2 value) {
    const f32 xy[2] = {value.x, value.y};
    return this->set_param(material, name, xy, sizeof(xy), REFLECT_SCALAR_FLOAT, 1, 2);
}

bool Renderer::set_vec3(const EntityId material, const char* name, const Vector3 value) {
    f32 xyz[3];
    value.store(xyz);
    return this->set_param(material, name, xyz, sizeof(xyz), REFLECT_SCALAR_FLOAT, 1, 3);
}

bool Renderer::set_vec4(const EntityId material, const char* name, const Vector4 value) {
    return this->set_param(material, name, &value, 16, REFLECT_SCALAR_FLOAT, 1, 4);
}

bool Renderer::set_mat4(const EntityId material, const char* name, const Matrix4x4& value) {
    return this->set_param(material, name, &value, sizeof(value), REFLECT_SCALAR_FLOAT, 4, 4);
}

bool Renderer::set_texture(const EntityId entity, const char* name, const AssetGuid& texture, const SamplerDesc& sampler) {
    Material* material = this->world != nullptr ? this->world->get<Material>(entity) : nullptr;
    const Shader* shader = material != nullptr ? this->world->get<Shader>(material->shader) : nullptr;
    if (material == nullptr || shader == nullptr) {
        this->log(RENDER_LOG_ERROR, "set_texture '%s': not a material entity", name != nullptr ? name : "");
        return false;
    }
    const ReflectedBinding* binding = shader->program.material_interface.find_binding(2, name);
    if (binding == nullptr || binding->type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
        this->log(RENDER_LOG_ERROR, "material of %s has no sampler '%s' in its material set", shader->name, name != nullptr ? name : "");
        return false;
    }
    for (u32 i = 0; i < material->texture_count; ++i) {
        if (material->textures[i].binding == binding->binding) {
            material->textures[i].texture = texture;
            material->textures[i].sampler = sampler;
            material->dirty_mask = (1u << FRAMES_IN_FLIGHT) - 1;
            return true;
        }
    }
    return false;
}

void Renderer::flush_material(Material& material, const u32 slot) {
    const Shader* shader = this->world->get<Shader>(material.shader);
    if (shader == nullptr || !shader->program.is_valid()) {
        return;
    }
    if (material.shader_generation != shader->generation) {
        // The shader was reloaded: its interface may have changed.
        if (!this->rebuild_material(material, *shader)) {
            return;
        }
    }
    const ShaderProgram& program = shader->program;
    if (!material.sets[slot].is_valid()) {
        material.dirty_mask &= ~(1u << slot);
        return;
    }
    DescriptorWriter writer;
    if (material.params != nullptr) {
        memcpy(static_cast<u8*>(material.uniform.mapped) + static_cast<usz>(slot) * material.slot_stride, material.params, material.param_size);
        writer.write_buffer(material.sets[slot].set, program.material_block_binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, material.uniform.buffer,
            static_cast<VkDeviceSize>(slot) * material.slot_stride, material.param_size);
    }
    for (u32 i = 0; i < material.texture_count; ++i) {
        const MaterialTextureSlot& texture_slot = material.textures[i];
        const GpuTexture* texture = this->assets.get_texture(texture_slot.texture);
        if (texture == nullptr) {
            texture = &this->white;
        }
        VkSampler sampler = this->samplers.get(texture_slot.sampler);
        writer.write_image(material.sets[slot].set, texture_slot.binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, texture->view, sampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    writer.update(this->gpu);
    material.dirty_mask &= ~(1u << slot);
}

// --- Graph ---------------------------------------------------------------------

bool Renderer::register_custom_pass(const char* name, const CustomPassFn fn, void* user_data) {
    if (name == nullptr || name[0] == '\0' || fn == nullptr) {
        return false;
    }
    CustomPass entry;
    entry.fn = fn;
    entry.user_data = user_data;
    this->custom_passes.insert(HASH::fnv1a_str(name), entry);
    return true;
}

bool Renderer::unregister_custom_pass(const char* name) {
    if (name == nullptr) {
        return false;
    }
    return this->custom_passes.remove(HASH::fnv1a_str(name));
}

void Renderer::report_diagnostics() {
    if (this->reported_version == this->graph.version) {
        return;
    }
    this->reported_version = this->graph.version;
    for (const RenderDiagnostic& diagnostic : this->diagnostics) {
        const RenderPassDesc* pass = this->graph.pass(diagnostic.pass);
        const RenderResourceDesc* resource = this->graph.resource(diagnostic.resource);
        this->log(diagnostic.severity == RENDER_DIAG_ERROR ? RENDER_LOG_ERROR : RENDER_LOG_WARNING, "render graph: %s%s%s%s%s%s%s",
            pass != nullptr ? "pass '" : "", pass != nullptr ? pass->name : "", pass != nullptr ? "': " : "", diagnostic.message,
            resource != nullptr ? " (resource '" : "", resource != nullptr ? resource->name : "", resource != nullptr ? "')" : "");
    }
}

void Renderer::prune_pipelines() {
    DynamicArray<PipelineKey> stale;
    for (auto& entry : this->pipelines) {
        bool alive = false;
        for (const u64 key : this->plan.compat_keys) {
            alive |= key == entry.key.compat_key;
        }
        if (!alive) {
            if (entry.value.is_valid()) {
                entry.value.release(*this->resources);
            }
            stale.push(entry.key);
        }
    }
    for (const PipelineKey& key : stale) {
        this->pipelines.remove(key);
    }
    stale.free();
}

bool Renderer::recompile(const RenderBackbufferInfo& info) {
    this->diagnostics.clear();
    const bool compiled = RENDER_GRAPH::compile(this->graph, info, this->next_plan, &this->diagnostics);
    this->report_diagnostics();
    if (!compiled) {
        return false;
    }
    if (!this->backend.realize(this->next_plan)) {
        this->log(RENDER_LOG_ERROR, "render graph: could not realize the plan; keeping the previous one");
        return false;
    }
    std::swap(this->plan, this->next_plan);
    this->plan_valid = true;
    this->last_backbuffer = info;
    this->prune_pipelines();
    return true;
}

void Renderer::render_fallback(const FrameContext& frame) {
    VkClearValue clear[RENDER_TARGET_ATTACHMENT_COUNT];
    render_target_clear_values(this->clear_color, clear);

    VkRenderPassBeginInfo pass_info{};
    pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass_info.renderPass = frame.target.render_pass;
    pass_info.framebuffer = frame.target.framebuffer;
    pass_info.renderArea.extent = frame.target.extent;
    pass_info.clearValueCount = RENDER_TARGET_ATTACHMENT_COUNT;
    pass_info.pClearValues = clear;
    vkCmdBeginRenderPass(frame.cmd, &pass_info, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(frame.cmd);
}

// --- Frame ---------------------------------------------------------------------

void Renderer::write_frame_uniforms(const u32 slot, const VkExtent2D extent) {
    FrameUniforms uniforms;
    Vector3 eye(0.0f, 1.0f, 3.0f);
    uniforms.view = Matrix4x4::look_at(eye, Vector3::zero(), Vector3::up());
    f32 fov_y = MATH::radians(60.0f);
    f32 near_plane = 0.1f;
    f32 far_plane = 1000.0f;
    if (this->world != nullptr) {
        const EntityId camera_entity = this->world->query<Transform, Camera>().first();
        if (camera_entity != 0) {
            const Transform* transform = this->world->get<Transform>(camera_entity);
            const Camera* camera = this->world->get<Camera>(camera_entity);
            eye = transform->position;
            uniforms.view = transform->matrix().inverse_affine();
            fov_y = camera->fov_y;
            near_plane = camera->near_plane;
            far_plane = camera->far_plane;
        }
    }
    const f32 aspect = extent.height > 0 ? static_cast<f32>(extent.width) / static_cast<f32>(extent.height) : 1.0f;
    uniforms.projection = Matrix4x4::perspective(fov_y, aspect, near_plane, far_plane);
    uniforms.view_projection = uniforms.projection * uniforms.view;
    uniforms.camera_position = Vector4(eye, 1.0f);
    uniforms.time = Vector4(this->time, this->delta_time, 0.0f, 0.0f);
    memcpy(static_cast<u8*>(this->frame_uniforms.mapped) + static_cast<usz>(slot) * this->frame_stride, &uniforms, sizeof(uniforms));

    const DescriptorSetHandle handle = this->frame_sets[slot].allocate(this->frame_layout);
    this->frame_set = handle.set;
    if (handle.is_valid()) {
        DescriptorWriter writer;
        writer.write_buffer(handle.set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, this->frame_uniforms.buffer, static_cast<VkDeviceSize>(slot) * this->frame_stride,
            sizeof(FrameUniforms));
        writer.update(this->gpu);
    }
}

void Renderer::begin_frame(const u32 slot, const VkExtent2D extent) {
    this->current_slot = slot;
    // Everything this slot used last time has completed.
    this->frame_sets[slot].reset();
    for (const DescriptorSetHandle& handle : this->retired_sets[slot]) {
        this->material_sets.free(handle);
    }
    this->retired_sets[slot].clear();

    if (this->world != nullptr) {
        this->world->query<Material>().each([&](const EntityId, Material& material) {
            if (material.dirty_mask & (1u << slot)) {
                this->flush_material(material, slot);
            }
        });
    }
    this->write_frame_uniforms(slot, extent);
}

void Renderer::render(const FrameContext& frame) {
    this->begin_frame(frame.slot, frame.target.extent);

    RenderBackbufferInfo info;
    info.color_format = static_cast<RenderFormat>(frame.target.format);
    info.depth_format = static_cast<RenderFormat>(frame.target.depth_format);
    info.width = frame.target.extent.width;
    info.height = frame.target.extent.height;

    if (this->graph.dirty || !this->plan_valid || info != this->last_backbuffer) {
        if (!this->recompile(info) && info != this->last_backbuffer) {
            // A plan for another target cannot run: its images and
            // framebuffers were sized for the old one.
            this->plan_valid = false;
        }
        this->graph.dirty = false;
    }

    if (!this->plan_valid) {
        this->render_fallback(frame);
        return;
    }
    this->backend.execute(frame, this->plan, this->graph, *this);
}

// --- Pipelines -----------------------------------------------------------------

GraphicsPipeline* Renderer::pipeline_for(const EntityId shader_entity, const u64 tag, const u64 compat_key, const GpuMeshLayout* mesh) {
    PipelineKey key;
    key.shader = shader_entity;
    key.tag = tag;
    key.compat_key = compat_key;
    key.vertex_layout = mesh != nullptr ? mesh->hash : 0;
    if (GraphicsPipeline* found = this->pipelines.find(key)) {
        return found->is_valid() ? found : nullptr;
    }

    const Shader* shader = this->world != nullptr ? this->world->get<Shader>(shader_entity) : nullptr;
    const ShaderPassProgram* pass = shader != nullptr ? shader->program.find_pass(tag) : nullptr;
    GraphicsPipeline pipeline;
    if (pass == nullptr) {
        // Not an error: the shader has no variant for this pass.
        this->pipelines.insert(key, pipeline);
        return nullptr;
    }
    const VkRenderPass render_pass = this->backend.render_pass_for_key(compat_key);
    if (render_pass == VK_NULL_HANDLE) {
        // Not yet known (the backbuffer pass before its first frame); try
        // again next frame.
        return nullptr;
    }

    GraphicsPipelineDesc desc;
    desc.vertex = &pass->vertex;
    desc.fragment = pass->fragment.is_valid() ? &pass->fragment : nullptr;
    desc.color_attachment_count = this->backend.color_count_for_key(compat_key);
    if (mesh == nullptr) {
        // A fullscreen pass covers the target whatever its winding and
        // ignores whatever depth the target holds.
        desc.cull_mode = VK_CULL_MODE_NONE;
        desc.depth_test = false;
        desc.depth_write = false;
    }
    char error[256];
    if (mesh != nullptr && !VERTEX_INPUT::build(pass->reflection, *mesh, desc, error, sizeof(error))) {
        this->log(RENDER_LOG_ERROR, "shader %s, pass %s: %s", shader->name, pass->tag_name, error);
        this->pipelines.insert(key, pipeline);
        return nullptr;
    }
    for (u32 s = 0; s < pass->set_layout_count; ++s) {
        desc.add_set_layout(pass->set_layouts[s]);
    }
    if (pass->has_push_constants) {
        desc.add_push_constants(pass->push_constants.stageFlags, pass->push_constants.size, pass->push_constants.offset);
    }
    if (!pipeline.init(this->gpu, render_pass, desc)) {
        this->log(RENDER_LOG_ERROR, "shader %s, pass %s: pipeline creation failed", shader->name, pass->tag_name);
        this->pipelines.insert(key, GraphicsPipeline{});
        return nullptr;
    }
    GraphicsPipeline& stored = this->pipelines.insert(key, pipeline);
    return &stored;
}

VkDescriptorSet Renderer::pass_input_set(const RenderPassContext& ctx, const ShaderPassProgram& pass) {
    if (pass.set_layout_count < 2 || (pass.reflection.set_mask() & 2u) == 0) {
        return VK_NULL_HANDLE;
    }
    const DescriptorSetHandle handle = this->frame_sets[ctx.slot].allocate(pass.set_layouts[1]);
    if (!handle.is_valid()) {
        return VK_NULL_HANDLE;
    }
    DescriptorWriter writer;
    for (u32 b = 0; b < pass.reflection.binding_count; ++b) {
        const ReflectedBinding& binding = pass.reflection.bindings[b];
        if (binding.set != 1 || binding.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            continue;
        }
        const VkImageView view = binding.binding < ctx.input_count && ctx.input_views[binding.binding] != VK_NULL_HANDLE ? ctx.input_views[binding.binding] : this->white.view;
        writer.write_image(handle.set, binding.binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, view, ctx.input_sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    writer.update(this->gpu);
    return handle.set;
}

// --- Executor ------------------------------------------------------------------

void Renderer::draw_scene(const RenderPassContext& ctx) {
    if (this->world == nullptr || ctx.desc == nullptr) {
        return;
    }
    const u64 tag = ctx.desc->tag_hash;
    GraphicsPipeline* bound = nullptr;
    EntityId bound_shader = 0;

    this->world->query<Transform, MeshRenderer>().each([&](const EntityId, Transform& transform, MeshRenderer& renderer) {
        const Material* material = this->world->get<Material>(renderer.material);
        if (material == nullptr) {
            return;
        }
        const Shader* shader = this->world->get<Shader>(material->shader);
        if (shader == nullptr || !shader->program.is_valid()) {
            return;
        }
        const GpuMesh* mesh = this->assets.get_mesh(renderer.mesh);
        if (mesh == nullptr) {
            return;
        }
        GraphicsPipeline* pipeline = this->pipeline_for(material->shader, tag, ctx.compat_key, &mesh->layout);
        if (pipeline == nullptr) {
            return;
        }
        const ShaderPassProgram* pass = shader->program.find_pass(tag);

        if (pipeline != bound) {
            pipeline->bind(ctx.cmd);
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout, 0, 1, &this->frame_set, 0, nullptr);
            if (material->shader != bound_shader || bound == nullptr) {
                const VkDescriptorSet inputs = this->pass_input_set(ctx, *pass);
                if (inputs != VK_NULL_HANDLE) {
                    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout, 1, 1, &inputs, 0, nullptr);
                }
            }
            bound = pipeline;
            bound_shader = material->shader;
        }
        if (pass->uses_material() && material->sets[ctx.slot].is_valid()) {
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout, 2, 1, &material->sets[ctx.slot].set, 0, nullptr);
        }
        if (pass->has_push_constants) {
            const Matrix4x4 model = transform.matrix();
            const u32 size = pass->push_constants.size < sizeof(model) ? pass->push_constants.size : static_cast<u32>(sizeof(model));
            pipeline->push_constants(ctx.cmd, pass->push_constants.stageFlags, &model, size, pass->push_constants.offset);
        }

        VkBuffer buffers[MESH_ASSET::MAX_STREAMS];
        VkDeviceSize offsets[MESH_ASSET::MAX_STREAMS] = {};
        for (u32 s = 0; s < mesh->stream_count; ++s) {
            buffers[s] = mesh->streams[s].buffer;
        }
        vkCmdBindVertexBuffers(ctx.cmd, 0, mesh->stream_count, buffers, offsets);
        vkCmdBindIndexBuffer(ctx.cmd, mesh->indices.buffer, 0, mesh->index_type);
        for (u32 i = 0; i < mesh->submesh_count; ++i) {
            const SubmeshDesc& submesh = mesh->submeshes[i];
            vkCmdDrawIndexed(ctx.cmd, submesh.index_count, 1, submesh.first_index, static_cast<i32>(submesh.base_vertex), 0);
        }
    });
}

void Renderer::fullscreen(const RenderPassContext& ctx) {
    if (ctx.desc == nullptr || ctx.desc->shader == 0) {
        return;
    }
    const u64 tag = ctx.desc->tag_hash != 0 ? ctx.desc->tag_hash : FULLSCREEN_TAG;
    GraphicsPipeline* pipeline = this->pipeline_for(ctx.desc->shader, tag, ctx.compat_key, nullptr);
    const Shader* shader = this->world != nullptr ? this->world->get<Shader>(ctx.desc->shader) : nullptr;
    const ShaderPassProgram* pass = shader != nullptr ? shader->program.find_pass(tag) : nullptr;
    if (pipeline == nullptr || pass == nullptr) {
        return;
    }
    pipeline->bind(ctx.cmd);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout, 0, 1, &this->frame_set, 0, nullptr);
    const VkDescriptorSet inputs = this->pass_input_set(ctx, *pass);
    if (inputs != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout, 1, 1, &inputs, 0, nullptr);
    }
    if (pass->has_push_constants && ctx.desc->constant_size > 0) {
        const u32 size = ctx.desc->constant_size < pass->push_constants.size ? ctx.desc->constant_size : pass->push_constants.size;
        pipeline->push_constants(ctx.cmd, pass->push_constants.stageFlags, ctx.desc->constants, size, pass->push_constants.offset);
    }
    vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
}

void Renderer::custom(const RenderPassContext& ctx) {
    if (ctx.desc == nullptr || ctx.desc->callback_hash == 0) {
        return;
    }
    const CustomPass* entry = this->custom_passes.find(ctx.desc->callback_hash);
    if (entry == nullptr || entry->fn == nullptr) {
        return;
    }
    entry->fn(ctx, entry->user_data);
}
