#include "ui/inspectors/inspectors.hpp"

#include "engine/asset/asset_types/material_asset.hpp"
#include "engine/asset/text_asset.hpp"
#include "engine/ecs/world.hpp"
#include "engine/render/components.hpp"
#include "engine/render/materials.hpp"
#include "engine/shaders/reflection.hpp"

#include <imgui.h>

#include <cstring>

// Edit state kept across frames: the texture or sampler field being typed
// into, so the text is not overwritten by the slot's value mid-edit.
struct MaterialScratch {
    // Which field owns `text`: 1 + slot index for textures, 101 + index
    // for samplers, 0 for none.
    u32 editing = 0;
    char text[96] = {};
};

static_assert(sizeof(MaterialScratch) <= INSPECTOR_SCRATCH_CAPACITY, "MaterialScratch must fit the inspector scratch");

// One block member as a field matching its reflected shape. Writes into
// `block` in place.
static bool draw_member(const ReflectedMember& member, u8* block, const u32 block_size) {
    const u32 components = static_cast<u32>(member.columns) * member.rows;
    if (components == 0 || member.offset + member.size > block_size) {
        return false;
    }
    ImGui::PushID(member.name);
    bool changed = false;
    u8* data = block + member.offset;
    if (member.scalar == REFLECT_SCALAR_FLOAT) {
        if (member.columns > 1) {
            // A matrix: one row of fields per column, column-major like the block.
            const u32 column_stride = member.size / member.columns;
            ImGui::TextUnformatted(member.name);
            for (u32 c = 0; c < member.columns; ++c) {
                ImGui::PushID(static_cast<int>(c));
                f32* column = reinterpret_cast<f32*>(data + c * column_stride);
                ImGui::SetNextItemWidth(-FLT_MIN);
                changed |= ImGui::DragScalarN("##column", ImGuiDataType_Float, column, static_cast<int>(member.rows), 0.01f);
                ImGui::PopID();
            }
        } else if (member.rows == 3 || member.rows == 4) {
            // Vectors of 3 or 4 get a swatch too: most of them are colors.
            f32* values = reinterpret_cast<f32*>(data);
            const ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR;
            changed = member.rows == 3 ? ImGui::ColorEdit3(member.name, values, flags) : ImGui::ColorEdit4(member.name, values, flags);
        } else {
            changed = ImGui::DragScalarN(member.name, ImGuiDataType_Float, data, static_cast<int>(member.rows), 0.01f);
        }
    } else if (member.scalar == REFLECT_SCALAR_INT) {
        changed = ImGui::DragScalarN(member.name, ImGuiDataType_S32, data, static_cast<int>(components), 0.1f);
    } else if (member.scalar == REFLECT_SCALAR_UINT) {
        changed = ImGui::DragScalarN(member.name, ImGuiDataType_U32, data, static_cast<int>(components), 0.1f);
    } else {
        // Bools are 32-bit in the block.
        for (u32 i = 0; i < components; ++i) {
            u32* flag = reinterpret_cast<u32*>(data) + i;
            bool value = *flag != 0;
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Checkbox(components == 1 ? member.name : "##flag", &value)) {
                *flag = value ? 1u : 0u;
                changed = true;
            }
            ImGui::PopID();
            if (components > 1 && i + 1 < components) {
                ImGui::SameLine();
            }
        }
        if (components > 1) {
            ImGui::SameLine();
            ImGui::TextUnformatted(member.name);
        }
    }
    ImGui::PopID();
    return changed;
}

// A text field whose contents belong to the scratch while it is being
// edited (`owner` identifies the field) and to `shown` otherwise. Returns
// true when Enter commits, with the typed text in `scratch.text`.
static bool draw_text_field(const char* label, const u32 owner, const char* shown, MaterialScratch& scratch) {
    const bool editing = scratch.editing == owner;
    char buffer[sizeof(scratch.text)];
    snprintf(buffer, sizeof(buffer), "%s", editing ? scratch.text : shown);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool committed = ImGui::InputText(label, buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemActive()) {
        scratch.editing = owner;
        snprintf(scratch.text, sizeof(scratch.text), "%s", buffer);
    } else if (editing && !committed) {
        scratch.editing = 0;
    }
    if (committed) {
        snprintf(scratch.text, sizeof(scratch.text), "%s", buffer);
        scratch.editing = 0;
    }
    return committed;
}

