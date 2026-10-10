#include "support/test_support.hpp"

#include "engine/asset/asset_entity.hpp"
#include "engine/asset/asset_writer.hpp"
#include "engine/ecs/query/query.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/scene/scene.hpp"

#include <cstring>

// Assets as entities: the AssetUuid component, the (AssetType, tag) pair
// and the AssetEntities index from GUID to entity.

namespace {

AssetGuid guid_of(const u64 lo, const u64 hi) {
    AssetGuid guid;
    guid.lo = lo;
    guid.hi = hi;
    return guid;
}

} // namespace

TEST_CASE("asset/asset_entity: name_from_path keeps the stem only") {
    char name[ENTITY_NAME_CAPACITY];
    CHECK(ASSET_ENTITY::name_from_path("textures/rock.lunaasset", name, sizeof(name)));
    CHECK(strcmp(name, "rock") == 0);
    CHECK(ASSET_ENTITY::name_from_path("C:\\project\\materials\\demo.material", name, sizeof(name)));
    CHECK(strcmp(name, "demo") == 0);
    CHECK(ASSET_ENTITY::name_from_path("noext", name, sizeof(name)));
    CHECK(strcmp(name, "noext") == 0);
    CHECK(ASSET_ENTITY::name_from_path("dir.v2/a.b.c", name, sizeof(name)));
    CHECK(strcmp(name, "a.b") == 0);
    CHECK(ASSET_ENTITY::name_from_path(nullptr, name, sizeof(name)));
    CHECK(name[0] == '\0');

    char small[4];
    CHECK_FALSE(ASSET_ENTITY::name_from_path("longname.png", small, sizeof(small)));
    CHECK(strcmp(small, "lon") == 0);
}

