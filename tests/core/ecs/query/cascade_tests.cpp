#include "support/test_support.hpp"

#include "engine/ecs/hierarchy.hpp"
#include "engine/ecs/query/dynamic_query.hpp"
#include "engine/ecs/query/query_builder.hpp"
#include "engine/ecs/query/query_iter.hpp"
#include "engine/ecs/query/query_scan.hpp"
#include "engine/ecs/query/query_term.hpp"
#include "engine/ecs/query/query_vm.hpp"

// cascade(): up() traversal that also orders the results by depth along the
// relation. The order of the uncached walk, of the cached walk across
// hierarchy changes and new archetypes, the multi-parent case, desc(), and
// the term shapes the compiler rejects.

namespace {

// The entities a query yields, in order.
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

// Position of `entity` in `entities`, or the count if absent.
usz index_of(const DynamicArray<EntityId>& entities, const EntityId entity) {
    for (usz i = 0; i < entities.count; i++) {
        if (entities[i] == entity) {
            return i;
        }
    }
    return entities.count;
}

// Whether the depths along `relation` never decrease (or never increase)
// along the list.
bool ordered_by_depth(World& world, const DynamicArray<EntityId>& entities, const Id relation, const bool descending) {
    for (usz i = 1; i < entities.count; i++) {
        const u32 previous = world.depth(entities[i - 1], relation);
        const u32 current = world.depth(entities[i], relation);
        if (descending ? current > previous : current < previous) {
            return false;
        }
    }
    return true;
}

// A hierarchy whose archetypes are created deepest first, so that creation
// order (the order SELECT would otherwise walk in) is the reverse of depth:
//
//     root ─ child ─ grandchild     other_root ─ other_child
//
// Every entity has a Position; grandchild also has TagA and other_root TagB,
// so each sits in its own archetype.
struct Tree {
    EntityId root = 0;
    EntityId child = 0;
    EntityId grandchild = 0;
    EntityId other_root = 0;
    EntityId other_child = 0;
};

Tree build_tree(World& world) {
    Tree tree;
    tree.root = world.new_entity();
    tree.child = world.new_entity();
    tree.grandchild = world.new_entity();
    tree.other_root = world.new_entity();
    tree.other_child = world.new_entity();

    world.add(tree.grandchild, world.pair(ECS::CHILD_OF, tree.child));
    world.set<Position>(tree.grandchild, { 3, 0 });
    world.add<TagA>(tree.grandchild);
    world.add(tree.child, world.pair(ECS::CHILD_OF, tree.root));
    world.set<Position>(tree.child, { 2, 0 });
    world.set<Position>(tree.root, { 1, 0 });
    world.add(tree.other_child, world.pair(ECS::CHILD_OF, tree.other_root));
    world.set<Position>(tree.other_child, { 20, 0 });
    world.set<Position>(tree.other_root, { 10, 0 });
    world.add<TagB>(tree.other_root);
    return tree;
}

} // namespace