bool INSPECTORS::material(InspectorContext& ctx, void* data) {
    Material& material = *static_cast<Material*>(data);
    MaterialScratch* scratch = ctx.scratch<MaterialScratch>();
    MaterialScratch spill{};
    if (scratch == nullptr) {
        scratch = &spill;
    }

    const Shader* shader = ctx.world != nullptr ? ctx.world->get<Shader>(material.shader) : nullptr;
    if (shader == nullptr) {
        ImGui::TextDisabled("Shader: entity %llu (not a shader)", static_cast<unsigned long long>(material.shader));
        return false;
    }
    ImGui::Text("Shader: %s", shader->name);
    MATERIAL::sync(material, *shader);
    bool changed = false;

    // --- Block members ---
    const ShaderProgram& program = shader->program;
    const ShaderReflection& interface = program.material_interface;
    const ReflectedBinding* block = program.has_material_block() ? interface.find_binding(2, program.material_block_binding) : nullptr;
    if (block != nullptr && material.params != nullptr && block->member_count > 0) {
        ImGui::SeparatorText("Params");
        for (u32 m = 0; m < block->member_count; ++m) {
            changed |= draw_member(interface.members[block->first_member + m], material.params, material.param_size);
        }
    }

    // --- Textures ---
    if (material.texture_count > 0) {
        ImGui::SeparatorText("Textures");
        for (u32 i = 0; i < material.texture_count; ++i) {
            MaterialTextureSlot& slot = material.textures[i];
            const ReflectedBinding* binding = interface.find_binding(2, slot.binding);
            char label[REFLECT_NAME_MAX + 16];
            snprintf(label, sizeof(label), "%s", binding != nullptr ? binding->name : "texture");
            ImGui::PushID(static_cast<int>(slot.binding));
            ImGui::TextUnformatted(label);
            char shown[TEXT_ASSET::GUID_TEXT_CAPACITY];
            if (slot.texture.is_null()) {
                snprintf(shown, sizeof(shown), "none");
            } else {
                TEXT_ASSET::format_guid(slot.texture, shown);
            }
            if (draw_text_field("##guid", 1 + i, shown, *scratch)) {
                AssetGuid guid;
                if (TEXT_ASSET::word_is(scratch->text, strlen(scratch->text), "none") || scratch->text[0] == '\0') {
                    slot.texture = AssetGuid{};
                    changed = true;
                } else if (TEXT_ASSET::parse_guid(scratch->text, strlen(scratch->text), &guid)) {
                    slot.texture = guid;
                    changed = true;
                }
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("Texture asset GUID (32 hex digits) or none");
            }
            ImGui::PopID();
        }
    }

    // --- Samplers ---
    if (material.sampler_count > 0) {
        ImGui::SeparatorText("Samplers");
        for (u32 i = 0; i < material.sampler_count; ++i) {
            MaterialSamplerSlot& slot = material.samplers[i];
            const ReflectedBinding* binding = interface.find_binding(2, slot.binding);
            ImGui::PushID(100 + static_cast<int>(slot.binding));
            ImGui::TextUnformatted(binding != nullptr ? binding->name : "sampler");
            char shown[96];
            MATERIAL_ASSET::format_sampler(MATERIAL::sampler_asset(slot.sampler), shown, sizeof(shown));
            if (draw_text_field("##words", 101 + i, shown, *scratch)) {
                MaterialSamplerDesc parsed;
                if (MATERIAL_ASSET::parse_sampler(scratch->text, strlen(scratch->text), &parsed)) {
                    slot.sampler = MATERIAL::sampler_desc(parsed);
                    changed = true;
                }
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("linear | nearest, repeat | clamp | mirror, min= mag= mip= u= v= w= anisotropy=");
            }
            ImGui::PopID();
        }
    }

    if (!material.asset.is_null()) {
        char guid[TEXT_ASSET::GUID_TEXT_CAPACITY];
        TEXT_ASSET::format_guid(material.asset, guid);
        ImGui::TextDisabled("asset %s", guid);
    }
    return changed;
}
