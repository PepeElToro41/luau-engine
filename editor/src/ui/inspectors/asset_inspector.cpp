#include "ui/inspectors/inspectors.hpp"

#include "engine/asset/asset_entity.hpp"
#include "engine/asset/text_asset.hpp"
#include "engine/ecs/world.hpp"

#include <imgui.h>

bool INSPECTORS::asset_uuid(InspectorContext& ctx, void* data) {
    const AssetUuid& asset = *static_cast<const AssetUuid*>(data);
    char guid[TEXT_ASSET::GUID_TEXT_CAPACITY];
    TEXT_ASSET::format_guid(asset.guid, guid);
    // Read-only: the GUID is the asset's identity, the type comes from the
    // (AssetType, tag) pair the index put on the entity.
    ImGui::Text("Type: %s", ctx.world != nullptr ? ASSET_ENTITY::type_name(ASSET_ENTITY::type_of(*ctx.world, ctx.entity)) : "unknown");
    ImGui::TextUnformatted("GUID");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##guid", guid, sizeof(guid), ImGuiInputTextFlags_ReadOnly);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Select the text to copy it");
    }
    return false;
}
