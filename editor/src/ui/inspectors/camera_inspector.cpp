#include "ui/inspectors/inspectors.hpp"

#include "engine/math/math.hpp"
#include "engine/render/components.hpp"

#include <imgui.h>

bool INSPECTORS::camera(InspectorContext&, void* data) {
    Camera& camera = *static_cast<Camera*>(data);
    bool changed = false;

    f32 fov_degrees = MATH::degrees(camera.fov_y);
    if (ImGui::SliderFloat("Field of view", &fov_degrees, 1.0f, 179.0f, "%.1f")) {
        camera.fov_y = MATH::radians(fov_degrees);
        changed = true;
    }
    if (ImGui::DragFloat("Near plane", &camera.near_plane, 0.01f, 0.001f, camera.far_plane, "%.3f")) {
        changed = true;
    }
    if (ImGui::DragFloat("Far plane", &camera.far_plane, 1.0f, camera.near_plane, 100000.0f, "%.1f")) {
        changed = true;
    }
    return changed;
}
