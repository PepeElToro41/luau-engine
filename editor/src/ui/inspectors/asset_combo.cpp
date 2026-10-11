#include "ui/inspectors/asset_combo.hpp"

#include "engine/asset/asset_entity.hpp"
#include "engine/asset/text_asset.hpp"
#include "engine/ecs/query/query.hpp"
#include "engine/ecs/world.hpp"
#include "engine/render/components.hpp"
#include "engine/scene/scene.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstring>

// --- Collecting -----------------------------------------------------------------

template <typename Tag>
static void collect_tag(World& world, std::vector<AssetChoice>& out) {
    world.query<AssetUuid>().with<ECS::Pair<AssetType, Tag>>().each([&](const EntityId entity, const AssetUuid& asset) {
        AssetChoice choice;
        choice.entity = entity;
        choice.guid = asset.guid;
        const char* name = SCENE::name(world, entity);
        snprintf(choice.name, sizeof(choice.name), "%s", name != nullptr && name[0] != '\0' ? name : "(unnamed)");
        out.push_back(choice);
    });
}

void ASSET_COMBO::collect(World& world, const u32 type, std::vector<AssetChoice>& out) {
    switch (type) {
    case ASSET_TYPE::TEXTURE:
        collect_tag<AssetTexture>(world, out);
        break;
    case ASSET_TYPE::MESH:
        collect_tag<AssetMesh>(world, out);
        break;
    case ASSET_TYPE::MATERIAL:
        collect_tag<AssetMaterial>(world, out);
        break;
    case ASSET_TYPE::SCENE:
        collect_tag<AssetScene>(world, out);
        break;
    case ASSET_TYPE::SHADER:
        collect_tag<AssetShader>(world, out);
        break;
    default:
        return;
    }
    std::sort(out.begin(), out.end(), [](const AssetChoice& a, const AssetChoice& b) {
        const int order = strcmp(a.name, b.name);
        return order != 0 ? order < 0 : a.entity < b.entity;
    });
}

const AssetChoice* ASSET_COMBO::find_entity(const std::vector<AssetChoice>& choices, const EntityId entity) {
    if (entity == 0) {
        return nullptr;
    }
    for (const AssetChoice& choice : choices) {
        if (choice.entity == entity) {
            return &choice;
        }
    }
    return nullptr;
}

const AssetChoice* ASSET_COMBO::find_guid(const std::vector<AssetChoice>& choices, const AssetGuid& guid) {
    if (guid.is_null()) {
        return nullptr;
    }
    for (const AssetChoice& choice : choices) {
        if (choice.guid == guid) {
            return &choice;
        }
    }
    return nullptr;
}

// --- The combo --------------------------------------------------------------------

