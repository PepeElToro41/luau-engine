#include "engine/render/components.hpp"

#include "engine/asset/asset_entity.hpp"
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

// A write to a Material that came from a file (the Inspector writing the
// block and calling World::modified, MATERIAL::set_*, set_shader) puts the
// file behind the entity: tag it AssetDirty so an editor's Save All finds
// it. Only CHANGED counts: the MOVED a set() raises is the load itself,
// and MATERIAL::load / reload / save clear the tag when file and entity
// agree again. The observer lives as long as the World.
static void material_changed(World* world, const EntityId entity, const ObserverEvent event, Id, void*) {
    if (event != OBSERVER_CHANGED) {
        return;
    }
    const Material* material = world->get<Material>(entity);
    if (material != nullptr && !material->asset.is_null()) {
        world->add<AssetDirty>(entity);
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
    world.query<Material>().observe(material_changed);
}
