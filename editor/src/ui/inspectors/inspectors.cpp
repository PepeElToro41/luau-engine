#include "ui/inspectors/inspectors.hpp"

#include "engine/asset/asset_entity.hpp"
#include "engine/ecs/world.hpp"
#include "engine/render/components.hpp"
#include "engine/scene/inspector.hpp"

void INSPECTORS::register_all(World& world) {
    INSPECTOR::expose<Transform>(world, "Transform", &INSPECTORS::transform, 0);
    INSPECTOR::expose<Camera>(world, "Camera", &INSPECTORS::camera, 10);
    INSPECTOR::expose<PrimitiveRenderer>(world, "PrimitiveRenderer", &INSPECTORS::primitive_renderer, 20);
    INSPECTOR::expose<Material>(world, "Material", &INSPECTORS::material, 30);
    INSPECTOR::expose<AssetUuid>(world, "Asset", &INSPECTORS::asset_uuid, 40);

    // What "Add Component" offers. Shader, Material and the asset
    // components are created by code (a shader file, a .material, an
    // import), so they are deliberately left out.
    INSPECTOR::addable<Transform>(world);
    INSPECTOR::addable<Camera>(world);
    INSPECTOR::addable<PrimitiveRenderer>(world, &INSPECTORS::default_primitive_renderer);
}

void INSPECTORS::default_primitive_renderer(World& world, EntityId, void* data) {
    PrimitiveRenderer renderer;
    // The lowest-id Material entity, so the shape shows up as soon as it is
    // added instead of drawing nothing until one is picked.
    EntityId material = 0;
    world.query<Material>().each([&](const EntityId entity, Material&) {
        if (material == 0 || ECS::ENTITY_LOW(entity) < ECS::ENTITY_LOW(material)) {
            material = entity;
        }
    });
    renderer.material = material;
    *static_cast<PrimitiveRenderer*>(data) = renderer;
}
