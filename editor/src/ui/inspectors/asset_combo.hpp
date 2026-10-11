#pragma once

#include "engine/asset/asset_view.hpp"
#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/scene/scene.hpp"
#include "ui/inspectors/inspector_context.hpp"

#include <vector>

struct World;

// The dropdowns the inspectors pick assets with. Every choice is an asset
// entity (AssetUuid with (AssetType, tag), see engine/asset/asset_entity.hpp)
// of one type, listed by Name: a texture slot picks a texture asset, a
// MeshRenderer a mesh asset and a material asset, a PrimitiveRenderer a
// material asset, a Material a shader asset. The current value is shown even
// when it is not in the list (a GUID nothing registered, an entity built in
// code), selectable so opening the combo never changes it.

// One asset entity a combo can pick: the entity, its GUID and its Name.
struct AssetChoice {
    EntityId entity = 0;
    AssetGuid guid;
    char name[ENTITY_NAME_CAPACITY] = {};
};

namespace ASSET_COMBO {

// Every asset entity of ASSET_TYPE::`type` in `world`
// (world.query<AssetUuid>().with<ECS::Pair<AssetType, tag>>()), sorted by
// name then entity so the list reads the same from frame to frame.
void collect(World& world, u32 type, std::vector<AssetChoice>& out);
// The choice for an entity / a GUID, or nullptr.
const AssetChoice* find_entity(const std::vector<AssetChoice>& choices, EntityId entity);
const AssetChoice* find_guid(const std::vector<AssetChoice>& choices, const AssetGuid& guid);

// A full-width combo over `choices`. `current` is the choice the value
// resolves to, nullptr when it is none (`current_is_none`) or something
// the list does not hold, in which case `preview` is kept as a selected
// row. With `allow_none` a "none" row comes first. True when the user
// picked another row: `*picked` is the choice, nullptr for none.
bool draw(const char* id, const char* preview, const AssetChoice* current, bool current_is_none, const std::vector<AssetChoice>& choices, bool allow_none, const AssetChoice** picked);

// --- Per-component combos ---------------------------------------------------------

// A material entity field (MeshRenderer / PrimitiveRenderer::material) as a
// combo over the material assets plus none. Picking an asset nobody loaded
// yet points the field at its entity at once and files a LOAD_MATERIAL
// request in `ctx`. Returns true when the field changed.
bool material(InspectorContext& ctx, EntityId& material);
// A mesh asset field (MeshRenderer::mesh) as a combo over the mesh assets
// plus none. Returns true when the field changed.
bool mesh(InspectorContext& ctx, AssetGuid& mesh);
// A Material's shader entity as a combo over the shader assets. Never
// writes the field: the pick is filed as a SET_MATERIAL_SHADER request in
// `ctx`, since the shader may still have to be compiled. Returns true when
// a request was filed.
bool shader(InspectorContext& ctx, EntityId shader);

} // namespace ASSET_COMBO
