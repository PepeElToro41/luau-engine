#include "engine/render/components.hpp"

#include "engine/ecs/world.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/render/renderer.hpp"
#include "engine/scene/scene.hpp"

static void shader_removed(World* world, const EntityId entity, Id, void* user_data) {
    Renderer* renderer = static_cast<Renderer*>(user_data);
    if (Shader* shader = world->get<Shader>(entity)) {
        PIPELINES::release_shader(*renderer, entity);
        shader->program.free();
    }
}

static void material_removed(World* world, const EntityId entity, Id, void* user_data) {
    Renderer* renderer = static_cast<Renderer*>(user_data);
    if (Material* material = world->get<Material>(entity)) {
        MEMORY::heap_allocator()->free(material->params);
        material->params = nullptr;
        material->param_size = 0;
        if (!material->asset.is_null()) {
            const EntityId* loaded = renderer->materials.find(material->asset);
            if (loaded != nullptr && *loaded == entity) {
                renderer->materials.remove(material->asset);
            }
        }
    }
}

void RENDER_COMPONENTS::register_all(World& world, Renderer& renderer) {
    // Named so the editor can list them by name even where nothing draws them.
    SCENE::set_name(world, world.component<Transform>(), "Transform");
    SCENE::set_name(world, world.component<Camera>(), "Camera");
    SCENE::set_name(world, world.tag<RenderCamera>(), "RenderCamera");
    SCENE::set_name(world, world.component<MeshRenderer>(), "MeshRenderer");
    SCENE::set_name(world, world.component<PrimitiveRenderer>(), "PrimitiveRenderer");
    SCENE::set_name(world, world.component<Shader>(), "Shader");
    SCENE::set_name(world, world.component<Material>(), "Material");
    world.hook_removed<Shader>(shader_removed, &renderer);
    world.hook_removed<Material>(material_removed, &renderer);
}
