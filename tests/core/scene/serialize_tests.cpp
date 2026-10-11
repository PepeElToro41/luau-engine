#include "support/test_support.hpp"

#include "engine/asset/asset_entity.hpp"
#include "engine/asset/text_asset.hpp"
#include "engine/scene/serialize.hpp"

#include <cstring>
#include <string>

// The Serializer component on component entities (expose / of / find), the
// field helpers and the entity reference rules.

namespace {

void position_write(SerializeContext&, const void* data, TextWriter& out) {
    const Position* position = static_cast<const Position*>(data);
    SERIALIZE::write_f32(out, "x", position->x);
    SERIALIZE::write_f32(out, "y", position->y);
}

bool position_read(SerializeContext&, void* data, const char* field, const usz field_length, const char* value, const usz value_length) {
    Position* position = static_cast<Position*>(data);
    if (TEXT_ASSET::word_is(field, field_length, "x")) {
        return SERIALIZE::read_f32(value, value_length, &position->x);
    }
    if (TEXT_ASSET::word_is(field, field_length, "y")) {
        return SERIALIZE::read_f32(value, value_length, &position->y);
    }
    return false;
}

void velocity_write(SerializeContext&, const void*, TextWriter&) {}
bool velocity_read(SerializeContext&, void*, const char*, usz, const char*, usz) { return false; }

std::string written(const DynamicArray<char>& out) {
    return std::string(out.data, out.count);
}

struct Stub {
    EntityId local_entity = 0;
    EntityId asset_entity = 0;
    AssetGuid asset;
    int warnings = 0;
    std::string last_warning;
};

EntityId stub_entity_of_local_id(const EntityIdLow id, void* scene_data) {
    return id == 7 ? static_cast<Stub*>(scene_data)->local_entity : 0;
}

EntityIdLow stub_local_id_of(const EntityId entity, void* scene_data) {
    return entity == static_cast<Stub*>(scene_data)->local_entity ? 7 : 0;
}

EntityId stub_resolve(const AssetGuid& guid, void* user_data) {
    const Stub* stub = static_cast<Stub*>(user_data);
    return guid == stub->asset ? stub->asset_entity : 0;
}

void stub_warn(const char* text, void* user_data) {
    Stub* stub = static_cast<Stub*>(user_data);
    stub->warnings += 1;
    stub->last_warning = text;
}

} // namespace

