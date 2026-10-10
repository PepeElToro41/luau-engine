#include "ui/inspectors/inspectors.hpp"

#include "engine/asset/asset_entity.hpp"
#include "engine/render/components.hpp"
#include "engine/scene/inspector.hpp"

void INSPECTORS::register_all(World& world) {
    INSPECTOR::expose<Transform>(world, "Transform", &INSPECTORS::transform, 0);
    INSPECTOR::expose<Camera>(world, "Camera", &INSPECTORS::camera, 10);
    INSPECTOR::expose<PrimitiveRenderer>(world, "PrimitiveRenderer", &INSPECTORS::primitive_renderer, 20);
    INSPECTOR::expose<Material>(world, "Material", &INSPECTORS::material, 30);
    INSPECTOR::expose<AssetUuid>(world, "Asset", &INSPECTORS::asset_uuid, 40);
}
