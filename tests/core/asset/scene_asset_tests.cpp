#include "support/test_support.hpp"

#include "engine/asset/asset_entity.hpp"
#include "engine/asset/asset_types/scene_asset.hpp"
#include "engine/asset/text_asset.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/serialize.hpp"

#include <cstring>
#include <string>
#include <vector>

// SCENE_ASSET::write / read over the sample components: the canonical
// layout, round trips, forward references, warnings and prefabs.

namespace {

// An entity reference field.
struct Ref {
    EntityId target = 0;
};

// A component whose default is not zero, to see init at work.
struct Spin {
    f32 rate = 1.0f;
};

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

void health_write(SerializeContext&, const void* data, TextWriter& out) {
    SERIALIZE::write_i32(out, "value", static_cast<const Health*>(data)->value);
}

bool health_read(SerializeContext&, void* data, const char* field, const usz field_length, const char* value, const usz value_length) {
    if (TEXT_ASSET::word_is(field, field_length, "value")) {
        return SERIALIZE::read_i32(value, value_length, &static_cast<Health*>(data)->value);
    }
    return false;
}

void ref_write(SerializeContext& ctx, const void* data, TextWriter& out) {
    SERIALIZE::write_entity_ref(ctx, out, "target", static_cast<const Ref*>(data)->target);
}

bool ref_read(SerializeContext& ctx, void* data, const char* field, const usz field_length, const char* value, const usz value_length) {
    if (TEXT_ASSET::word_is(field, field_length, "target")) {
        return SERIALIZE::read_entity_ref(ctx, value, value_length, &static_cast<Ref*>(data)->target);
    }
    return false;
}

void spin_write(SerializeContext&, const void* data, TextWriter& out) {
    SERIALIZE::write_f32(out, "rate", static_cast<const Spin*>(data)->rate);
}

bool spin_read(SerializeContext&, void* data, const char* field, const usz field_length, const char* value, const usz value_length) {
    if (TEXT_ASSET::word_is(field, field_length, "rate")) {
        return SERIALIZE::read_f32(value, value_length, &static_cast<Spin*>(data)->rate);
    }
    return false;
}

void expose_all(World& world) {
    SERIALIZE::expose<Position>(world, "Position", &position_write, &position_read, 0);
    SERIALIZE::expose<Health>(world, "Health", &health_write, &health_read, 20);
    SERIALIZE::expose<Ref>(world, "Ref", &ref_write, &ref_read, 10);
    SERIALIZE::expose<Spin>(world, "Spin", &spin_write, &spin_read, 30);
    SERIALIZE::expose_tag<TagA>(world, "TagA");
    SERIALIZE::expose_tag<TagB>(world, "TagB");
}

struct Log {
    std::vector<std::string> warnings;
    static void warn(const char* text, void* user_data) { static_cast<Log*>(user_data)->warnings.push_back(text); }
    bool contains(const char* needle) const {
        for (const std::string& warning : this->warnings) {
            if (warning.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

const AssetGuid GUID = {0x179a52dd06b33f96ull, 0xe26f6a98c55cbe22ull};
constexpr const char* GUID_TEXT = "e26f6a98c55cbe22179a52dd06b33f96";

std::string write_scene(World& world, const EntityId root, SerializeContext& ctx) {
    DynamicArray<char> out;
    SCENE_ASSET::write(world, root, GUID, ctx, out);
    std::string text(out.data, out.count);
    out.free();
    return text;
}

SCENE_ASSET::SceneReadResult read_scene(World& world, const std::string& text, const EntityId parent, SerializeContext& ctx) {
    return SCENE_ASSET::read(world, text.data(), text.size(), parent, ctx);
}

// The children of `parent` by name, in SCENE::children order.
std::vector<std::string> child_names(World& world, const EntityId parent) {
    DynamicArray<EntityId> children;
    SCENE::children(world, parent, children);
    std::vector<std::string> names;
    for (usz i = 0; i < children.count; ++i) {
        const char* name = SCENE::name(world, children[i]);
        names.push_back(name != nullptr ? name : "");
    }
    children.free();
    return names;
}

EntityId child_named(World& world, const EntityId parent, const char* name) {
    DynamicArray<EntityId> children;
    SCENE::children(world, parent, children);
    EntityId found = 0;
    for (usz i = 0; i < children.count; ++i) {
        const char* child_name = SCENE::name(world, children[i]);
        if (child_name != nullptr && strcmp(child_name, name) == 0) {
            found = children[i];
        }
    }
    children.free();
    return found;
}

} // namespace

TEST_CASE("asset/scene_asset: a root without children writes just the guid") {
    World world;
    world.init();
    expose_all(world);
    const EntityId root = SCENE::spawn(world, "scene_root", 0);
    SerializeContext ctx;
    CHECK(write_scene(world, root, ctx) == std::string("guid = ") + GUID_TEXT + "\n");
    CHECK(write_scene(world, 0, ctx) == std::string("guid = ") + GUID_TEXT + "\n");
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("asset/scene_asset: a tree writes the canonical layout and round-trips byte for byte") {
    World world;
    world.init();
    expose_all(world);
    Log log;
    SerializeContext ctx;
    ctx.warn = &Log::warn;
    ctx.user_data = &log;

    // root -> a (Position, Health, TagA) -> b (Ref -> a), a -> c (Spin)
    //      -> d (Ref -> a stranger, written as none)
    const EntityId root = SCENE::spawn(world, "scene_root", 0);
    const EntityId a = SCENE::spawn(world, "a", root);
    world.set(a, Position{1.0f, 2.5f});
    world.set(a, Health{5});
    world.add<TagA>(a);
    const EntityId b = SCENE::spawn(world, "b", a);
    world.set(b, Ref{a});
    const EntityId c = SCENE::spawn(world, "c", a);
    world.set(c, Spin{0.5f});
    const EntityId d = SCENE::spawn(world, "d", root);
    const EntityId stranger = world.new_entity();
    world.set(d, Ref{stranger});
    world.add<TagB>(d);
    world.add<TagA>(d);

    const std::string expected = std::string("guid = ") + GUID_TEXT + "\n"
        "\n[entity 1]\nname = a\nPosition.x = 1\nPosition.y = 2.5\nHealth.value = 5\ntags = TagA\n"
        "\n[entity 2]\nname = b\nparent = 1\nRef.target = 1\n"
        "\n[entity 3]\nname = c\nparent = 1\nSpin.rate = 0.5\n"
        "\n[entity 4]\nname = d\nRef.target = none\ntags = TagA TagB\n";
    const std::string text = write_scene(world, root, ctx);
    CHECK(text == expected);
    CHECK(log.warnings.size() == 1);
    CHECK(log.contains("Ref.target of 'd'"));
    CHECK(ctx.warnings == 1);
    // The writer leaves no callbacks behind.
    CHECK(ctx.local_id_of == nullptr);
    CHECK(ctx.scene_data == nullptr);

    SUBCASE("read back under a fresh root, then written again") {
        log.warnings.clear();
        const EntityId other = SCENE::spawn(world, "other_root", 0);
        const SCENE_ASSET::SceneReadResult result = read_scene(world, text, other, ctx);
        CHECK(result.ok);
        CHECK(result.entities == 4);
        CHECK(result.warnings == 0);
        CHECK(result.guid == GUID);
        CHECK(log.warnings.empty());
        CHECK(child_names(world, other) == std::vector<std::string>{"a", "d"});
        const EntityId a2 = child_named(world, other, "a");
        const EntityId d2 = child_named(world, other, "d");
        REQUIRE(a2 != 0);
        REQUIRE(d2 != 0);
        CHECK(a2 != a);
        CHECK(child_names(world, a2) == std::vector<std::string>{"b", "c"});
        const EntityId b2 = child_named(world, a2, "b");
        const EntityId c2 = child_named(world, a2, "c");
        REQUIRE(world.get<Position>(a2) != nullptr);
        CHECK(world.get<Position>(a2)->x == 1.0f);
        CHECK(world.get<Position>(a2)->y == 2.5f);
        REQUIRE(world.get<Health>(a2) != nullptr);
        CHECK(world.get<Health>(a2)->value == 5);
        CHECK(world.has<TagA>(a2));
        CHECK_FALSE(world.has<TagB>(a2));
        REQUIRE(world.get<Ref>(b2) != nullptr);
        CHECK(world.get<Ref>(b2)->target == a2);
        REQUIRE(world.get<Spin>(c2) != nullptr);
        CHECK(world.get<Spin>(c2)->rate == 0.5f);
        REQUIRE(world.get<Ref>(d2) != nullptr);
        CHECK(world.get<Ref>(d2)->target == 0);
        CHECK(world.has<TagA>(d2));
        CHECK(world.has<TagB>(d2));
        // The original tree is untouched.
        CHECK(child_names(world, root) == std::vector<std::string>{"a", "d"});

        const std::string again = write_scene(world, other, ctx);
        CHECK(again == expected);
        CHECK(log.warnings.empty());
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("asset/scene_asset: a parent may be defined after its child, and references resolve forward") {
    World world;
    world.init();
    expose_all(world);
    Log log;
    SerializeContext ctx;
    ctx.warn = &Log::warn;
    ctx.user_data = &log;
    ctx.path = "forward.scene";
    const EntityId root = SCENE::spawn(world, "scene_root", 0);

    const std::string text = std::string("guid = ") + GUID_TEXT + "\n"
        "[entity 20]\nname = child\nparent = 5\nRef.target = 9\n"
        "[entity 5]\nname = parent\n"
        "[entity 9]\nname = sibling\nparent = 5\n";
    const SCENE_ASSET::SceneReadResult result = read_scene(world, text, root, ctx);
    CHECK(result.ok);
    CHECK(result.entities == 3);
    CHECK(result.warnings == 0);
    CHECK(child_names(world, root) == std::vector<std::string>{"parent"});
    const EntityId parent = child_named(world, root, "parent");
    // Created in file order, so the child comes first.
    CHECK(child_names(world, parent) == std::vector<std::string>{"child", "sibling"});
    const EntityId child = child_named(world, parent, "child");
    const EntityId sibling = child_named(world, parent, "sibling");
    REQUIRE(world.get<Ref>(child) != nullptr);
    CHECK(world.get<Ref>(child)->target == sibling);
    // File ids are never entity ids.
    CHECK(ECS::ENTITY_LOW(child) != 20);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("asset/scene_asset: line problems warn and skip, the rest loads") {
    World world;
    world.init();
    expose_all(world);
    Log log;
    SerializeContext ctx;
    ctx.warn = &Log::warn;
    ctx.user_data = &log;
    ctx.path = "messy.scene";
    const EntityId root = SCENE::spawn(world, "scene_root", 0);

    const std::string text = std::string("guid = ") + GUID_TEXT + "\n"
        "author = me\n"
        "[entity 1]\n"
        "name = thing\n"
        "Position.x = 3\n"          // y keeps its default
        "Position.z = 1\n"          // unknown field
        "Health.value = lots\n"     // bad value
        "Armor.value = 1\n"         // unknown component
        "Spin.other = 2\n"          // unknown field, but Spin is added with its default
        "TagA.x = 1\n"              // a tag has no fields
        "color = red\n"             // no dot
        "parent = x\n"              // bad parent
        "tags = TagB Nope Health\n" // one unknown, one not a tag
        "[embedded 1]\n"
        "data = 1\n"
        "[entity 2]\n"
        "name = loner\n"
        "parent = 1\n";
    const SCENE_ASSET::SceneReadResult result = read_scene(world, text, root, ctx);
    CHECK(result.ok);
    CHECK(result.entities == 2);
    CHECK(result.warnings == 11);
    CHECK(log.warnings.size() == 11);
    CHECK(log.contains("messy.scene:2: unknown top-level key 'author'"));
    CHECK(log.contains("messy.scene:6: Position.z: unknown field"));
    CHECK(log.contains("messy.scene:7: Health.value: unknown field or bad value 'lots'"));
    CHECK(log.contains("messy.scene:8: unknown component 'Armor'"));
    CHECK(log.contains("messy.scene:9: Spin.other"));
    CHECK(log.contains("messy.scene:10: 'TagA' is a tag"));
    CHECK(log.contains("messy.scene:11: unknown key 'color'"));
    CHECK(log.contains("messy.scene:12: bad parent 'x'"));
    CHECK(log.contains("messy.scene:13: unknown tag 'Nope'"));
    CHECK(log.contains("messy.scene:13: 'Health' is a component, not a tag"));
    CHECK(log.contains("messy.scene:15: unknown section [embedded 1]"));

    CHECK(child_names(world, root) == std::vector<std::string>{"thing"});
    const EntityId thing = child_named(world, root, "thing");
    REQUIRE(world.get<Position>(thing) != nullptr);
    CHECK(world.get<Position>(thing)->x == 3.0f);
    CHECK(world.get<Position>(thing)->y == 0.0f);
    REQUIRE(world.get<Health>(thing) != nullptr);
    CHECK(world.get<Health>(thing)->value == 0);
    REQUIRE(world.get<Spin>(thing) != nullptr);
    CHECK(world.get<Spin>(thing)->rate == 1.0f);
    CHECK(world.has<TagB>(thing));
    CHECK_FALSE(world.has<TagA>(thing));
    CHECK(child_names(world, thing) == std::vector<std::string>{"loner"});

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("asset/scene_asset: structural failures create nothing") {
    World world;
    world.init();
    expose_all(world);
    SerializeContext ctx;
    const EntityId root = SCENE::spawn(world, "scene_root", 0);
    const usz before = world.query<Name>().count();

    SUBCASE("no guid") {
        const SCENE_ASSET::SceneReadResult result = read_scene(world, "[entity 1]\nname = a\n", root, ctx);
        CHECK_FALSE(result.ok);
        CHECK(result.line == 0);
        CHECK(strcmp(result.error, "no top-level guid") == 0);
    }
    SUBCASE("malformed guid") {
        const SCENE_ASSET::SceneReadResult result = read_scene(world, "guid = 12\n[entity 1]\nname = a\n", root, ctx);
        CHECK_FALSE(result.ok);
        CHECK(result.line == 1);
    }
    SUBCASE("duplicate entity id") {
        const std::string text = std::string("guid = ") + GUID_TEXT + "\n[entity 1]\nname = a\n[entity 1]\nname = b\n";
        const SCENE_ASSET::SceneReadResult result = read_scene(world, text, root, ctx);
        CHECK_FALSE(result.ok);
        CHECK(result.line == 5);
        CHECK(strcmp(result.error, "duplicate [entity N]") == 0);
    }
    SUBCASE("malformed line") {
        const std::string text = std::string("guid = ") + GUID_TEXT + "\n[entity 1]\nname = a\nwhat\n";
        const SCENE_ASSET::SceneReadResult result = read_scene(world, text, root, ctx);
        CHECK_FALSE(result.ok);
        CHECK(result.line == 4);
    }
    SUBCASE("dead parent") {
        const EntityId dead = world.new_entity();
        world.delete_entity(dead);
        const std::string text = std::string("guid = ") + GUID_TEXT + "\n[entity 1]\nname = a\n";
        CHECK_FALSE(read_scene(world, text, dead, ctx).ok);
    }

    CHECK(world.query<Name>().count() == before);
    CHECK_FALSE(SCENE::has_children(world, root));
    world.free();
    CHECK_ARENA_CLEAN();
}

namespace {

struct Resolver {
    AssetGuid guid;
    EntityId entity = 0;
    int calls = 0;
    static EntityId resolve(const AssetGuid& guid, void* user_data) {
        Resolver* resolver = static_cast<Resolver*>(user_data);
        resolver->calls += 1;
        return guid == resolver->guid ? resolver->entity : 0;
    }
};

} // namespace

TEST_CASE("asset/scene_asset: asset references go through the resolver and come back as GUIDs") {
    World world;
    world.init();
    expose_all(world);
    AssetEntities assets;
    assets.init(&world);
    const AssetGuid material_guid = {0x1ull, 0x2ull};
    Resolver resolver;
    resolver.guid = material_guid;
    resolver.entity = assets.add(material_guid, ASSET_TYPE::MATERIAL, "demo");
    Log log;
    SerializeContext ctx;
    ctx.resolve_asset = &Resolver::resolve;
    ctx.user_data = &resolver;
    ctx.warn = &Log::warn;
    const EntityId root = SCENE::spawn(world, "scene_root", 0);

    const std::string text = std::string("guid = ") + GUID_TEXT + "\n"
        "[entity 1]\nname = a\nRef.target = 00000000000000020000000000000001\n"
        "[entity 2]\nname = b\nRef.target = 00000000000000000000000000000009\n";
    // The warning sink gets the resolver's user_data too; share the log
    // through it by resolving first, then checking the count.
    ctx.warn = nullptr;
    const SCENE_ASSET::SceneReadResult result = read_scene(world, text, root, ctx);
    CHECK(result.ok);
    CHECK(result.warnings == 1);
    CHECK(resolver.calls == 2);
    const EntityId a = child_named(world, root, "a");
    const EntityId b = child_named(world, root, "b");
    REQUIRE(world.get<Ref>(a) != nullptr);
    CHECK(world.get<Ref>(a)->target == resolver.entity);
    REQUIRE(world.get<Ref>(b) != nullptr);
    CHECK(world.get<Ref>(b)->target == 0);

    // Written back: the asset entity is a GUID again, the empty one none.
    const std::string again = write_scene(world, root, ctx);
    CHECK(again == std::string("guid = ") + GUID_TEXT + "\n"
        "\n[entity 1]\nname = a\nRef.target = 00000000000000020000000000000001\n"
        "\n[entity 2]\nname = b\nRef.target = none\n");

    assets.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("asset/scene_asset: reading under another entity is a prefab, and a self parent falls back") {
    World world;
    world.init();
    expose_all(world);
    Log log;
    SerializeContext ctx;
    ctx.warn = &Log::warn;
    ctx.user_data = &log;
    const EntityId root = SCENE::spawn(world, "scene_root", 0);
    const EntityId holder = SCENE::spawn(world, "holder", root);

    const std::string text = std::string("guid = ") + GUID_TEXT + "\n"
        "[entity 1]\nname = wheel\n"
        "[entity 2]\nname = axle\nparent = 2\n"
        "[entity 3]\nname = nut\nparent = 1\n";
    const SCENE_ASSET::SceneReadResult first = read_scene(world, text, holder, ctx);
    CHECK(first.ok);
    CHECK(first.entities == 3);
    CHECK(first.warnings == 1);
    CHECK(log.contains("parent 2 is not an entity of this file"));
    CHECK(child_names(world, root) == std::vector<std::string>{"holder"});
    CHECK(child_names(world, holder) == std::vector<std::string>{"wheel", "axle"});
    CHECK(child_names(world, child_named(world, holder, "wheel")) == std::vector<std::string>{"nut"});

    // Instantiated twice: two independent copies.
    const SCENE_ASSET::SceneReadResult second = read_scene(world, text, holder, ctx);
    CHECK(second.ok);
    CHECK(child_names(world, holder) == std::vector<std::string>{"wheel", "axle", "wheel", "axle"});

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("asset/scene_asset: .scene is a text asset type") {
    CHECK(TEXT_ASSET::type_of_path("scenes/demo.scene") == ASSET_TYPE::SCENE);
    CHECK(TEXT_ASSET::type_of_path("DEMO.SCENE") == ASSET_TYPE::SCENE);
    CHECK(TEXT_ASSET::type_of_path("demo.material") == ASSET_TYPE::MATERIAL);
    CHECK(TEXT_ASSET::type_of_path("demo.scenes") == 0);
    CHECK(strcmp(TEXT_ASSET::extension_of_type(ASSET_TYPE::SCENE), ".scene") == 0);
    CHECK(strcmp(SCENE_ASSET::EXTENSION, TEXT_ASSET::SCENE_EXTENSION) == 0);
    AssetGuid guid;
    const std::string text = std::string("guid = ") + GUID_TEXT + "\n[entity 1]\nname = a\n";
    CHECK(TEXT_ASSET::find_guid(text.data(), text.size(), &guid));
    CHECK(guid == GUID);
}

TEST_CASE("scene/scene: unload deletes the root's subtree and keeps the root") {
    World world;
    world.init();
    Scene scene;
    scene.init(&world);
    const EntityId a = scene.spawn("a");
    const EntityId b = scene.spawn("b", a);
    const EntityId outside = SCENE::spawn(world, "outside", 0);
    scene.set_current(GUID, "scenes/demo.scene");
    CHECK(scene.has_current());
    CHECK(strcmp(scene.current_path, "scenes/demo.scene") == 0);

    scene.unload();
    CHECK(world.alive(scene.root));
    CHECK_FALSE(world.alive(a));
    CHECK_FALSE(world.alive(b));
    CHECK(world.alive(outside));
    CHECK_FALSE(SCENE::has_children(world, scene.root));
    CHECK(scene.has_current());

    scene.set_current(AssetGuid{}, nullptr);
    CHECK_FALSE(scene.has_current());
    CHECK(scene.current_path[0] == '\0');

    world.free();
    CHECK_ARENA_CLEAN();
}
