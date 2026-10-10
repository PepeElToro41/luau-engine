#include "ui/inspectors/inspectors.hpp"

#include "engine/math/math.hpp"
#include "engine/render/components.hpp"

#include <imgui.h>

namespace {

// Edit state kept between frames. The quaternion is the one the fields
// were last derived from or written to: while it still matches the
// component, the Euler values shown are the ones the user typed, so they
// do not snap to a different but equivalent triple (or jump near the
// gimbal lock) every frame. Zero bytes mean "not derived yet".
struct TransformEdit {
    Quaternion written;
    f32 euler_degrees[3];
    bool valid;
};

bool drag3(const char* label, Vector3& value, const f32 speed) {
    f32 xyz[3];
    value.store(xyz);
    if (!ImGui::DragFloat3(label, xyz, speed, 0.0f, 0.0f, "%.3f")) {
        return false;
    }
    value = Vector3::load(xyz);
    return true;
}

} // namespace

bool INSPECTORS::transform(InspectorContext& ctx, void* data) {
    Transform& transform = *static_cast<Transform*>(data);
    TransformEdit* edit = ctx.scratch<TransformEdit>();
    bool changed = false;

    changed |= drag3("Position", transform.position, 0.05f);

    if (!edit->valid || transform.rotation != edit->written) {
        // First look, or something else rotated the entity: re-derive.
        // + 0 turns a -0 into 0 so the field never shows "-0.0".
        const Vector3 euler = transform.rotation.to_euler();
        edit->euler_degrees[0] = MATH::degrees(euler.x) + 0.0f;
        edit->euler_degrees[1] = MATH::degrees(euler.y) + 0.0f;
        edit->euler_degrees[2] = MATH::degrees(euler.z) + 0.0f;
        edit->written = transform.rotation;
        edit->valid = true;
    }
    if (ImGui::DragFloat3("Rotation", edit->euler_degrees, 0.5f, 0.0f, 0.0f, "%.1f")) {
        transform.rotation = Quaternion::from_euler(
            MATH::radians(edit->euler_degrees[0]),
            MATH::radians(edit->euler_degrees[1]),
            MATH::radians(edit->euler_degrees[2]));
        edit->written = transform.rotation;
        changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("pitch (X), yaw (Y), roll (Z) in degrees");
    }

    changed |= drag3("Scale", transform.scale, 0.01f);
    return changed;
}
