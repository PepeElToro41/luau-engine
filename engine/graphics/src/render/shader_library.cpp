#include "engine/render/shader_library.hpp"

#include "engine/ecs/world.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/platform/file.hpp"
#include "engine/render/components.hpp"
#include "engine/render/renderer.hpp"
#include "engine/scene/scene.hpp"
#include "engine/templates/dynamic_array.hpp"

#include <cstdio>
#include <cstring>

// --- Helpers -------------------------------------------------------------------

static bool resolve_path(const Renderer& renderer, const char* name, char* out, const usz out_size) {
    const char* roots[2] = {renderer.project_render_dir, renderer.engine_render_dir};
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

// Compiles the file and, on success, the bind layouts its passes use.
static bool load_program(Renderer& renderer, const char* name, const char* path, Shader& shader) {
    const char* include_dirs[2];
    usz include_dir_count = 0;
    if (renderer.project_render_dir[0] != '\0') {
        include_dirs[include_dir_count++] = renderer.project_render_dir;
    }
    if (renderer.engine_render_dir[0] != '\0') {
        include_dirs[include_dir_count++] = renderer.engine_render_dir;
    }
    ShaderProgramLoadDesc desc;
    desc.path = path;
    desc.include_dirs = include_dirs;
    desc.include_dir_count = include_dir_count;
    desc.frame_interface = &renderer.frame_interface;
    desc.target = renderer.gpu->info.bytecode;
    bool ok = SHADER_PROGRAM::load(desc, MEMORY::heap_allocator(), shader.program);
    if (shader.program.log != nullptr && shader.program.log[0] != '\0') {
        RENDERER::log(renderer, ok ? RENDER_LOG_WARNING : RENDER_LOG_ERROR, "shader %s:\n%s", path, shader.program.log);
    }
    if (!ok) {
        return false;
    }

    char error[256];
    GpuBindLayoutDesc layout;
    shader.material_layout = GpuBindLayout{};
    if (shader.program.material_interface.binding_count > 0) {
        if (!SHADER_REFLECT::layout_bindings(shader.program.material_interface, 2, layout, error, sizeof(error))) {
            RENDERER::log(renderer, RENDER_LOG_ERROR, "shader %s: material set: %s", name, error);
            return false;
        }
        shader.material_layout = GPU::bind_layout(renderer.gpu, layout);
        ok &= shader.material_layout.is_valid();
    }
    for (u32 p = 0; p < shader.program.pass_count; ++p) {
        const ShaderPassProgram& pass = shader.program.passes[p];
        shader.input_layouts[p] = GpuBindLayout{};
        if ((pass.reflection.set_mask() & 2u) == 0) {
            continue;
        }
        if (!SHADER_REFLECT::layout_bindings(pass.reflection, 1, layout, error, sizeof(error))) {
            RENDERER::log(renderer, RENDER_LOG_ERROR, "shader %s, pass %s: inputs: %s", name, pass.tag_name, error);
            return false;
        }
        shader.input_layouts[p] = GPU::bind_layout(renderer.gpu, layout);
        ok &= shader.input_layouts[p].is_valid();
    }
    return ok;
}

// --- SHADER_LIBRARY --------------------------------------------------------------

EntityId SHADER_LIBRARY::load(Renderer& renderer, const char* name) {
    if (name == nullptr || name[0] == '\0' || renderer.world == nullptr) {
        return 0;
    }
    // Already loaded under this name?
    EntityId existing = 0;
    renderer.world->query<Shader>().each([&](const EntityId entity, Shader& shader) {
        if (existing == 0 && strcmp(shader.name, name) == 0) {
            existing = entity;
        }
    });
    if (existing != 0) {
        return existing;
    }

    char path[RENDERER_PATH_MAX];
    if (!resolve_path(renderer, name, path, sizeof(path))) {
        RENDERER::log(renderer, RENDER_LOG_ERROR, "no %s.slang (or .glsl) in the project or engine render directories", name);
        return 0;
    }
    Shader shader;
    strncpy(shader.name, name, sizeof(shader.name) - 1);
    if (!load_program(renderer, name, path, shader)) {
        shader.program.free();
        return 0;
    }
    // Named, unparented: shaders are not scene content (see scene/scene.hpp).
    const EntityId entity = SCENE::spawn(*renderer.world, name, 0);
    if (entity == 0) {
        shader.program.free();
        return 0;
    }
    renderer.world->set(entity, shader);
    return entity;
}

bool SHADER_LIBRARY::reload(Renderer& renderer, const EntityId entity) {
    Shader* shader = renderer.world != nullptr ? renderer.world->get<Shader>(entity) : nullptr;
    if (shader == nullptr) {
        return false;
    }
    char path[RENDERER_PATH_MAX];
    if (!resolve_path(renderer, shader->name, path, sizeof(path))) {
        RENDERER::log(renderer, RENDER_LOG_ERROR, "shader %s: file disappeared", shader->name);
        return false;
    }
    Shader fresh;
    strncpy(fresh.name, shader->name, sizeof(fresh.name) - 1);
    if (!load_program(renderer, shader->name, path, fresh)) {
        fresh.program.free();
        return false;
    }
    PIPELINES::release_shader(renderer, entity);
    shader->program.free();
    shader->program = fresh.program;
    shader->material_layout = fresh.material_layout;
    for (u32 p = 0; p < SHADER_PREPROCESSING::MAX_PASSES; ++p) {
        shader->input_layouts[p] = fresh.input_layouts[p];
    }
    shader->generation += 1;
    renderer.world->modified<Shader>(entity);
    return true;
}

void SHADER_LIBRARY::reload_all(Renderer& renderer) {
    if (renderer.world == nullptr) {
        return;
    }
    DynamicArray<EntityId> entities;
    renderer.world->query<Shader>().each([&](const EntityId entity, Shader&) { entities.push(entity); });
    for (const EntityId entity : entities) {
        reload(renderer, entity);
    }
    entities.free();
}
