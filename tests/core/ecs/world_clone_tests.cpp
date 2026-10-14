#include "support/test_support.hpp"

#include "engine/asset/asset_types/scene_asset.hpp"
#include "engine/asset/text_asset.hpp"
#include "engine/ecs/archetype/archetype.hpp"
#include "engine/ecs/component_registry.hpp"
#include "engine/ecs/entity_index.hpp"
#include "engine/ecs/query/query_builder.hpp"
#include "engine/ecs/world_clone.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/serialize.hpp"
#include "engine/utils/log.hpp"

#include <cstring>
#include <string>

// WORLD_CLONE::clone: an id-exact copy of a world, with Cloners for the
// components that own memory, and the fence the destination issues above.

namespace {

// A component that owns a heap block: a memcpy would alias it.
struct Buffer {
    u8* bytes = nullptr;
    usz size = 0;
};

// Live Buffer blocks, to see the deep copies come and go.
usz buffer_blocks = 0;

Buffer make_buffer(const u8 fill, const usz size) {
    Buffer buffer;
    buffer.size = size;
    buffer.bytes = MEMORY::heap_allocator()->allocate_array<u8>(size);
    memset(buffer.bytes, fill, size);
    buffer_blocks += 1;
    return buffer;
}

void buffer_clone(const void* source, void* destination, const usz size, void* user_data) {
    CHECK(size == sizeof(Buffer));
    *static_cast<int*>(user_data) += 1;
    const Buffer* from = static_cast<const Buffer*>(source);
    Buffer* to = static_cast<Buffer*>(destination);
    *to = *from;
    if (from->bytes != nullptr) {
        to->bytes = MEMORY::heap_allocator()->allocate_array<u8>(from->size);
        memcpy(to->bytes, from->bytes, from->size);
        buffer_blocks += 1;
    }
}

void buffer_removed(World* world, const EntityId entity, Id, void*) {
    if (Buffer* buffer = world->get<Buffer>(entity)) {
        MEMORY::heap_allocator()->free(buffer->bytes);
        buffer->bytes = nullptr;
        buffer_blocks -= 1;
    }
}

// A component with a non-zero default and a pointer-free payload.
struct Spin {
    f32 rate = 1.0f;
};

// An entity reference.
struct Ref {
    EntityId target = 0;
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

const AssetGuid GUID = { 0x179a52dd06b33f96ull, 0xe26f6a98c55cbe22ull };

std::string write_scene(World& world, const EntityId root) {
    SerializeContext ctx;
    DynamicArray<char> out;
    SCENE_ASSET::write(world, root, GUID, ctx, out);
    std::string text(out.data, out.count);
    out.free();
    return text;
}

// Every alive entity of `a` is alive in `b` with the same archetype type and
// the same component bytes, `skip` excepted (compared by its owner).
void check_same_content(World& a, World& b, const Id skip = 0) {
    const usz count = a.entity_index.count();
    CHECK(b.entity_index.count() == count);
    for (usz i = 0; i < count; ++i) {
        const EntityId entity = a.entity_index.get_alive_id(i);
        REQUIRE(b.alive(entity));
        const EntityRecord* record_a = a.entity_index.get_record_alive(entity);
        const EntityRecord* record_b = b.entity_index.get_record_alive(entity);
        REQUIRE(record_a != nullptr);
        REQUIRE(record_b != nullptr);
        const ArchetypeType& type_a = record_a->archetype->type;
        const ArchetypeType& type_b = record_b->archetype->type;
        REQUIRE(type_a.equals(type_b));
        for (usz c = 0; c < type_a.id_count; ++c) {
            const ArchetypeColumn& column_a = record_a->archetype->data.columns[c];
            const ArchetypeColumn& column_b = record_b->archetype->data.columns[c];
            CHECK(column_a.type_info.length == column_b.type_info.length);
            if (column_a.type_info.length == 0 || type_a.ids[c] == skip) {
                continue;
            }
            CHECK(memcmp(column_a.read(record_a->archetype_row), column_b.read(record_b->archetype_row), column_a.type_info.length) == 0);
        }
    }
}

DynamicArray<EntityId> collect(DynamicQuery& query) {
    DynamicArray<EntityId> entities;
    TemporalAllocator temp = TemporalAllocator::create();
    QueryIter it = query.begin(&temp);
    while (it.next(&it)) {
        for (usz row = 0; row < it.count; row++) {
            entities.push(it.entities[row]);
        }
    }
    return entities;
}

} // namespace

TEST_CASE("ecs/world_clone: the clone holds every entity under its id, generation, type and data") {
    World source;
    source.init();
    SERIALIZE::expose<Position>(source, "Position", &position_write, &position_read, 0);
    SERIALIZE::expose<Health>(source, "Health", &health_write, &health_read, 20);
    SERIALIZE::expose<Ref>(source, "Ref", &ref_write, &ref_read, 10);
    SERIALIZE::expose_tag<TagA>(source, "TagA");
    SERIALIZE::expose_tag<TagB>(source, "TagB");
    int clones = 0;
    CLONE::expose<Buffer>(source, &buffer_clone, &clones);
    source.hook_removed<Buffer>(&buffer_removed);
    buffer_blocks = 0;

    // A recycled id, so a generation other than 0 is in the mix.
    const EntityId stale = source.new_entity();
    REQUIRE(source.delete_entity(stale));
    const EntityId root = SCENE::spawn(source, "scene_root", 0);
    REQUIRE(ENTITY_INDEX::entity_generation(root) == 1);
    const EntityId parent = SCENE::spawn(source, "parent", root);
    source.set(parent, Position { 1, 2 });
    source.set(parent, Health { 7 });
    source.add<TagA>(parent);
    const EntityId child = SCENE::spawn(source, "child", parent);
    source.set(child, Position { 3, 4 });
    source.set(child, Ref { parent });
    const EntityId grandchild = SCENE::spawn(source, "grandchild", child);
    source.set(grandchild, Spin { 2.5f });
    source.add<TagB>(grandchild);
    const EntityId loose = source.new_entity();
    source.set(loose, make_buffer(0xAB, 24));
    source.add<Likes>(loose, parent);
    source.set<Position>(loose, child, Position { 9, 9 });
    const EntityId named = source.register_component("Mana", TypeInfo { 4, 4 });
    REQUIRE(named != 0);
    const i32 mana = 42;
    source.set(child, named, &mana);
    REQUIRE(buffer_blocks == 1);
    const std::string before = write_scene(source, root);

    World clone;
    clone.init();
    const WorldCloneResult result = WORLD_CLONE::clone(source, clone);
    REQUIRE(result.ok);
    CHECK(result.error == nullptr);
    CHECK(result.entities == source.entity_index.count());
    CHECK(result.archetypes > 0);
    CHECK(clones == 1);
    CHECK(buffer_blocks == 2);
    clone.hook_removed<Buffer>(&buffer_removed);

    SUBCASE("ids, generations, types and bytes") {
        check_same_content(source, clone, source.id<Buffer>());
        CHECK(clone.alive(root));
        CHECK_FALSE(clone.alive(stale));
        CHECK(clone.alive(ENTITY_INDEX::append_generation(ECS::ENTITY_LOW(root), 1)));
        REQUIRE(clone.get<Position>(parent) != nullptr);
        CHECK(clone.get<Position>(parent)->y == 2);
        CHECK(clone.get<Health>(parent)->value == 7);
        CHECK(clone.has<TagA>(parent));
        CHECK(clone.get<Ref>(child)->target == parent);
        CHECK(clone.get<Spin>(grandchild)->rate == 2.5f);
        CHECK(clone.has<TagB>(grandchild));
        CHECK(clone.has<Likes>(loose, parent));
        REQUIRE(clone.get<Position>(loose, child) != nullptr);
        CHECK(clone.get<Position>(loose, child)->x == 9);
        CHECK(strcmp(SCENE::name(clone, grandchild), "grandchild") == 0);
        // The source was only read.
        CHECK(source.get<Position>(parent)->x == 1);
        CHECK(source.entity_index.count() == clone.entity_index.count());
    }
    SUBCASE("the component tiers agree") {
        CHECK(clone.id<Position>() == source.id<Position>());
        CHECK(clone.id<Health>() == source.id<Health>());
        CHECK(clone.id<TagA>() == source.id<TagA>());
        CHECK(clone.id<Ref>() == source.id<Ref>());
        CHECK(clone.id<Spin>() == source.id<Spin>());
        CHECK(clone.id<Buffer>() == source.id<Buffer>());
        CHECK(clone.id<Name>() == source.id<Name>());
        CHECK(clone.id<Serializer>() == source.id<Serializer>());
        CHECK(clone.runtime_next_id == source.runtime_next_id);
        CHECK(clone.find_component("Mana") == named);
        REQUIRE(clone.get(child, named) != nullptr);
        CHECK(*static_cast<const i32*>(clone.get(child, named)) == 42);
        CHECK(clone.component_type_of(source.id<Position>()) == TYPE_ID::get<Position>());
        // A type first met in the clone gets the next free runtime id, never
        // one the source handed out.
        const EntityId fresh = clone.component<Velocity>();
        CHECK(fresh == source.runtime_next_id);
        CHECK(fresh != source.id<Position>());
        CHECK(fresh != named);
    }
    SUBCASE("the scene text is byte-identical") {
        CHECK(write_scene(clone, root) == before);
        CHECK(write_scene(source, root) == before);
    }
    SUBCASE("the hierarchy came along, caches included") {
        CHECK(clone.depth(parent) == 1);
        CHECK(clone.depth(child) == 2);
        CHECK(clone.depth(grandchild) == 3);
        CHECK(SCENE::parent(clone, grandchild) == child);
        DynamicArray<EntityId> children;
        SCENE::children(clone, parent, children);
        REQUIRE(children.count == 1);
        CHECK(children[0] == child);
        children.free();
        // The cascade order reads the cloned nodes.
        DynamicQuery query = clone.query_build().term<Name>().cascade().build();
        DynamicArray<EntityId> order = collect(query);
        usz index_parent = 0;
        usz index_child = 0;
        usz index_grandchild = 0;
        for (usz i = 0; i < order.count; ++i) {
            if (order[i] == parent) {
                index_parent = i;
            } else if (order[i] == child) {
                index_child = i;
            } else if (order[i] == grandchild) {
                index_grandchild = i;
            }
        }
        CHECK(index_parent < index_child);
        CHECK(index_child < index_grandchild);
        order.free();
        query.free();
        // And reparenting in the clone keeps them right.
        SCENE::set_parent(clone, grandchild, root);
        CHECK(clone.depth(grandchild) == 1);
        CHECK(source.depth(grandchild) == 3);
    }
    SUBCASE("a Cloner deep-copies, so the two worlds own separate blocks") {
        const Buffer* original = source.get<Buffer>(loose);
        const Buffer* copy = clone.get<Buffer>(loose);
        REQUIRE(original != nullptr);
        REQUIRE(copy != nullptr);
        CHECK(copy->bytes != original->bytes);
        CHECK(copy->size == 24);
        CHECK(memcmp(copy->bytes, original->bytes, 24) == 0);
        copy->bytes[0] = 0x11;
        CHECK(original->bytes[0] == 0xAB);
    }
    SUBCASE("the clone is independent: edits on one side stay there") {
        clone.get<Position>(parent)->x = 100;
        clone.set(child, Health { 1 });
        clone.delete_entity(grandchild);
        CHECK(source.get<Position>(parent)->x == 1);
        CHECK_FALSE(source.has<Health>(child));
        CHECK(source.alive(grandchild));
        CHECK_FALSE(clone.alive(grandchild));
        CHECK(write_scene(source, root) == before);
    }
    SUBCASE("the fence: new ids start above every cloned one, and cloned ids are never recycled") {
        CHECK(result.fence != 0);
        CHECK(result.fence == clone.range_min());
        CHECK(result.fence == source.last_id() + 1);
        const EntityId created = clone.new_entity();
        CHECK(ECS::ENTITY_LOW(created) == result.fence);
        CHECK_FALSE(source.alive(created));
        // A cloned entity deleted in the clone falls below the range: dropped.
        REQUIRE(clone.delete_entity(child));
        CHECK(ECS::ENTITY_LOW(clone.new_entity()) == result.fence + 1);
        CHECK_FALSE(clone.alive(child));
        CHECK(source.alive(child));
        // The source keeps issuing where it was.
        const EntityId in_source = source.new_entity();
        CHECK(ECS::ENTITY_LOW(in_source) == result.fence);
    }
    SUBCASE("without a fence the clone issues ids where the source does") {
        World plain;
        plain.init();
        WorldCloneDesc desc;
        desc.fence = false;
        const WorldCloneResult unfenced = WORLD_CLONE::clone(source, plain, desc);
        REQUIRE(unfenced.ok);
        CHECK(unfenced.fence == 0);
        CHECK(plain.range_min() == ECS::FIRST_USER_ENTITY_ID);
        CHECK(ECS::ENTITY_LOW(plain.new_entity()) == source.last_id() + 1);
        plain.hook_removed<Buffer>(&buffer_removed);
        plain.free();
    }

    clone.free();
    CHECK(buffer_blocks == 1);
    source.free();
    CHECK(buffer_blocks == 0);
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/world_clone: preregistered and runtime components both keep their ids") {
    COMPONENT_REGISTRY::reset();
    const Id spin = COMPONENT_REGISTRY::preregister<Spin>("Spin");
    REQUIRE(spin == ECS::FIRST_COMPONENT_ID);

    World source;
    source.init();
    const EntityId e = source.new_entity();
    source.set(e, Spin { 3.0f });
    source.set(e, Position { 5, 6 });
    CHECK(source.id<Spin>() == spin);
    const Id position = source.id<Position>();
    CHECK(position == ECS::MAX_COMPONENT_ID);

    World clone;
    clone.init();
    REQUIRE(WORLD_CLONE::clone(source, clone).ok);
    CHECK(clone.id<Spin>() == spin);
    CHECK(clone.id<Position>() == position);
    REQUIRE(clone.get<Spin>(e) != nullptr);
    CHECK(clone.get<Spin>(e)->rate == 3.0f);
    CHECK(clone.get<Position>(e)->y == 6);
    // A world not cloned from the source agrees on the preregistered id
    // only, as the tiers promise.
    World other;
    other.init();
    CHECK(other.id<Spin>() == spin);
    CHECK(other.id<Health>() == ECS::MAX_COMPONENT_ID);

    other.free();
    clone.free();
    source.free();
    COMPONENT_REGISTRY::reset();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/world_clone: relation traits and exclusive pairs survive the clone") {
    World source;
    source.init();
    // A user relation made exclusive before use, the way AssetType is.
    const EntityId owns = source.tag<Likes>();
    source.add(owns, ECS::EXCLUSIVE);
    const EntityId apple = source.new_entity();
    const EntityId pear = source.new_entity();
    const EntityId fan = source.new_entity();
    source.add(fan, ECS::PAIR(owns, apple));
    // A traversable relation of the user's own.
    const EntityId above = source.tag<Eats>();
    source.add(above, ECS::TRAVERSABLE);
    const EntityId top = source.new_entity();
    const EntityId middle = source.new_entity();
    const EntityId bottom = source.new_entity();
    source.add(middle, ECS::PAIR(above, top));
    source.add(bottom, ECS::PAIR(above, middle));
    CHECK(source.depth(bottom, above) == 2);

    World clone;
    clone.init();
    REQUIRE(WORLD_CLONE::clone(source, clone).ok);
    CHECK(clone.has(owns, ECS::EXCLUSIVE));
    CHECK(clone.has(above, ECS::TRAVERSABLE));
    CHECK(clone.has(fan, ECS::PAIR(owns, apple)));
    // Exclusive in the clone too: the second target replaces the first.
    clone.add(fan, ECS::PAIR(owns, pear));
    CHECK(clone.has(fan, ECS::PAIR(owns, pear)));
    CHECK_FALSE(clone.has(fan, ECS::PAIR(owns, apple)));
    CHECK(source.has(fan, ECS::PAIR(owns, apple)));
    // Traversable in the clone too.
    CHECK(clone.depth(bottom, above) == 2);
    CHECK(clone.depth(middle, above) == 1);
    // Deleting a CHILD_OF parent still cascades in the clone.
    const EntityId root = SCENE::spawn(clone, "root", 0);
    const EntityId leaf = SCENE::spawn(clone, "leaf", root);
    REQUIRE(clone.delete_entity(root));
    CHECK_FALSE(clone.alive(leaf));

    clone.free();
    source.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/world_clone: a destination that is not fresh, or the source itself, is refused") {
    World source;
    source.init();
    const EntityId e = source.new_entity();
    source.set(e, Position { 1, 1 });

    const LogLevel level = LOG::stderr_level();
    LOG::set_stderr_level(LOG_LEVEL_NONE);
    SUBCASE("into itself") {
        const WorldCloneResult result = WORLD_CLONE::clone(source, source);
        CHECK_FALSE(result.ok);
        CHECK(result.error != nullptr);
        CHECK(source.alive(e));
    }
    SUBCASE("into a world with an entity of its own") {
        World busy;
        busy.init();
        busy.new_entity();
        const WorldCloneResult result = WORLD_CLONE::clone(source, busy);
        CHECK_FALSE(result.ok);
        CHECK(result.error != nullptr);
        // Nothing of the source came over.
        CHECK(busy.entity_index.count() == ECS::REST + 1);
        CHECK(busy.type_index.count == 0);
        busy.free();
    }
    SUBCASE("into a world that registered a component") {
        World busy;
        busy.init();
        busy.component<Velocity>();
        const WorldCloneResult result = WORLD_CLONE::clone(source, busy);
        CHECK_FALSE(result.ok);
        busy.free();
    }
    SUBCASE("into a world that was never init()ed") {
        World bare;
        const WorldCloneResult result = WORLD_CLONE::clone(source, bare);
        CHECK_FALSE(result.ok);
        bare.free();
    }
    LOG::set_stderr_level(level);

    source.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/world_clone: CLONE::of never registers the Cloner component on a world") {
    World world;
    world.init();
    const EntityId position = world.component<Position>();
    const Id next = world.runtime_next_id;
    CHECK(CLONE::of(world, position) == nullptr);
    CHECK(world.runtime_next_id == next);
    CHECK(world.type_index.find(TYPE_ID::get<Cloner>()) == nullptr);

    int calls = 0;
    CLONE::expose<Position>(world, &buffer_clone, &calls);
    const Cloner* cloner = CLONE::of(world, position);
    REQUIRE(cloner != nullptr);
    CHECK(cloner->clone == &buffer_clone);
    CHECK(cloner->user_data == &calls);
    CHECK(CLONE::of(world, world.id<Health>()) == nullptr);
    CHECK(CLONE::of(world, 0) == nullptr);
    CHECK(CLONE::of(world, ECS::PAIR(ECS::CHILD_OF, position)) == nullptr);
    // Ignored shapes.
    CLONE::expose(world, 0, &buffer_clone);
    CLONE::expose(world, position, nullptr);
    CHECK(CLONE::of(world, position)->clone == &buffer_clone);

    world.free();
    CHECK_ARENA_CLEAN();
}
