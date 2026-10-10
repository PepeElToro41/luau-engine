#include "engine/asset/asset_entity.hpp"

#include "engine/asset/text_asset.hpp"
#include "engine/ecs/query/query.hpp"
#include "engine/ecs/world.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/scene/scene.hpp"

#include <cstring>

namespace {

// Every ASSET_TYPE::* value with a tag, for the lookups below.
constexpr u32 KNOWN_TYPES[] = {ASSET_TYPE::TEXTURE, ASSET_TYPE::MESH, ASSET_TYPE::MATERIAL, ASSET_TYPE::SCENE, ASSET_TYPE::SHADER};

} // namespace

// --- ASSET_ENTITY -----------------------------------------------------------

void ASSET_ENTITY::register_components(World& world) {
    SCENE::set_name(world, world.component<AssetUuid>(), "AssetUuid");
    const EntityId type = world.tag<AssetType>();
    SCENE::set_name(world, type, "AssetType");
    world.add(type, ECS::EXCLUSIVE);
    SCENE::set_name(world, world.tag<AssetTexture>(), "AssetTexture");
    SCENE::set_name(world, world.tag<AssetMesh>(), "AssetMesh");
    SCENE::set_name(world, world.tag<AssetMaterial>(), "AssetMaterial");
    SCENE::set_name(world, world.tag<AssetScene>(), "AssetScene");
    SCENE::set_name(world, world.tag<AssetShader>(), "AssetShader");
}

EntityId ASSET_ENTITY::type_tag(World& world, const u32 asset_type) {
    switch (asset_type) {
    case ASSET_TYPE::TEXTURE:
        return world.tag<AssetTexture>();
    case ASSET_TYPE::MESH:
        return world.tag<AssetMesh>();
    case ASSET_TYPE::MATERIAL:
        return world.tag<AssetMaterial>();
    case ASSET_TYPE::SCENE:
        return world.tag<AssetScene>();
    case ASSET_TYPE::SHADER:
        return world.tag<AssetShader>();
    default:
        return 0;
    }
}

u32 ASSET_ENTITY::type_of(World& world, const EntityId entity) {
    if (entity == 0 || !world.alive(entity)) {
        return 0;
    }
    for (const u32 type : KNOWN_TYPES) {
        if (world.has(entity, world.pair<AssetType>(ASSET_ENTITY::type_tag(world, type)))) {
            return type;
        }
    }
    return 0;
}

const char* ASSET_ENTITY::type_name(const u32 asset_type) {
    switch (asset_type) {
    case ASSET_TYPE::TEXTURE:
        return "texture";
    case ASSET_TYPE::MESH:
        return "mesh";
    case ASSET_TYPE::MATERIAL:
        return "material";
    case ASSET_TYPE::SCENE:
        return "scene";
    case ASSET_TYPE::SHADER:
        return "shader";
    default:
        return "unknown";
    }
}

EntityId ASSET_ENTITY::find(World& world, const AssetGuid& guid) {
    if (guid.is_null()) {
        return 0;
    }
    EntityId found = 0;
    world.query<AssetUuid>().each([&](const EntityId entity, const AssetUuid& asset) {
        if (found == 0 && asset.guid == guid) {
            found = entity;
        }
    });
    return found;
}

bool ASSET_ENTITY::name_from_path(const char* path, char* out, const usz capacity) {
    if (path == nullptr) {
        return TEXT_ASSET::copy_span("", 0, out, capacity);
    }
    const char* start = path;
    for (const char* c = path; *c != '\0'; ++c) {
        if (*c == '/' || *c == '\\') {
            start = c + 1;
        }
    }
    const char* end = start + strlen(start);
    for (const char* c = end; c > start; --c) {
        if (c[-1] == '.') {
            end = c - 1;
            break;
        }
    }
    return TEXT_ASSET::copy_span(start, static_cast<usz>(end - start), out, capacity);
}

// --- AssetEntities ----------------------------------------------------------

AssetEntities::AssetEntities() : AssetEntities(MEMORY::heap_allocator()) {}

AssetEntities::AssetEntities(BaseAllocator* allocator) : entities(allocator) {}

void AssetEntities::init(World* world) {
    this->world = world;
    ASSET_ENTITY::register_components(*world);
    this->removed_hook = world->hook_removed<AssetUuid>(&AssetEntities::on_removed, this);
}

EntityId AssetEntities::add(const AssetGuid& guid, const u32 asset_type, const char* name) {
    if (this->world == nullptr || guid.is_null()) {
        return 0;
    }
    EntityId entity = this->find(guid);
    if (entity == 0) {
        entity = this->world->new_entity();
        if (entity == 0) {
            return 0;
        }
        this->world->set<AssetUuid>(entity, AssetUuid{guid});
        this->entities.insert(guid, entity);
    }
    // AssetType is exclusive: adding the new pair replaces the old one. A
    // type without a tag drops whatever pair the entity held.
    const EntityId tag = ASSET_ENTITY::type_tag(*this->world, asset_type);
    if (tag != 0) {
        this->world->add(entity, this->world->pair<AssetType>(tag));
    } else {
        const u32 previous = ASSET_ENTITY::type_of(*this->world, entity);
        if (previous != 0) {
            this->world->remove(entity, this->world->pair<AssetType>(ASSET_ENTITY::type_tag(*this->world, previous)));
        }
    }
    SCENE::set_name(*this->world, entity, name != nullptr ? name : "");
    return entity;
}

EntityId AssetEntities::add(const AssetView& view, const char* path) {
    if (!view.is_ok()) {
        return 0;
    }
    char name[ENTITY_NAME_CAPACITY];
    ASSET_ENTITY::name_from_path(path, name, sizeof(name));
    return this->add(view.header->guid, view.header->type, name);
}

EntityId AssetEntities::find(const AssetGuid& guid) const {
    const EntityId* entity = this->entities.find(guid);
    if (entity == nullptr) {
        return 0;
    }
    // The removed hook keeps the index in step, but be safe about a World
    // freed or an entity deleted with hooks silenced.
    return this->world != nullptr && this->world->alive(*entity) ? *entity : 0;
}

bool AssetEntities::remove(const AssetGuid& guid) {
    const EntityId* found = this->entities.find(guid);
    if (found == nullptr) {
        return false;
    }
    const EntityId entity = *found;
    // delete_entity fires the removed hook, which drops the index entry;
    // remove it ourselves too in case the entity was already gone.
    this->entities.remove(guid);
    if (this->world != nullptr && this->world->alive(entity)) {
        this->world->delete_entity(entity);
    }
    return true;
}

void AssetEntities::free() {
    if (this->world != nullptr && this->removed_hook != 0) {
        this->world->unhook(this->world->id<AssetUuid>(), this->removed_hook);
    }
    this->removed_hook = 0;
    this->entities.free();
    this->world = nullptr;
}

void AssetEntities::on_removed(World* world, const EntityId entity, Id, void* user_data) {
    AssetEntities* self = static_cast<AssetEntities*>(user_data);
    const AssetUuid* asset = world->get<AssetUuid>(entity);
    if (asset == nullptr) {
        return;
    }
    const EntityId* indexed = self->entities.find(asset->guid);
    if (indexed != nullptr && *indexed == entity) {
        self->entities.remove(asset->guid);
    }
}