TEST_CASE("ecs/dynamic_query: cascade() yields every ancestor before its descendants") {
    World world;
    world.init();
    const Tree tree = build_tree(world);

    QueryBuilder builder = world.query_build();
    builder.term<Position>().term<Position>().cascade().optional();
    DynamicQuery query = builder.build();
    REQUIRE(query.is_ok());

    // The builder set the term up as an up() term that also orders.
    const QueryTerm& term = query.terms()[1];
    CHECK(term.traverses());
    CHECK(term.cascades());
    CHECK_FALSE(term.descends());
    CHECK(term.traverse == ECS::CHILD_OF);
    CHECK(query.program.cascade_term == 1);
    CHECK(query.program.cascade_relation == ECS::ENTITY_LOW(ECS::CHILD_OF));
    CHECK_FALSE(query.program.cascade_desc);

    // With optional() the roots are in, and first.
    DynamicArray<EntityId> order = collect(query);
    CHECK(order.count == 5);
    CHECK(ordered_by_depth(world, order, ECS::CHILD_OF, false));
    CHECK(index_of(order, tree.root) < index_of(order, tree.child));
    CHECK(index_of(order, tree.child) < index_of(order, tree.grandchild));
    CHECK(index_of(order, tree.other_root) < index_of(order, tree.other_child));
    CHECK(world.depth(order[0]) == 0);
    CHECK(world.depth(order.last()) == 2);
    order.free();

    // The field is the parent's Position, shared by the chunk, nullptr for
    // a root; every parent has been visited by the time its child comes.
    DynamicArray<EntityId> visited;
    query.iter<Position, Position>([&](QueryIter& it, Position* own, Position* parent) {
        CHECK(it.count == 1);
        CHECK_FALSE(it.shared[0]);
        const EntityId entity = it.entities[0];
        if (entity == tree.root || entity == tree.other_root) {
            CHECK(parent == nullptr);
            CHECK(it.sources[1] == 0);
        } else {
            REQUIRE(parent != nullptr);
            CHECK(it.shared[1]);
            CHECK(it.sources[1] != 0);
            CHECK(index_of(visited, it.sources[1]) < visited.count);
            CHECK(parent->x == world.get<Position>(it.sources[1])->x);
        }
        CHECK(own->x == world.get<Position>(entity)->x);
        visited.push(entity);
    });
    CHECK(visited.count == 5);
    visited.free();

    // Propagating down the tree in one pass works because of the order.
    query.each<Position, Position*>([](EntityId, Position& own, Position* parent) {
        if (parent != nullptr) {
            own.y = parent->y + 1;
        }
    });
    CHECK(world.get<Position>(tree.root)->y == 0);
    CHECK(world.get<Position>(tree.child)->y == 1);
    CHECK(world.get<Position>(tree.grandchild)->y == 2);
    CHECK(world.get<Position>(tree.other_child)->y == 1);

    SUBCASE("without optional() the entities with no such ancestor are excluded, like up()") {
        QueryBuilder b2 = world.query_build();
        b2.term<Position>().term<Position>().cascade();
        DynamicQuery q2 = b2.build();
        REQUIRE(q2.is_ok());
        DynamicArray<EntityId> o2 = collect(q2);
        CHECK(o2.count == 3);
        CHECK(ordered_by_depth(world, o2, ECS::CHILD_OF, false));
        CHECK(index_of(o2, tree.root) == o2.count);
        CHECK(index_of(o2, tree.other_root) == o2.count);
        CHECK(index_of(o2, tree.child) < index_of(o2, tree.grandchild));
        // matches() is unaffected by the order.
        CHECK(q2.matches(tree.child));
        CHECK(q2.matches(tree.grandchild));
        CHECK_FALSE(q2.matches(tree.root));
        o2.free();
        q2.free();
        b2.free();
    }

    SUBCASE("desc() yields the deepest first") {
        QueryBuilder b2 = world.query_build();
        b2.term<Position>().term<Position>().cascade().desc().optional();
        DynamicQuery q2 = b2.build();
        REQUIRE(q2.is_ok());
        CHECK(q2.program.cascade_desc);
        DynamicArray<EntityId> o2 = collect(q2);
        CHECK(o2.count == 5);
        CHECK(ordered_by_depth(world, o2, ECS::CHILD_OF, true));
        CHECK(o2[0] == tree.grandchild);
        CHECK(index_of(o2, tree.child) < index_of(o2, tree.root));
        CHECK(index_of(o2, tree.other_child) < index_of(o2, tree.other_root));
        o2.free();
        q2.free();
        b2.free();
    }

    SUBCASE("a query that narrows nothing sorts the whole world") {
        // Every term optional: no with id, so every non-empty archetype is a
        // candidate, the built-in entities' included.
        QueryBuilder b2 = world.query_build();
        b2.term<Position>().optional().term<Position>().cascade().optional();
        DynamicQuery q2 = b2.build();
        REQUIRE(q2.is_ok());
        DynamicArray<EntityId> o2 = collect(q2);
        CHECK(o2.count >= 5);
        CHECK(ordered_by_depth(world, o2, ECS::CHILD_OF, false));
        CHECK(index_of(o2, tree.root) < index_of(o2, tree.child));
        CHECK(index_of(o2, tree.child) < index_of(o2, tree.grandchild));
        o2.free();
        q2.free();
        b2.free();
    }

    SUBCASE("entities of one archetype come out together, between the depths around them") {
        const EntityId sibling = world.new_entity();
        world.add(sibling, world.pair(ECS::CHILD_OF, tree.root));
        world.set<Position>(sibling, { 4, 0 });
        DynamicArray<EntityId> o2 = collect(query);
        CHECK(o2.count == 6);
        CHECK(ordered_by_depth(world, o2, ECS::CHILD_OF, false));
        const usz first = index_of(o2, tree.child);
        const usz second = index_of(o2, sibling);
        CHECK((second == first + 1 || first == second + 1));
        o2.free();
    }

    query.free();
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/dynamic_query: cascade() sorts an archetype with several parents after its deepest one") {
    World world;
    world.init();

    // a <- b <- c along IS_A, and d derives from both a and c: depth 3.
    const EntityId a = world.new_entity();
    const EntityId b = world.new_entity();
    const EntityId c = world.new_entity();
    const EntityId d = world.new_entity();
    world.set<Position>(d, { 4, 0 });
    world.add(d, world.pair(ECS::IS_A, a));
    world.add(d, world.pair(ECS::IS_A, c));
    world.set<Position>(c, { 3, 0 });
    world.add(c, world.pair(ECS::IS_A, b));
    world.set<Position>(b, { 2, 0 });
    world.add(b, world.pair(ECS::IS_A, a));
    world.set<Position>(a, { 1, 0 });
    REQUIRE(world.depth(d, ECS::IS_A) == 3);

    QueryBuilder builder = world.query_build();
    builder.term<Position>().term<Position>().cascade(ECS::IS_A).optional();
    DynamicQuery query = builder.build();
    REQUIRE(query.is_ok());
    CHECK(query.program.cascade_relation == ECS::ENTITY_LOW(ECS::IS_A));

    DynamicArray<EntityId> order = collect(query);
    REQUIRE(order.count == 4);
    CHECK(order[0] == a);
    CHECK(order[1] == b);
    CHECK(order[2] == c);
    CHECK(order[3] == d);
    order.free();

    query.free();
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/dynamic_query: a cached cascade() query re-sorts only when a depth or the match list changed") {
    World world;
    world.init();
    Tree tree = build_tree(world);

    QueryBuilder builder = world.query_build(QUERY_CACHED);
    builder.term<Position>().term<Position>().cascade().optional();
    DynamicQuery query = builder.build();
    REQUIRE(query.is_ok());

    DynamicArray<EntityId> order = collect(query);
    REQUIRE(query.cache != nullptr);
    QueryScanCache* cache = query.cache;
    CHECK(cache->cascade_relation == ECS::ENTITY_LOW(ECS::CHILD_OF));
    CHECK_FALSE(cache->cascade_desc);
    CHECK_FALSE(cache->order_dirty);
    CHECK(cache->hierarchy_generation == world.hierarchy_generation);
    CHECK(cache->order.count == cache->matches.count);
    // Intermediate archetypes left empty while the tree was built are
    // cached too (see query_scan.hpp), so there are more matches than rows.
    const usz base = cache->matches.count;
    CHECK(base >= 5);
    CHECK(order.count == 5);
    CHECK(ordered_by_depth(world, order, ECS::CHILD_OF, false));
    order.free();

    // The order lists every match once.
    for (usz i = 0; i < cache->matches.count; i++) {
        usz seen = 0;
        for (usz j = 0; j < cache->order.count; j++) {
            seen += cache->order[j] == i ? 1 : 0;
        }
        CHECK(seen == 1);
    }

    SUBCASE("a reparent at the same depth leaves the order alone") {
        // other_child joins child's archetype under root: no depth changed
        // and no archetype came or went, so nothing is re-sorted.
        const u64 generation = world.hierarchy_generation;
        world.add(tree.other_child, world.pair(ECS::CHILD_OF, tree.root));
        CHECK(world.hierarchy_generation == generation);
        CHECK(cache->matches.count == base);
        CHECK_FALSE(cache->order_dirty);
        order = collect(query);
        CHECK_FALSE(cache->order_dirty);
        CHECK(cache->hierarchy_generation == generation);
        CHECK(order.count == 5);
        CHECK(ordered_by_depth(world, order, ECS::CHILD_OF, false));
        order.free();
    }

    SUBCASE("a reparent that changes depths re-sorts on the next run") {
        // child (and so grandchild) go under other_child: depths 2 and 3.
        const u64 generation = world.hierarchy_generation;
        world.add(tree.child, world.pair(ECS::CHILD_OF, tree.other_child));
        CHECK(world.hierarchy_generation != generation);
        REQUIRE(world.depth(tree.grandchild) == 3);

        order = collect(query);
        CHECK_FALSE(cache->order_dirty);
        CHECK(cache->hierarchy_generation == world.hierarchy_generation);
        CHECK(order.count == 5);
        CHECK(ordered_by_depth(world, order, ECS::CHILD_OF, false));
        CHECK(index_of(order, tree.other_child) < index_of(order, tree.child));
        CHECK(order.last() == tree.grandchild);
        order.free();

        // Nothing changed since: the next run finds the order current.
        order = collect(query);
        CHECK_FALSE(cache->order_dirty);
        CHECK(cache->hierarchy_generation == world.hierarchy_generation);
        order.free();
    }

    SUBCASE("a new archetype lands at its depth") {
        const EntityId leaf = world.new_entity();
        world.add(leaf, world.pair(ECS::CHILD_OF, tree.grandchild));
        world.set<Position>(leaf, { 5, 0 });
        world.set<Health>(leaf, { 1 });
        CHECK(cache->matches.count > base);
        CHECK(cache->order_dirty);

        order = collect(query);
        CHECK_FALSE(cache->order_dirty);
        CHECK(cache->order.count == cache->matches.count);
        CHECK(order.count == 6);
        CHECK(ordered_by_depth(world, order, ECS::CHILD_OF, false));
        CHECK(order.last() == leaf);
        order.free();

        // Propagation through the cache sees parents first too.
        query.each<Position, Position*>([](EntityId, Position& own, Position* parent) {
            if (parent != nullptr) {
                own.y = parent->y + 1;
            }
        });
        CHECK(world.get<Position>(leaf)->y == 3);
    }

    SUBCASE("desc() through the cache") {
        QueryBuilder b2 = world.query_build(QUERY_CACHED);
        b2.term<Position>().term<Position>().cascade().desc().optional();
        DynamicQuery q2 = b2.build();
        REQUIRE(q2.is_ok());
        DynamicArray<EntityId> o2 = collect(q2);
        REQUIRE(q2.cache != nullptr);
        CHECK(q2.cache->cascade_desc);
        CHECK(o2.count == 5);
        CHECK(ordered_by_depth(world, o2, ECS::CHILD_OF, true));
        CHECK(o2[0] == tree.grandchild);
        o2.free();
        q2.free();
        b2.free();
    }

    SUBCASE("the VM refuses a cache without the program's order") {
        // A cache built from the same ids but unordered is not this
        // program's: it is ignored and the walk still comes out sorted.
        TemporalAllocator temp = TemporalAllocator::create();
        QueryTerm* terms = temp.allocate_array<QueryTerm>(query.program.this_term_count + 1);
        for (usz i = 0; i < query.program.this_term_count; i++) {
            terms[i] = query.program.terms[query.program.this_terms[i]];
        }
        QueryScanCache* wrong = QUERY_SCAN::create_cache(&world, query.program.with_ids, query.program.with_count,
            query.program.without_ids, query.program.without_count, terms, query.program.this_term_count, world.allocator);
        CHECK(wrong->cascade_relation == 0);
        {
            TemporalAllocator walk = TemporalAllocator::create();
            QueryIter it = QUERY_VM::begin(&world, &query.program, wrong, &walk);
            DynamicArray<EntityId> o2;
            while (it.next(&it)) {
                for (usz row = 0; row < it.count; row++) {
                    o2.push(it.entities[row]);
                }
            }
            CHECK(o2.count == 5);
            CHECK(ordered_by_depth(world, o2, ECS::CHILD_OF, false));
            o2.free();
        }
        QUERY_SCAN::destroy_cache(wrong);
    }

    query.free();
    builder.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/dynamic_query: cascade() rejects the shapes it cannot order") {
    World world;
    world.init();
    const EntityId someone = world.new_entity();
    world.set<Position>(someone, { 1, 0 });

    SUBCASE("a relation that is not TRAVERSABLE") {
        const EntityId relation = world.new_entity();
        QueryBuilder builder = world.query_build();
        builder.term<Position>().term<Position>().cascade(relation).optional();
        DynamicQuery query = builder.build();
        CHECK_FALSE(query.is_ok());
        query.free();
        builder.free();
    }

    SUBCASE("two cascade() terms") {
        QueryBuilder builder = world.query_build();
        builder.term<Position>().cascade().optional().term<Velocity>().cascade().optional();
        DynamicQuery query = builder.build();
        CHECK_FALSE(query.is_ok());
        query.free();
        builder.free();
    }

    SUBCASE("cascade() on another source") {
        QueryBuilder builder = world.query_build();
        builder.term<Position>().term<Position>().src(someone).cascade();
        DynamicQuery query = builder.build();
        CHECK_FALSE(query.is_ok());
        query.free();
        builder.free();
    }

    SUBCASE("cascade() on a without() term") {
        QueryBuilder builder = world.query_build();
        builder.term<Position>().without<Velocity>().cascade();
        DynamicQuery query = builder.build();
        CHECK_FALSE(query.is_ok());
        query.free();
        builder.free();
    }

    SUBCASE("cascade() in an or-chain") {
        QueryBuilder builder = world.query_build();
        builder.term<Position>().with<TagA>().bor().with<TagB>().cascade();
        DynamicQuery query = builder.build();
        CHECK_FALSE(query.is_ok());
        query.free();
        builder.free();
    }

    SUBCASE("desc() without cascade()") {
        QueryBuilder builder = world.query_build();
        builder.term<Position>().desc();
        DynamicQuery query = builder.build();
        CHECK_FALSE(query.is_ok());
        query.free();
        builder.free();
    }

    SUBCASE("cascade() and desc() before any term") {
        QueryBuilder builder = world.query_build();
        builder.cascade().desc();
        CHECK(builder.terms.count == 0);
        builder.free();
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/hierarchy: order_by_depth is a stable counting sort over archetype depths") {
    World world;
    world.init();

    // Four archetypes at depths 0, 1, 2 and 0 again (TagB tells the two
    // roots apart), created deepest first.
    const EntityId root = world.new_entity();
    const EntityId child = world.new_entity();
    const EntityId grandchild = world.new_entity();
    const EntityId other = world.new_entity();
    world.add(grandchild, world.pair(ECS::CHILD_OF, child));
    world.set<Position>(grandchild, { 3, 0 });
    world.add(child, world.pair(ECS::CHILD_OF, root));
    world.set<Position>(child, { 2, 0 });
    world.set<Position>(root, { 1, 0 });
    world.set<Position>(other, { 1, 0 });
    world.add<TagB>(other);

    auto archetype_of = [&](const EntityId entity) {
        const EntityRecord* record = world.entity_index.get_record_alive(entity);
        REQUIRE(record != nullptr);
        return record->archetype;
    };
    Archetype* archetypes[4] = { archetype_of(grandchild), archetype_of(child), archetype_of(root), archetype_of(other) };
    u32 order[4];

    HIERARCHY::order_by_depth(&world, archetypes, 4, ECS::CHILD_OF, false, order);
    // Depth 0 first in given order (root, other), then child, then grandchild.
    CHECK(order[0] == 2);
    CHECK(order[1] == 3);
    CHECK(order[2] == 1);
    CHECK(order[3] == 0);
    // The scratch is the sort's own: nothing is left on the arena.
    CHECK_ARENA_CLEAN();

    HIERARCHY::order_by_depth(&world, archetypes, 4, ECS::CHILD_OF, true, order);
    CHECK(order[0] == 0);
    CHECK(order[1] == 1);
    CHECK(order[2] == 2);
    CHECK(order[3] == 3);

    // Nothing to sort is fine.
    HIERARCHY::order_by_depth(&world, archetypes, 0, ECS::CHILD_OF, false, order);

    world.free();
    CHECK_ARENA_CLEAN();
}
