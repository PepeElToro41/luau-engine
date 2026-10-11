#include "ui/inspectors/inspectors.hpp"

#include "engine/ecs/world.hpp"
#include "engine/geometry/primitives.hpp"
#include "engine/render/components.hpp"
#include "ui/inspectors/asset_combo.hpp"

#include <imgui.h>

bool INSPECTORS::primitive_renderer(InspectorContext& ctx, void* data) {
    PrimitiveRenderer& renderer = *static_cast<PrimitiveRenderer*>(data);
    bool changed = false;

    const char* current = PRIMITIVES::is_valid(renderer.shape) ? PRIMITIVES::name(renderer.shape) : "unknown";
    if (ImGui::BeginCombo("Shape", current)) {
        for (u32 i = 0; i < PRIMITIVE_COUNT; ++i) {
            const PrimitiveShape shape = static_cast<PrimitiveShape>(i);
            const bool selected = renderer.shape == shape;
            if (ImGui::Selectable(PRIMITIVES::name(shape), selected) && !selected) {
                renderer.shape = shape;
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    changed |= ASSET_COMBO::material(ctx, renderer.material);
    return changed;
}
