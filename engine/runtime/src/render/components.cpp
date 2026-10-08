#include "engine/render/components.hpp"

#include "engine/ecs/world.hpp"
#include "engine/render/renderer.hpp"

static void shader_removed(World* world, const EntityId entity, Id, void* user_data) {
    Renderer* renderer = static_cast<Renderer*>(user_data);
    if (Shader* shader = world->get<Shader>(entity)) {
        renderer->on_shader_removed(entity, *shader);
    }
}

static void material_removed(World* world, const EntityId entity, Id, void* user_data) {
    Renderer* renderer = static_cast<Renderer*>(user_data);
    if (Material* material = world->get<Material>(entity)) {
        renderer->on_material_removed(entity, *material);
    }
}

void RENDER_COMPONENTS::register_all(World& world, Renderer& renderer) {
    world.component<Transform>();
    world.component<Camera>();
    world.component<MeshRenderer>();
    world.component<Shader>();
    world.component<Material>();
    world.hook_removed<Shader>(shader_removed, &renderer);
    world.hook_removed<Material>(material_removed, &renderer);
}