TEST_CASE("scene/serialize: expose stores a Serializer on the component entity") {
    World world;
    world.init();

    SERIALIZE::expose<Position>(world, "Position", &position_write, &position_read, 5);
    const Id position = world.id<Position>();
    const Serializer* serializer = SERIALIZE::of(world, position);
    REQUIRE(serializer != nullptr);
    CHECK(strcmp(serializer->name, "Position") == 0);
    CHECK(serializer->write == &position_write);
    CHECK(serializer->read == &position_read);
    CHECK(serializer->init == &SERIALIZE::default_init<Position>);
    CHECK(serializer->order == 5);
    CHECK_FALSE(serializer->is_tag());

    SUBCASE("exposing again replaces it") {
        SERIALIZE::expose<Position>(world, "Pos", &position_write, &position_read);
        const Serializer* again = SERIALIZE::of(world, position);
        REQUIRE(again != nullptr);
        CHECK(strcmp(again->name, "Pos") == 0);
        CHECK(again->order == 100);
        CHECK(world.query<Serializer>().count() == 1);
    }

    SUBCASE("a tag has no functions") {
        SERIALIZE::expose_tag<TagA>(world, "TagA");
        const Serializer* tag = SERIALIZE::of(world, world.id<TagA>());
        REQUIRE(tag != nullptr);
        CHECK(tag->is_tag());
        CHECK(tag->write == nullptr);
        CHECK(tag->read == nullptr);
        CHECK(tag->init == nullptr);
    }

    SUBCASE("default_init writes a value-initialized T") {
        Health health{42};
        SERIALIZE::default_init<Health>(&health);
        CHECK(health.value == 0);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("scene/serialize: of is nullptr for unexposed ids, pairs and 0; find looks up by name") {
    World world;
    world.init();

    SERIALIZE::expose<Position>(world, "Position", &position_write, &position_read);
    SERIALIZE::expose<Velocity>(world, "Velocity", &velocity_write, &velocity_read);
    SERIALIZE::expose_tag<TagA>(world, "TagA");
    CHECK(SERIALIZE::of(world, world.id<Health>()) == nullptr);
    CHECK(SERIALIZE::of(world, world.id<TagB>()) == nullptr);
    CHECK(SERIALIZE::of(world, 0) == nullptr);
    const EntityId parent = world.new_entity();
    CHECK(SERIALIZE::of(world, ECS::PAIR(ECS::CHILD_OF, parent)) == nullptr);

    CHECK(SERIALIZE::find(world, "Position", 8) == world.id<Position>());
    CHECK(SERIALIZE::find(world, "Velocity", 8) == world.id<Velocity>());
    CHECK(SERIALIZE::find(world, "TagA", 4) == world.id<TagA>());
    CHECK(SERIALIZE::find(world, "Positio", 7) == 0);
    CHECK(SERIALIZE::find(world, "Position.x", 10) == 0);
    CHECK(SERIALIZE::find(world, "Health", 6) == 0);
    CHECK(SERIALIZE::find(world, "", 0) == 0);
    CHECK(SERIALIZE::find(world, nullptr, 3) == 0);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("scene/serialize: TextWriter prefixes fields") {
    DynamicArray<char> out;
    TextWriter writer;
    writer.out = &out;
    writer.prefix = "Transform";
    SERIALIZE::write_f32(writer, "x", 1.5f);
    writer.prefix = nullptr;
    SERIALIZE::write_f32(writer, "y", 2.0f);
    CHECK(written(out) == "Transform.x = 1.5\ny = 2\n");
    out.free();
}

TEST_CASE("scene/serialize: scalar and vector helpers round-trip") {
    DynamicArray<char> out;
    TextWriter writer;
    writer.out = &out;

    SUBCASE("f32 with the shortest text that reads back exactly") {
        SERIALIZE::write_f32(writer, "a", 0.1f);
        CHECK(written(out) == "a = 0.1\n");
        f32 value = 0.0f;
        CHECK(SERIALIZE::read_f32("0.1", 3, &value));
        CHECK(value == 0.1f);
        CHECK_FALSE(SERIALIZE::read_f32("abc", 3, &value));
        CHECK_FALSE(SERIALIZE::read_f32("1 2", 3, &value));
        CHECK_FALSE(SERIALIZE::read_f32("", 0, &value));
        CHECK(value == 0.1f);
    }

    SUBCASE("i32 and u32") {
        SERIALIZE::write_i32(writer, "a", -7);
        SERIALIZE::write_u32(writer, "b", 4000000000u);
        CHECK(written(out) == "a = -7\nb = 4000000000\n");
        i32 i = 0;
        u32 u = 0;
        CHECK(SERIALIZE::read_i32("-7", 2, &i));
        CHECK(i == -7);
        CHECK_FALSE(SERIALIZE::read_i32("1.5", 3, &i));
        CHECK(SERIALIZE::read_u32("4000000000", 10, &u));
        CHECK(u == 4000000000u);
        CHECK_FALSE(SERIALIZE::read_u32("-1", 2, &u));
        CHECK(u == 4000000000u);
    }

    SUBCASE("bool") {
        SERIALIZE::write_bool(writer, "a", true);
        SERIALIZE::write_bool(writer, "b", false);
        CHECK(written(out) == "a = true\nb = false\n");
        bool value = false;
        CHECK(SERIALIZE::read_bool("true", 4, &value));
        CHECK(value);
        CHECK(SERIALIZE::read_bool("false", 5, &value));
        CHECK_FALSE(value);
        CHECK(SERIALIZE::read_bool("1", 1, &value));
        CHECK(value);
        CHECK_FALSE(SERIALIZE::read_bool("yes", 3, &value));
    }

    SUBCASE("vectors and quaternions") {
        SERIALIZE::write_vec2(writer, "a", Vector2(1.0f, -2.5f));
        SERIALIZE::write_vec3(writer, "b", Vector3(0.0f, 1.5f, 4.5f));
        SERIALIZE::write_vec4(writer, "c", Vector4(1.0f, 0.5f, 0.25f, 1.0f));
        SERIALIZE::write_quat(writer, "d", Quaternion(0.0f, 0.258819f, 0.0f, 0.9659258f));
        CHECK(written(out) == "a = 1 -2.5\nb = 0 1.5 4.5\nc = 1 0.5 0.25 1\nd = 0 0.258819 0 0.9659258\n");
        Vector2 v2;
        Vector3 v3;
        Vector4 v4;
        Quaternion q;
        CHECK(SERIALIZE::read_vec2("1 -2.5", 6, &v2));
        CHECK(v2.x == 1.0f);
        CHECK(v2.y == -2.5f);
        CHECK(SERIALIZE::read_vec3("0, 1.5, 4.5", 11, &v3));
        CHECK(v3.y == 1.5f);
        CHECK(v3.z == 4.5f);
        CHECK(SERIALIZE::read_vec4("1 0.5 0.25 1", 12, &v4));
        CHECK(v4.z == 0.25f);
        CHECK(SERIALIZE::read_quat("0 0.258819 0 0.9659258", 22, &q));
        CHECK(q.y() == 0.258819f);
        CHECK(q.w() == 0.9659258f);
        CHECK_FALSE(SERIALIZE::read_vec3("1 2", 3, &v3));
        CHECK_FALSE(SERIALIZE::read_vec3("1 2 3 4", 7, &v3));
        CHECK_FALSE(SERIALIZE::read_quat("1 2 x 4", 7, &q));
        CHECK(v3.y == 1.5f);
    }

    SUBCASE("guid and words") {
        const AssetGuid guid = {0x179a52dd06b33f96ull, 0xe26f6a98c55cbe22ull};
        SERIALIZE::write_guid(writer, "a", guid);
        SERIALIZE::write_guid(writer, "b", AssetGuid{});
        SERIALIZE::write_word(writer, "c", "cube");
        CHECK(written(out) == "a = e26f6a98c55cbe22179a52dd06b33f96\nb = none\nc = cube\n");
        AssetGuid read;
        CHECK(SERIALIZE::read_guid("e26f6a98c55cbe22179a52dd06b33f96", 32, &read));
        CHECK(read == guid);
        CHECK(SERIALIZE::read_guid("none", 4, &read));
        CHECK(read.is_null());
        CHECK_FALSE(SERIALIZE::read_guid("e26f", 4, &read));
    }

    out.free();
}

TEST_CASE("scene/serialize: entity references") {
    World world;
    world.init();
    AssetEntities assets;
    assets.init(&world);

    Stub stub;
    stub.local_entity = world.new_entity();
    stub.asset = {0x179a52dd06b33f96ull, 0xe26f6a98c55cbe22ull};
    stub.asset_entity = assets.add(stub.asset, ASSET_TYPE::MATERIAL, "demo");
    const EntityId stranger = world.new_entity();
    SCENE::set_name(world, stranger, "stranger");

    SerializeContext ctx;
    ctx.world = &world;
    ctx.local_id_of = &stub_local_id_of;
    ctx.entity_of_local_id = &stub_entity_of_local_id;
    ctx.scene_data = &stub;
    ctx.resolve_asset = &stub_resolve;
    ctx.user_data = &stub;
    ctx.warn = &stub_warn;
    ctx.path = "test.scene";
    ctx.line = 12;

    DynamicArray<char> out;
    TextWriter writer;
    writer.out = &out;
    writer.prefix = "Ref";

    SUBCASE("writing: local id, asset GUID, none, and a warning for the rest") {
        SERIALIZE::write_entity_ref(ctx, writer, "a", stub.local_entity);
        SERIALIZE::write_entity_ref(ctx, writer, "b", stub.asset_entity);
        SERIALIZE::write_entity_ref(ctx, writer, "c", 0);
        CHECK(written(out) == "Ref.a = 7\nRef.b = e26f6a98c55cbe22179a52dd06b33f96\nRef.c = none\n");
        CHECK(stub.warnings == 0);

        out.clear();
        SERIALIZE::write_entity_ref(ctx, writer, "d", stranger);
        CHECK(written(out) == "Ref.d = none\n");
        CHECK(stub.warnings == 1);
        CHECK(ctx.warnings == 1);
        CHECK(stub.last_warning.find("test.scene:12: Ref.d") == 0);
        CHECK(stub.last_warning.find("stranger") != std::string::npos);

        out.clear();
        world.delete_entity(stranger);
        SERIALIZE::write_entity_ref(ctx, writer, "e", stranger);
        CHECK(written(out) == "Ref.e = none\n");
        CHECK(stub.warnings == 1);
    }

    SUBCASE("reading: a number is a local id") {
        EntityId entity = 1;
        CHECK(SERIALIZE::read_entity_ref(ctx, "7", 1, &entity));
        CHECK(entity == stub.local_entity);
        CHECK(SERIALIZE::read_entity_ref(ctx, "8", 1, &entity));
        CHECK(entity == 0);
        CHECK(stub.warnings == 1);
        CHECK(stub.last_warning.find("no entity 8") != std::string::npos);
    }

    SUBCASE("reading: a GUID goes through the resolver") {
        EntityId entity = 1;
        CHECK(SERIALIZE::read_entity_ref(ctx, "e26f6a98c55cbe22179a52dd06b33f96", 32, &entity));
        CHECK(entity == stub.asset_entity);
        CHECK(SERIALIZE::read_entity_ref(ctx, "00000000000000000000000000000001", 32, &entity));
        CHECK(entity == 0);
        CHECK(stub.warnings == 1);
        CHECK(stub.last_warning.find("00000000000000000000000000000001") != std::string::npos);
    }

    SUBCASE("reading: without a resolver the asset entity is found by scan") {
        ctx.resolve_asset = nullptr;
        EntityId entity = 1;
        CHECK(SERIALIZE::read_entity_ref(ctx, "e26f6a98c55cbe22179a52dd06b33f96", 32, &entity));
        CHECK(entity == stub.asset_entity);
    }

    SUBCASE("reading: none and garbage") {
        EntityId entity = 1;
        CHECK(SERIALIZE::read_entity_ref(ctx, "none", 4, &entity));
        CHECK(entity == 0);
        entity = 1;
        CHECK_FALSE(SERIALIZE::read_entity_ref(ctx, "cube", 4, &entity));
        CHECK_FALSE(SERIALIZE::read_entity_ref(ctx, "", 0, &entity));
        CHECK(entity == 1);
        CHECK(stub.warnings == 0);
    }

    out.free();
    assets.free();
    world.free();
    CHECK_ARENA_CLEAN();
}