bool ASSET_COMBO::draw(const char* id, const char* preview, const AssetChoice* current, const bool current_is_none, const std::vector<AssetChoice>& choices, const bool allow_none,
    const AssetChoice** picked) {
    char guid_text[TEXT_ASSET::GUID_TEXT_CAPACITY];
    bool changed = false;
    *picked = nullptr;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(id, preview)) {
        if (allow_none) {
            if (ImGui::Selectable("none", current_is_none) && !current_is_none) {
                changed = true;
            }
            if (current_is_none) {
                ImGui::SetItemDefaultFocus();
            }
        }
        if (current == nullptr && !current_is_none) {
            // Keep the unresolved value in the list so it is visibly what is set.
            ImGui::Selectable(preview, true);
            ImGui::SetItemDefaultFocus();
        }
        for (const AssetChoice& choice : choices) {
            const bool selected = &choice == current;
            ImGui::PushID(static_cast<int>(choice.entity & 0x7fffffffu));
            ImGui::PushID(static_cast<int>(choice.entity >> 32));
            if (ImGui::Selectable(choice.name, selected) && !selected) {
                *picked = &choice;
                changed = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                TEXT_ASSET::format_guid(choice.guid, guid_text);
                ImGui::SetTooltip("%s", guid_text);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && current != nullptr) {
        TEXT_ASSET::format_guid(current->guid, guid_text);
        ImGui::SetTooltip("%s", guid_text);
    }
    return changed;
}

// --- Per-component combos ---------------------------------------------------------

// The preview for an entity field: the choice's name, or what the entity
// is when the list does not hold it.
static void entity_preview(World* world, const EntityId entity, const AssetChoice* current, char* out, const usz capacity) {
    if (entity == 0) {
        snprintf(out, capacity, "none");
    } else if (current != nullptr) {
        snprintf(out, capacity, "%s", current->name);
    } else if (world != nullptr && world->alive(entity)) {
        const char* name = SCENE::name(*world, entity);
        snprintf(out, capacity, "%s (not an asset)", name != nullptr && name[0] != '\0' ? name : "(unnamed)");
    } else {
        snprintf(out, capacity, "entity %llu (dead)", static_cast<unsigned long long>(entity));
    }
}

bool ASSET_COMBO::material(InspectorContext& ctx, EntityId& material) {
    std::vector<AssetChoice> choices;
    if (ctx.world != nullptr) {
        collect(*ctx.world, ASSET_TYPE::MATERIAL, choices);
    }
    const AssetChoice* current = find_entity(choices, material);
    char preview[ENTITY_NAME_CAPACITY + 32];
    entity_preview(ctx.world, material, current, preview, sizeof(preview));

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Material");
    ImGui::SameLine();
    const AssetChoice* picked = nullptr;
    const bool changed = draw("##material", preview, current, material == 0, choices, true, &picked);
    if (changed) {
        material = picked != nullptr ? picked->entity : 0;
        if (picked != nullptr && ctx.world != nullptr && !ctx.world->has<Material>(picked->entity)) {
            // The file is registered but nothing turned it into a Material
            // yet: the App loads it after the draw.
            ctx.request.kind = InspectorRequestKind::LOAD_MATERIAL;
            ctx.request.target = ctx.entity;
            ctx.request.asset = picked->entity;
        }
    } else if (current != nullptr && ctx.world != nullptr && !ctx.world->has<Material>(material)) {
        ImGui::TextDisabled("(not loaded, draws nothing)");
    }
    return changed;
}

bool ASSET_COMBO::mesh(InspectorContext& ctx, AssetGuid& mesh) {
    std::vector<AssetChoice> choices;
    if (ctx.world != nullptr) {
        collect(*ctx.world, ASSET_TYPE::MESH, choices);
    }
    const AssetChoice* current = find_guid(choices, mesh);
    char guid_text[TEXT_ASSET::GUID_TEXT_CAPACITY];
    char preview[TEXT_ASSET::GUID_TEXT_CAPACITY + 32];
    if (mesh.is_null()) {
        snprintf(preview, sizeof(preview), "none");
    } else if (current != nullptr) {
        snprintf(preview, sizeof(preview), "%s", current->name);
    } else {
        TEXT_ASSET::format_guid(mesh, guid_text);
        snprintf(preview, sizeof(preview), "%s (not registered)", guid_text);
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Mesh");
    ImGui::SameLine();
    const AssetChoice* picked = nullptr;
    const bool changed = draw("##mesh", preview, current, mesh.is_null(), choices, true, &picked);
    if (changed) {
        mesh = picked != nullptr ? picked->guid : AssetGuid{};
    }
    return changed;
}

bool ASSET_COMBO::shader(InspectorContext& ctx, const EntityId shader) {
    std::vector<AssetChoice> choices;
    if (ctx.world != nullptr) {
        collect(*ctx.world, ASSET_TYPE::SHADER, choices);
    }
    const AssetChoice* current = find_entity(choices, shader);
    const Shader* loaded = ctx.world != nullptr ? ctx.world->get<Shader>(shader) : nullptr;
    char preview[ENTITY_NAME_CAPACITY + 32];
    if (current != nullptr) {
        snprintf(preview, sizeof(preview), "%s", current->name);
    } else if (loaded != nullptr) {
        snprintf(preview, sizeof(preview), "%s (not an asset)", loaded->name);
    } else if (shader == 0) {
        snprintf(preview, sizeof(preview), "none");
    } else {
        snprintf(preview, sizeof(preview), "entity %llu (not a shader)", static_cast<unsigned long long>(shader));
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Shader");
    ImGui::SameLine();
    const AssetChoice* picked = nullptr;
    // No "none": a material without a shader has no interface to edit.
    if (!draw("##shader", preview, current, false, choices, false, &picked) || picked == nullptr) {
        return false;
    }
    ctx.request.kind = InspectorRequestKind::SET_MATERIAL_SHADER;
    ctx.request.target = ctx.entity;
    ctx.request.asset = picked->entity;
    return true;
}
