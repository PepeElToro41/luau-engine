#include "support/test_support.hpp"

#include "engine/scene/scene.hpp"

#include <cstring>

// The scene hierarchy helpers (SCENE::) over CHILD_OF + Name, and the Scene
// object with its "scene_root" entity.

TEST_CASE("scene/name: make copies the text and truncates to the capacity") {
    const Name empty = Name::make(nullptr);
    CHECK(empty.value[0] == '\0');

    const Name short_name = Name::make("cube");
    CHECK(strcmp(short_name.value, "cube") == 0);

    char long_text[ENTITY_NAME_CAPACITY + 16];
    memset(long_text, 'x', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    const Name long_name = Name::make(long_text);
    CHECK(strlen(long_name.value) == ENTITY_NAME_CAPACITY - 1);
    CHECK(long_name.value[ENTITY_NAME_CAPACITY - 1] == '\0');
}

TEST_CASE("scene/scene: spawn names the entity and parents it") {
    World world;
    world.init();

    const EntityId parent = SCENE::spawn(world, "parent", 0);
    REQUIRE(parent != 0);
    CHECK(SCENE::parent(world, parent) == 0);
    CHECK(strcmp(SCENE::name(world, parent), "parent") == 0);

    const EntityId child = SCENE::spawn(world, "child", parent);
    REQUIRE(child != 0);
    CHECK(world.has(child, ECS::PAIR(ECS::CHILD_OF, parent)));
    CHECK(SCENE::parent(world, child) == parent);
    CHECK(strcmp(SCENE::name(world, child), "child") == 0);
    CHECK(world.depth(child) == 1);

    SUBCASE("a dead parent leaves the entity unparented") {
        world.delete_entity(child);
        const EntityId orphan = SCENE::spawn(world, "orphan", child);
        REQUIRE(orphan != 0);
        CHECK(SCENE::parent(world, orphan) == 0);
    }

    SUBCASE("name and parent of a dead or unnamed entity") {
        const EntityId plain = world.new_entity();
        CHECK(SCENE::name(world, plain) == nullptr);
        CHECK(SCENE::parent(world, plain) == 0);
        world.delete_entity(child);
        CHECK(SCENE::name(world, child) == nullptr);
        CHECK(SCENE::parent(world, child) == 0);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("scene/scene: set_name adds or overwrites the Name") {
    World world;
    world.init();

    const EntityId entity = world.new_entity();
    SCENE::set_name(world, entity, "first");
    CHECK(strcmp(SCENE::name(world, entity), "first") == 0);
    SCENE::set_name(world, entity, "second");
    CHECK(strcmp(SCENE::name(world, entity), "second") == 0);
    CHECK(world.query<Name>().count() == 1);

    world.delete_entity(entity);
    SCENE::set_name(world, entity, "ghost");
    CHECK(world.query<Name>().count() == 0);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("scene/scene: set_parent moves between parents, detaches, and refuses cycles") {
    World world;
    world.init();

    const EntityId a = SCENE::spawn(world, "a", 0);
    const EntityId b = SCENE::spawn(world, "b", 0);
    const EntityId child = SCENE::spawn(world, "child", a);
    const EntityId grandchild = SCENE::spawn(world, "grandchild", child);

    SUBCASE("moving replaces the single CHILD_OF pair") {
        SCENE::set_parent(world, child, b);
        CHECK(SCENE::parent(world, child) == b);
        CHECK_FALSE(world.has(child, ECS::PAIR(ECS::CHILD_OF, a)));
        CHECK(world.has(child, ECS::PAIR(ECS::CHILD_OF, b)));
        CHECK(SCENE::child_count(world, a) == 0);
        CHECK(SCENE::child_count(world, b) == 1);
        // The grandchild followed its parent.
        CHECK(world.depth(grandchild) == 2);
    }

    SUBCASE("a parent of 0 detaches") {
        SCENE::set_parent(world, child, 0);
        CHECK(SCENE::parent(world, child) == 0);
        CHECK_FALSE(world.has(child, ECS::PAIR(ECS::CHILD_OF, a)));
        CHECK(world.depth(child) == 0);
        CHECK(world.depth(grandchild) == 1);
        // Detaching an entity without a parent is a no-op.
        SCENE::set_parent(world, child, 0);
        CHECK(SCENE::parent(world, child) == 0);
    }

    SUBCASE("an entity cannot be its own ancestor") {
        SCENE::set_parent(world, a, a);
        CHECK(SCENE::parent(world, a) == 0);
        SCENE::set_parent(world, a, grandchild);
        CHECK(SCENE::parent(world, a) == 0);
        CHECK(SCENE::parent(world, grandchild) == child);
    }

    SUBCASE("dead entities are ignored") {
        world.delete_entity(b);
        SCENE::set_parent(world, child, b);
        CHECK(SCENE::parent(world, child) == a);
        world.delete_entity(child);
        SCENE::set_parent(world, child, a);
        CHECK(SCENE::child_count(world, a) == 0);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("scene/scene: is_descendant_of walks the whole parent chain") {
    World world;
    world.init();

    const EntityId root = SCENE::spawn(world, "root", 0);
    const EntityId child = SCENE::spawn(world, "child", root);
    const EntityId grandchild = SCENE::spawn(world, "grandchild", child);
    const EntityId other = SCENE::spawn(world, "other", 0);

    CHECK(SCENE::is_descendant_of(world, child, root));
    CHECK(SCENE::is_descendant_of(world, grandchild, root));
    CHECK(SCENE::is_descendant_of(world, grandchild, child));
    CHECK_FALSE(SCENE::is_descendant_of(world, root, root));
    CHECK_FALSE(SCENE::is_descendant_of(world, root, child));
    CHECK_FALSE(SCENE::is_descendant_of(world, other, root));
    CHECK_FALSE(SCENE::is_descendant_of(world, 0, root));
    CHECK_FALSE(SCENE::is_descendant_of(world, child, 0));

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("scene/scene: children lists direct children lowest id first, across archetypes") {
    World world;
    world.init();

    const EntityId parent = SCENE::spawn(world, "parent", 0);
    const EntityId c1 = SCENE::spawn(world, "c1", parent);
    const EntityId c2 = SCENE::spawn(world, "c2", parent);
    const EntityId c3 = SCENE::spawn(world, "c3", parent);
    const EntityId grandchild = SCENE::spawn(world, "grandchild", c2);
    (void)grandchild;
    // Spread the children over several archetypes so the query visits them
    // out of id order.
    world.set(c1, Position { 1, 1 });
    world.add<TagA>(c3);

    CHECK(SCENE::child_count(world, parent) == 3);
    CHECK(SCENE::has_children(world, parent));
    CHECK_FALSE(SCENE::has_children(world, c1));
    CHECK(SCENE::has_children(world, c2));

    DynamicArray<EntityId> out;
    out.push(42);
    CHECK(SCENE::children(world, parent, out) == 3);
    REQUIRE(out.count == 4);
    CHECK(out[0] == 42);
    CHECK(out[1] == c1);
    CHECK(out[2] == c2);
    CHECK(out[3] == c3);

    SUBCASE("a childless or dead parent adds nothing") {
        CHECK(SCENE::children(world, c1, out) == 0);
        world.delete_entity(parent);
        CHECK(SCENE::children(world, parent, out) == 0);
        CHECK(SCENE::child_count(world, parent) == 0);
        CHECK_FALSE(SCENE::has_children(world, parent));
        CHECK(out.count == 4);
    }

    out.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("scene/scene: deleting a parent deletes its subtree but not its siblings") {
    World world;
    world.init();

    const EntityId root = SCENE::spawn(world, "root", 0);
    const EntityId a = SCENE::spawn(world, "a", root);
    const EntityId b = SCENE::spawn(world, "b", root);
    const EntityId a_child = SCENE::spawn(world, "a_child", a);

    CHECK(world.delete_entity(a));
    CHECK_FALSE(world.alive(a));
    CHECK_FALSE(world.alive(a_child));
    CHECK(world.alive(b));
    CHECK(SCENE::child_count(world, root) == 1);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("scene/scene: Scene::init creates scene_root and spawn parents under it") {
    World world;
    world.init();

    Scene scene;
    scene.init(&world);
    REQUIRE(scene.root != 0);
    CHECK(world.alive(scene.root));
    CHECK(strcmp(SCENE::name(world, scene.root), "scene_root") == 0);
    CHECK(SCENE::parent(world, scene.root) == 0);
    CHECK(scene.contains(scene.root));

    const EntityId cube = scene.spawn("cube");
    REQUIRE(cube != 0);
    CHECK(SCENE::parent(world, cube) == scene.root);
    CHECK(strcmp(SCENE::name(world, cube), "cube") == 0);
    CHECK(scene.contains(cube));

    const EntityId wheel = scene.spawn("wheel", cube);
    CHECK(SCENE::parent(world, wheel) == cube);
    CHECK(scene.contains(wheel));
    CHECK(world.depth(wheel) == 2);

    // Content outside the scene: simply unparented.
    const EntityId shader = SCENE::spawn(world, "unlit", 0);
    const EntityId plain = world.new_entity();
    CHECK_FALSE(scene.contains(shader));
    CHECK_FALSE(scene.contains(plain));
    CHECK_FALSE(scene.contains(0));

    SUBCASE("the root only lists scene content") {
        DynamicArray<EntityId> out;
        CHECK(SCENE::children(world, scene.root, out) == 1);
        CHECK(out[0] == cube);
        out.free();
    }

    SUBCASE("moving an entity into the scene") {
        SCENE::set_parent(world, shader, cube);
        CHECK(scene.contains(shader));
        CHECK(SCENE::child_count(world, cube) == 2);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}
