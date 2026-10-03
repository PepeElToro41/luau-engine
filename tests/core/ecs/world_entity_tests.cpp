#include "support/test_support.hpp"

#include "engine/ecs/entity_index.hpp"
#include "engine/memory/arena_allocator.hpp"
#include "engine/memory/heap_allocator.hpp"

#include <new>

// World lifecycle and the entity id space: init() state, create / delete,
// generations and reuse, make_alive, ranges, clear and pair sides.

TEST_CASE("ecs/world: init registers the root archetype and every built-in id") {
    World world;
    world.init();

    REQUIRE(world.root_archetype != nullptr);
    CHECK(world.root_archetype->type.id_count == 0);
    CHECK(world.root_archetype->data.entity_count == 0);

    // Every built-in id, component range included, is alive at generation 0.
    for (Id id = 1; id <= ECS::REST; ++id) {
        CHECK(world.alive(id));
    }
    CHECK_FALSE(world.alive(0));
    CHECK_FALSE(world.alive(ECS::REST + 1));

    // ECS::COMPONENT describes itself and the built-in relations carry their traits.
    const TypeInfo* component_info = static_cast<const TypeInfo*>(world.get(ECS::COMPONENT, ECS::COMPONENT));
    REQUIRE(component_info != nullptr);
    CHECK(component_info->length == sizeof(TypeInfo));
    CHECK(world.has(ECS::CHILD_OF, ECS::EXCLUSIVE));
    CHECK(world.has(ECS::CHILD_OF, ECS::TRAVERSABLE));
    CHECK(world.has(ECS::CHILD_OF, ECS::PAIR(ECS::ON_DELETE_TARGET, ECS::DELETE)));
    CHECK(world.has(ECS::IS_A, ECS::TRAVERSABLE));
    CHECK(world.has(ECS::COMPONENT, ECS::PAIR(ECS::ON_DELETE, ECS::PANIC)));
    CHECK(world.has(1, ECS::PAIR(ECS::ON_DELETE, ECS::PANIC)));

    // Fresh entities start right after the built-ins.
    const EntityId first = world.new_entity();
    CHECK(ECS::ENTITY_LOW(first) == ECS::REST + 1);
    CHECK(ENTITY_INDEX::entity_generation(first) == 0);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/world: new_entity hands out distinct alive ids parked in the root") {
    World world;
    world.init();

    const EntityId a = world.new_entity();
    const EntityId b = world.new_entity();
    const EntityId c = world.new_entity();

    CHECK(a != 0);
    CHECK(a != b);
    CHECK(b != c);
    CHECK(world.alive(a));
    CHECK(world.alive(b));
    CHECK(world.alive(c));
    CHECK(ECS::ENTITY_LOW(b) == ECS::ENTITY_LOW(a) + 1);
    CHECK(ECS::ENTITY_LOW(c) == ECS::ENTITY_LOW(a) + 2);

    // Parked in the root: holds nothing, and the root stores no rows.
    CHECK_FALSE(world.has(a, ECS::COMPONENT));
    CHECK(world.get(a, ECS::COMPONENT) == nullptr);
    CHECK(world.root_archetype->data.entity_count == 0);

    // Ids never issued are not alive.
    CHECK_FALSE(world.alive(ECS::ENTITY_LOW(c) + 1));

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/world: delete_entity kills the entity and refuses dead ids") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    REQUIRE(world.alive(e));

    CHECK(world.delete_entity(e));
    CHECK_FALSE(world.alive(e));

    SUBCASE("a second delete of the same id is refused") {
        CHECK_FALSE(world.delete_entity(e));
    }
    SUBCASE("id 0 and never-issued ids are refused") {
        CHECK_FALSE(world.delete_entity(0));
        CHECK_FALSE(world.delete_entity(ECS::ENTITY_LOW(e) + 100));
    }
    SUBCASE("an entity with ids drops its row") {
        const EntityId holder = world.new_entity();
        world.add<TagA>(holder);
        world.set(holder, Position { 1, 2 });
        CHECK(world.delete_entity(holder));
        CHECK_FALSE(world.alive(holder));
        CHECK_FALSE(world.has<TagA>(holder));
        CHECK(world.get<Position>(holder) == nullptr);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/world: built-in ids cannot be deleted (ON_DELETE, PANIC)") {
    World world;
    world.init();

    const Id built_ins[] = { 1, ECS::MAX_COMPONENT_ID, ECS::WILDCARD, ECS::COMPONENT, ECS::ON_DELETE, ECS::CHILD_OF, ECS::IS_A, ECS::REST };
    for (const Id id : built_ins) {
        CHECK_FALSE(world.delete_entity(id));
        CHECK(world.alive(id));
    }

    // Component ids claimed by types are built-ins too.
    const EntityId position = world.component<Position>();
    CHECK_FALSE(world.delete_entity(position));
    CHECK(world.alive(position));
    CHECK(world.component<Position>() == position);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/world: deleting bumps the generation and the low id is reused") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    const EntityId other = world.new_entity();
    REQUIRE(world.delete_entity(e));

    const EntityId reused = world.new_entity();
    CHECK(ECS::ENTITY_LOW(reused) == ECS::ENTITY_LOW(e));
    CHECK(ENTITY_INDEX::entity_generation(reused) == ENTITY_INDEX::entity_generation(e) + 1);
    CHECK(reused != e);

    // The stale id stays dead even though its slot is alive again.
    CHECK(world.alive(reused));
    CHECK_FALSE(world.alive(e));
    CHECK_FALSE(world.delete_entity(e));
    CHECK(world.alive(reused));

    // Operations through the stale id are no-ops and never touch the new entity.
    world.add<TagA>(e);
    CHECK_FALSE(world.has<TagA>(reused));
    CHECK_FALSE(world.has<TagA>(e));

    // Nothing else was recycled: the next id is fresh.
    const EntityId fresh = world.new_entity();
    CHECK(ECS::ENTITY_LOW(fresh) == ECS::ENTITY_LOW(other) + 1);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/world: deleting many entities recycles every low id once") {
    World world;
    world.init();

    constexpr usz COUNT = 64;
    EntityId entities[COUNT];
    for (usz i = 0; i < COUNT; ++i) {
        entities[i] = world.new_entity();
    }
    for (usz i = 0; i < COUNT; i += 2) {
        CHECK(world.delete_entity(entities[i]));
    }
    for (usz i = 0; i < COUNT; ++i) {
        CHECK(world.alive(entities[i]) == (i % 2 == 1));
    }

    // Each recycled id is one of the deleted ones at the next generation.
    const EntityIdLow highest = ECS::ENTITY_LOW(entities[COUNT - 1]);
    for (usz i = 0; i < COUNT / 2; ++i) {
        const EntityId recycled = world.new_entity();
        CHECK(ECS::ENTITY_LOW(recycled) <= highest);
        CHECK(ENTITY_INDEX::entity_generation(recycled) == 1);
        CHECK(world.alive(recycled));
    }
    // The pool is drained: the next one is fresh.
    const EntityId fresh = world.new_entity();
    CHECK(ECS::ENTITY_LOW(fresh) == highest + 1);
    CHECK(ENTITY_INDEX::entity_generation(fresh) == 0);

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/world: make_alive registers a specific id and generation") {
    World world;
    world.init();

    SUBCASE("a never-issued id with an explicit generation") {
        const EntityId wanted = ENTITY_INDEX::append_generation(5000, 3);
        world.make_alive(wanted);
        CHECK(world.alive(wanted));
        CHECK_FALSE(world.alive(5000));
        CHECK_FALSE(world.alive(ENTITY_INDEX::append_generation(5000, 2)));

        // Parked in the root and fully usable.
        CHECK_FALSE(world.has<TagA>(wanted));
        world.add<TagA>(wanted);
        world.set(wanted, Health { 9 });
        CHECK(world.has<TagA>(wanted));
        REQUIRE(world.get<Health>(wanted) != nullptr);
        CHECK(world.get<Health>(wanted)->value == 9);

        CHECK(world.delete_entity(wanted));
        CHECK_FALSE(world.alive(wanted));
    }
    SUBCASE("reviving a deleted entity at the generation it had") {
        const EntityId e = world.new_entity();
        world.add<TagA>(e);
        REQUIRE(world.delete_entity(e));
        REQUIRE_FALSE(world.alive(e));

        world.make_alive(e);
        CHECK(world.alive(e));
        // Revived entities start clean in the root.
        CHECK_FALSE(world.has<TagA>(e));

        // The slot is alive again, so it is not handed out by new_entity().
        const EntityId next = world.new_entity();
        CHECK(ECS::ENTITY_LOW(next) != ECS::ENTITY_LOW(e));
    }
    SUBCASE("an alive entity keeps its archetype and adopts the generation") {
        const EntityId e = world.new_entity();
        world.add<TagA>(e);

        world.make_alive(e);
        CHECK(world.alive(e));
        CHECK(world.has<TagA>(e));

        const EntityId regenerated = ENTITY_INDEX::append_generation(ECS::ENTITY_LOW(e), 7);
        world.make_alive(regenerated);
        CHECK(world.alive(regenerated));
        CHECK_FALSE(world.alive(e));
        CHECK(world.has<TagA>(regenerated));
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/world: set_range restricts the ids new_entity issues") {
    World world;
    world.init();

    SUBCASE("a bounded range is exhausted in order") {
        world.set_range(1000, 1002);
        const EntityId a = world.new_entity();
        const EntityId b = world.new_entity();
        const EntityId c = world.new_entity();
        CHECK(ECS::ENTITY_LOW(a) == 1000);
        CHECK(ECS::ENTITY_LOW(b) == 1001);
        CHECK(ECS::ENTITY_LOW(c) == 1002);
        CHECK(world.new_entity() == 0);

        // Deleting an in-range id makes room again.
        REQUIRE(world.delete_entity(b));
        const EntityId recycled = world.new_entity();
        CHECK(ECS::ENTITY_LOW(recycled) == 1001);
        CHECK(ENTITY_INDEX::entity_generation(recycled) == 1);
        CHECK(world.new_entity() == 0);

        // A max of 0 lifts the bound.
        world.set_range(1000, 0);
        const EntityId d = world.new_entity();
        CHECK(ECS::ENTITY_LOW(d) == 1003);
    }
    SUBCASE("ids outside the range are not recycled") {
        const EntityId before = world.new_entity();
        world.set_range(2000, 0);
        REQUIRE(world.delete_entity(before));
        const EntityId next = world.new_entity();
        CHECK(ECS::ENTITY_LOW(next) == 2000);
        CHECK(ECS::ENTITY_LOW(world.new_entity()) == 2001);
    }
    SUBCASE("a min of 0 continues after the highest id issued so far") {
        const EntityId last = world.new_entity();
        world.set_range(0, 0);
        const EntityId next = world.new_entity();
        CHECK(ECS::ENTITY_LOW(next) == ECS::ENTITY_LOW(last) + 1);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/world: clear removes every id but keeps the entity alive") {
    World world;
    world.init();

    const EntityId target = world.new_entity();
    const EntityId e = world.new_entity();
    world.add<TagA>(e);
    world.set(e, Position { 3, 4 });
    world.add<Likes>(e, target);
    REQUIRE(world.has<TagA>(e));

    world.clear(e);
    CHECK(world.alive(e));
    CHECK_FALSE(world.has<TagA>(e));
    CHECK_FALSE(world.has<Position>(e));
    CHECK_FALSE(world.has<Likes>(e, target));
    CHECK(world.get<Position>(e) == nullptr);

    SUBCASE("the entity is reusable afterwards") {
        world.set(e, Health { 5 });
        REQUIRE(world.get<Health>(e) != nullptr);
        CHECK(world.get<Health>(e)->value == 5);
        CHECK_FALSE(world.has<TagA>(e));
    }
    SUBCASE("clearing an empty or dead entity is a no-op") {
        world.clear(e);
        CHECK(world.alive(e));
        REQUIRE(world.delete_entity(e));
        world.clear(e);
        CHECK_FALSE(world.alive(e));
    }
    SUBCASE("nothing cascades: pairs on other entities pointing at it stay") {
        const EntityId fan = world.new_entity();
        world.add<Likes>(fan, e);
        world.clear(e);
        CHECK(world.has<Likes>(fan, e));
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/world: pair_first / pair_second resolve the alive entities behind a pair") {
    World world;
    world.init();

    // Recycle an id so the target carries a non-zero generation.
    const EntityId stale = world.new_entity();
    REQUIRE(world.delete_entity(stale));
    const EntityId target = world.new_entity();
    REQUIRE(ENTITY_INDEX::entity_generation(target) == 1);
    const EntityId relation = world.new_entity();

    const Id pair = World::pair(relation, target);
    CHECK(pair == ECS::PAIR(relation, target));
    CHECK(ECS::IS_PAIR(pair));

    // The raw helpers only know the low ids; the world adds the generation back.
    CHECK(ECS::PAIR_FIRST(pair) == ECS::ENTITY_LOW(relation));
    CHECK(ECS::PAIR_SECOND(pair) == ECS::ENTITY_LOW(target));
    CHECK(ECS::PAIR_SECOND(pair) != target);
    CHECK(world.pair_first(pair) == relation);
    CHECK(world.pair_second(pair) == target);

    // The same pair id regardless of which generation was used to build it.
    CHECK(ECS::PAIR(relation, stale) == pair);

    SUBCASE("a dead side resolves to 0") {
        REQUIRE(world.delete_entity(target));
        CHECK(world.pair_second(pair) == 0);
        CHECK(world.pair_first(pair) == relation);

        // Once the slot is recycled the pair resolves to the new entity.
        const EntityId recycled = world.new_entity();
        REQUIRE(ECS::ENTITY_LOW(recycled) == ECS::ENTITY_LOW(target));
        CHECK(world.pair_second(pair) == recycled);
    }
    SUBCASE("built-in sides resolve to themselves") {
        const Id child_of = ECS::PAIR(ECS::CHILD_OF, target);
        CHECK(world.pair_first(child_of) == ECS::CHILD_OF);
        CHECK(world.pair_second(child_of) == target);
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/world: a constructed world only wires the allocator") {
    ArenaAllocator arena(MEMORY::MB);
    World world(&arena);

    CHECK(world.allocator == &arena);
    CHECK(world.root_archetype == nullptr);
    CHECK(world.entity_index.is_empty());
    CHECK(world.archetypes.alive_count == 0);
    CHECK(world.component_records.alive_count == 0);
    CHECK(world.next_component_id == 1);
    CHECK(world.hook_counts[HOOK_ADDED] == 0);
    CHECK(world.any_hooks == nullptr);

    // Nothing is alive without init(); queries report "missing".
    CHECK_FALSE(world.alive(1));
    CHECK_FALSE(world.alive(ECS::COMPONENT));
    CHECK_FALSE(world.has(1, ECS::COMPONENT));
    CHECK(world.get(1, ECS::COMPONENT) == nullptr);

    // Its tables live on the arena, which free() rewinds nothing of; the
    // world must still be releasable.
    world.free();
    CHECK(world.root_archetype == nullptr);

    // By design a world is not init()ed on an arena: Archetype::ensure_capacity
    // grows columns through BaseAllocator::reallocate, which arenas do not
    // support. Archetype storage belongs on the heap allocator.

    arena.release();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/world: World::destroy releases a world created on an allocator") {
    BaseAllocator* allocator = MEMORY::heap_allocator();

    World* world = new (allocator->allocate_array<World>(1)) World(allocator);
    CHECK(world->allocator == allocator);
    world->init();

    const EntityId e = world->new_entity();
    world->set(e, Position { 1, 2 });
    world->add(e, ECS::PAIR(ECS::CHILD_OF, ECS::REST));
    REQUIRE(world->get<Position>(e) != nullptr);
    CHECK(world->get<Position>(e)->y == 2);

    // Frees the world's tables and returns the World block to the allocator.
    World::destroy(world);
    CHECK_ARENA_CLEAN();

    // nullptr is accepted.
    World::destroy(nullptr);
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/world: free resets the world so it can be initialized again") {
    World world;
    world.init();

    const EntityId e = world.new_entity();
    world.set(e, Position { 1, 1 });
    world.add<TagA>(e);
    const EntityId position = world.component<Position>();

    world.free();
    CHECK(world.root_archetype == nullptr);
    CHECK(world.next_component_id == 1);
    CHECK_FALSE(world.alive(e));
    CHECK_ARENA_CLEAN();

    world.init();
    CHECK(world.root_archetype != nullptr);
    CHECK_FALSE(world.alive(e));
    // Component ids are handed out from the start again.
    CHECK(world.component<Position>() == position);
    const EntityId again = world.new_entity();
    CHECK(ECS::ENTITY_LOW(again) == ECS::REST + 1);
    world.set(again, Position { 2, 2 });
    REQUIRE(world.get<Position>(again) != nullptr);
    CHECK(world.get<Position>(again)->x == 2);

    world.free();
    CHECK_ARENA_CLEAN();
}