TEST_CASE("asset/asset_entity: type_tag, type_of and type_name cover every ASSET_TYPE") {
    World world;
    world.init();
    ASSET_ENTITY::register_components(world);

    const u32 types[] = {ASSET_TYPE::TEXTURE, ASSET_TYPE::MESH, ASSET_TYPE::MATERIAL, ASSET_TYPE::SCENE, ASSET_TYPE::SHADER};
    for (const u32 type : types) {
        const EntityId tag = ASSET_ENTITY::type_tag(world, type);
        REQUIRE(tag != 0);
        CHECK(strcmp(ASSET_ENTITY::type_name(type), "unknown") != 0);
        const EntityId entity = world.new_entity();
        world.add(entity, world.pair<AssetType>(tag));
        CHECK(ASSET_ENTITY::type_of(world, entity) == type);
    }
    CHECK(ASSET_ENTITY::type_tag(world, ASSET_TYPE::TEXTURE) == world.tag<AssetTexture>());
    CHECK(ASSET_ENTITY::type_tag(world, ASSET_TYPE::MESH) == world.tag<AssetMesh>());
    CHECK(ASSET_ENTITY::type_tag(world, ASSET_TYPE::MATERIAL) == world.tag<AssetMaterial>());
    CHECK(ASSET_ENTITY::type_tag(world, 0x12345678u) == 0);
    CHECK(strcmp(ASSET_ENTITY::type_name(0x12345678u), "unknown") == 0);
    CHECK(strcmp(ASSET_ENTITY::type_name(ASSET_TYPE::TEXTURE), "texture") == 0);

    const EntityId untyped = world.new_entity();
    CHECK(ASSET_ENTITY::type_of(world, untyped) == 0);
    CHECK(ASSET_ENTITY::type_of(world, 0) == 0);

    // The component entities are named for the editor.
    CHECK(strcmp(SCENE::name(world, world.id<AssetUuid>()), "AssetUuid") == 0);
    CHECK(strcmp(SCENE::name(world, world.tag<AssetType>()), "AssetType") == 0);
    CHECK(world.has(world.tag<AssetType>(), ECS::EXCLUSIVE));

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("asset/asset_entities: add creates one entity per GUID with uuid, type pair and name") {
    World world;
    world.init();
    AssetEntities assets;
    assets.init(&world);

    const AssetGuid rock = guid_of(1, 2);
    const EntityId entity = assets.add(rock, ASSET_TYPE::TEXTURE, "rock");
    REQUIRE(entity != 0);
    CHECK(assets.count() == 1);
    CHECK(assets.find(rock) == entity);
    CHECK(assets.contains(rock));
    CHECK(ASSET_ENTITY::find(world, rock) == entity);
    REQUIRE(world.get<AssetUuid>(entity) != nullptr);
    CHECK(world.get<AssetUuid>(entity)->guid == rock);
    CHECK(world.has<AssetType, AssetTexture>(entity));
    CHECK(ASSET_ENTITY::type_of(world, entity) == ASSET_TYPE::TEXTURE);
    CHECK(strcmp(SCENE::name(world, entity), "rock") == 0);
    // Not scene content: no parent.
    CHECK(SCENE::parent(world, entity) == 0);

    SUBCASE("adding the same GUID again returns the same entity and updates type and name") {
        const EntityId again = assets.add(rock, ASSET_TYPE::MESH, "rock_mesh");
        CHECK(again == entity);
        CHECK(assets.count() == 1);
        CHECK(world.has<AssetType, AssetMesh>(entity));
        CHECK_FALSE(world.has<AssetType, AssetTexture>(entity));
        CHECK(strcmp(SCENE::name(world, entity), "rock_mesh") == 0);
    }

    SUBCASE("an unknown type leaves no type pair") {
        const EntityId again = assets.add(rock, 0x12345678u, "rock");
        CHECK(again == entity);
        CHECK(ASSET_ENTITY::type_of(world, entity) == 0);
        CHECK_FALSE(world.has<AssetType, AssetTexture>(entity));
    }

    SUBCASE("a null GUID is refused") {
        CHECK(assets.add(AssetGuid{}, ASSET_TYPE::TEXTURE, "nothing") == 0);
        CHECK(assets.find(AssetGuid{}) == 0);
        CHECK(ASSET_ENTITY::find(world, AssetGuid{}) == 0);
        CHECK(assets.count() == 1);
    }

    SUBCASE("unknown GUIDs are not found") {
        CHECK(assets.find(guid_of(9, 9)) == 0);
        CHECK(ASSET_ENTITY::find(world, guid_of(9, 9)) == 0);
        CHECK_FALSE(assets.remove(guid_of(9, 9)));
    }

    assets.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("asset/asset_entities: queries select asset entities by type pair") {
    World world;
    world.init();
    AssetEntities assets;
    assets.init(&world);

    const EntityId tex_a = assets.add(guid_of(1, 1), ASSET_TYPE::TEXTURE, "a");
    const EntityId tex_b = assets.add(guid_of(2, 2), ASSET_TYPE::TEXTURE, "b");
    const EntityId mesh = assets.add(guid_of(3, 3), ASSET_TYPE::MESH, "m");
    const EntityId material = assets.add(guid_of(4, 4), ASSET_TYPE::MATERIAL, "mat");
    REQUIRE(tex_a != 0);
    REQUIRE(tex_b != 0);
    REQUIRE(mesh != 0);
    REQUIRE(material != 0);

    usz textures = 0;
    bool saw_a = false;
    bool saw_b = false;
    world.query<AssetUuid>().with<ECS::Pair<AssetType, AssetTexture>>().each([&](const EntityId entity, AssetUuid& asset) {
        textures += 1;
        saw_a |= entity == tex_a && asset.guid == guid_of(1, 1);
        saw_b |= entity == tex_b && asset.guid == guid_of(2, 2);
    });
    CHECK(textures == 2);
    CHECK(saw_a);
    CHECK(saw_b);

    CHECK(world.query<AssetUuid>().with<ECS::Pair<AssetType, AssetMesh>>().count() == 1);
    CHECK(world.query<AssetUuid>().with<ECS::Pair<AssetType, AssetMaterial>>().count() == 1);
    CHECK(world.query<AssetUuid>().with(world.pair<AssetType>(world.tag<AssetTexture>())).count() == 2);
    CHECK(world.query<AssetUuid>().count() == 4);
    // Every asset entity has exactly one type: the wildcard pair matches all.
    CHECK(world.query<AssetUuid>().with(ECS::PAIR(world.tag<AssetType>(), ECS::WILDCARD)).count() == 4);

    assets.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("asset/asset_entities: remove deletes the entity and deleting the entity drops the index entry") {
    World world;
    world.init();
    AssetEntities assets;
    assets.init(&world);

    const AssetGuid a = guid_of(1, 1);
    const AssetGuid b = guid_of(2, 2);
    const EntityId entity_a = assets.add(a, ASSET_TYPE::TEXTURE, "a");
    const EntityId entity_b = assets.add(b, ASSET_TYPE::TEXTURE, "b");
    REQUIRE(entity_a != 0);
    REQUIRE(entity_b != 0);

    SUBCASE("remove") {
        CHECK(assets.remove(a));
        CHECK_FALSE(world.alive(entity_a));
        CHECK(assets.find(a) == 0);
        CHECK(assets.count() == 1);
        CHECK(assets.find(b) == entity_b);
        CHECK_FALSE(assets.remove(a));
    }

    SUBCASE("delete_entity") {
        world.delete_entity(entity_b);
        CHECK(assets.find(b) == 0);
        CHECK(assets.count() == 1);
        CHECK(assets.find(a) == entity_a);
        // A later add makes a fresh entity.
        const EntityId again = assets.add(b, ASSET_TYPE::TEXTURE, "b");
        CHECK(again != 0);
        CHECK(again != entity_b);
        CHECK(assets.find(b) == again);
    }

    SUBCASE("removing the component alone drops the entry too") {
        world.remove<AssetUuid>(entity_a);
        CHECK(world.alive(entity_a));
        CHECK(assets.find(a) == 0);
        CHECK(assets.count() == 1);
    }

    SUBCASE("free leaves the entities in the world and stops tracking them") {
        assets.free();
        CHECK(world.alive(entity_a));
        CHECK(world.alive(entity_b));
        CHECK(assets.count() == 0);
        CHECK(assets.find(a) == 0);
        world.delete_entity(entity_a); // no hook left to fire
    }

    assets.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("asset/asset_entities: add from a view takes guid and type from the header and the name from the path") {
    World world;
    world.init();
    AssetEntities assets;
    assets.init(&world);

    AssetWriter writer;
    writer.type = ASSET_TYPE::MESH;
    writer.guid = guid_of(7, 8);
    const u8 payload[16] = {};
    writer.add_chunk(CHUNK_TYPE::MESH, 1, 0, payload, sizeof(payload));
    usz size = 0;
    u8* bytes = writer.write(MEMORY::heap_allocator(), &size);
    REQUIRE(bytes != nullptr);
    const AssetView view = AssetView::parse(bytes, size);
    REQUIRE(view.is_ok());

    const EntityId entity = assets.add(view, "meshes/cube.lunaasset");
    REQUIRE(entity != 0);
    CHECK(world.get<AssetUuid>(entity)->guid == guid_of(7, 8));
    CHECK(world.has<AssetType, AssetMesh>(entity));
    CHECK(strcmp(SCENE::name(world, entity), "cube") == 0);

    AssetView bad;
    CHECK(assets.add(bad, "x") == 0);

    MEMORY::heap_allocator()->free(bytes);
    writer.free();
    assets.free();
    world.free();
    CHECK_ARENA_CLEAN();
}
