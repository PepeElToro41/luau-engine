#include "ui/inspectors/inspectors.hpp"

#include "engine/ecs/world.hpp"
#include "engine/geometry/primitives.hpp"
#include "engine/render/components.hpp"
#include "engine/scene/scene.hpp"

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

    if (renderer.material == 0) {
        ImGui::TextDisabled("Material: none");
    } else if (ctx.world != nullptr && ctx.world->alive(renderer.material)) {
        const char* name = SCENE::name(*ctx.world, renderer.material);
        ImGui::Text("Material: %s", name != nullptr && name[0] != 0 ? name : "(unnamed)");
    } else {
        ImGui::TextDisabled("Material: entity %llu (dead)", static_cast<unsigned long long>(renderer.material));
    }
    return changed;
}
