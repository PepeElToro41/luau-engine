#pragma once

#include "engine/asset/asset_view.hpp"
#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/ecs/hooks.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/templates/hash_map.hpp"

// Assets as entities. Every asset the engine knows (what the
// AssetResourceProvider has registered) is also an entity of the World, so
// the rest of the engine and the editor can reach assets the way they
// reach everything else: by query, by component, by name.
//
//     AssetUuid             the asset's GUID; what makes an entity an asset entity
//     (AssetType, T)        exclusive relation; T is one of the tags below and
//                           says what kind of asset it is
//     Name                  the file's stem, for display (scene.hpp)
//
//     world.query<AssetUuid>().with<ECS::Pair<AssetType, AssetTexture>>().each([](EntityId e, AssetUuid& asset) { ... });
//
// AssetEntities is the index from GUID to entity that keeps one entity per
// asset: add() creates the entity the first time a GUID is seen and
// updates its type and name afterwards, find() looks one up, remove()
// deletes it. The runtime Engine creates one as a singleton and fills it
// from load_asset_file(), so registering a file is what makes its entity.
// Deleting an asset entity by any other route (world.delete_entity) drops
// it from the index through the removed hook on AssetUuid.
//
// Asset entities are unparented, like the renderer's Shader and Material
// entities, so they never show in the scene tree. A loader may attach its
// runtime component to the asset entity itself (MATERIAL::load puts its
// Material on the .material's asset entity), which is what makes the file,
// the asset and the thing drawn one entity.

struct World;

// The asset an entity stands for.
struct AssetUuid {
    AssetGuid guid;
};

// Relation on asset entities: (AssetType, AssetTexture) and so on, one per
// entity (exclusive).
struct AssetType {};

// Targets of AssetType, one per ASSET_TYPE::* value.
struct AssetTexture {};
struct AssetMesh {};
struct AssetMaterial {};
struct AssetScene {};
struct AssetShader {};

namespace ASSET_ENTITY {

// Registers and names the components above on `world` and makes AssetType
// exclusive. Idempotent; AssetEntities::init calls it.
void register_components(World& world);

// The tag entity for an ASSET_TYPE::* value (world.tag<AssetTexture>() for
// ASSET_TYPE::TEXTURE, ...), or 0 for a type this header does not know.
EntityId type_tag(World& world, u32 asset_type);
// The ASSET_TYPE::* value of the (AssetType, T) pair on `entity`, or 0 if
// it holds none or is not alive.
u32 type_of(World& world, EntityId entity);
// A lowercase word for an ASSET_TYPE::* value: "texture", "mesh",
// "material", "scene", "shader", or "unknown".
const char* type_name(u32 asset_type);

// The entity holding AssetUuid `guid`, found by scanning the asset
// entities, or 0 if none. AssetEntities::find is the indexed lookup; this
// is for code that has only the World.
EntityId find(World& world, const AssetGuid& guid);

// The file name without directories or extension, cut to `capacity`:
// what an asset entity is named after. False if it was cut.
bool name_from_path(const char* path, char* out, usz capacity);

} // namespace ASSET_ENTITY

struct AssetEntities {
    AssetEntities();
    explicit AssetEntities(BaseAllocator* allocator);

    AssetEntities(const AssetEntities&) = delete;
    AssetEntities& operator=(const AssetEntities&) = delete;

    World* world = nullptr;

    // Registers the components on `world`, which must be initialized and
    // outlive this, and installs the removed hook that keeps the index in
    // step with the World.
    void init(World* world);

    // The entity for `guid`, created with AssetUuid, (AssetType, type_tag)
    // and Name `name` the first time, otherwise the existing one with its
    // type pair and name brought up to date. `asset_type` is an
    // ASSET_TYPE::* value; an unknown one leaves the entity without a type
    // pair. Returns 0 for a null GUID or when no entity could be created.
    EntityId add(const AssetGuid& guid, u32 asset_type, const char* name);
    // add() with the GUID and type from `view`'s header and the name from
    // `path` (ASSET_ENTITY::name_from_path). 0 when `view` is not ok.
    EntityId add(const AssetView& view, const char* path);

    // The entity for `guid`, or 0 if none (never added, or deleted since).
    EntityId find(const AssetGuid& guid) const;
    bool contains(const AssetGuid& guid) const { return this->find(guid) != 0; }
    // Deletes the entity for `guid` and forgets it. False if unknown.
    bool remove(const AssetGuid& guid);

    usz count() const { return this->entities.count; }

    // Releases the index and the hook. The entities stay in the World.
    void free();

private:
    HashMap<AssetGuid, EntityId, AssetGuidHash> entities;
    HookId removed_hook = 0;

    static void on_removed(World* world, EntityId entity, Id id, void* user_data);
};
