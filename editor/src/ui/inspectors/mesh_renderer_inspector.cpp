#include "ui/inspectors/inspectors.hpp"

#include "engine/ecs/world.hpp"
#include "engine/render/components.hpp"
#include "ui/inspectors/asset_combo.hpp"

#include <imgui.h>

bool INSPECTORS::mesh_renderer(InspectorContext& ctx, void* data) {
    MeshRenderer& renderer = *static_cast<MeshRenderer*>(data);
    bool changed = false;
    changed |= ASSET_COMBO::mesh(ctx, renderer.mesh);
    changed |= ASSET_COMBO::material(ctx, renderer.material);
    return changed;
}
