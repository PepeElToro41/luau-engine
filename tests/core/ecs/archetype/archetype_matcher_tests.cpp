#include "support/test_support.hpp"

#include "engine/ecs/archetype/archetype.hpp"
#include "engine/ecs/archetype/archetype_matcher.hpp"
#include "engine/ecs/archetype/archetype_signature.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/entity_index.hpp"
#include "engine/memory/heap_allocator.hpp"

namespace {

// Ids that look like real ones without a world: a few components, two tag
// entities and relations past ECS::REST.
constexpr Id COMPONENT_A = 3;
constexpr Id COMPONENT_B = 70;
constexpr Id COMPONENT_C = 200;
constexpr Id TAG_ENTITY = ECS::REST + 1;
constexpr Id LIKES = ECS::REST + 2;
constexpr Id EATS = ECS::REST + 3;
constexpr Id APPLE = ECS::REST + 4;
constexpr Id PEAR = ECS::REST + 5;

constexpr Id LIKES_APPLE = ECS::PAIR(LIKES, APPLE);
constexpr Id LIKES_PEAR = ECS::PAIR(LIKES, PEAR);
constexpr Id EATS_APPLE = ECS::PAIR(EATS, APPLE);

static_assert(LIKES_APPLE < LIKES_PEAR, "pairs with the same relation sort by target");
static_assert(LIKES_PEAR < EATS_APPLE, "pairs sort by relation first");

// Matches `type` through a freshly built signature, which is what the world
// does for every archetype.
bool matches_type(const ArchetypeMatcher& matcher, const ArchetypeType& type) {
    const ArchetypeSignature signature = ArchetypeSignature::build(type.ids, type.id_count);
    return matcher.matches(signature, type);
}

Archetype* archetype_of(World& world, const EntityId entity) {
    EntityRecord* record = world.entity_index.get_record_alive(entity);
    REQUIRE(record != nullptr);
    REQUIRE(record->archetype != nullptr);
    return record->archetype;
}

} // namespace

// --- create / free -----------------------------------------------------------

TEST_CASE("ecs/archetype_matcher: an empty matcher matches every type") {
    ArchetypeMatcher matcher = ArchetypeMatcher::create(MEMORY::heap_allocator(), nullptr, 0, nullptr, 0);
    CHECK(matcher.with_count == 0);
    CHECK(matcher.without_count == 0);
    CHECK(matcher.with_ids == nullptr);
    CHECK(matcher.without_ids == nullptr);
    CHECK(matcher.with_mask.is_empty());
    CHECK(matcher.with_bloom.is_empty());

    CHECK(matches_type(matcher, ArchetypeType()));
    u64 ids[] = { COMPONENT_A, TAG_ENTITY, LIKES_APPLE };
    CHECK(matches_type(matcher, ArchetypeType(ids, 3)));

    matcher.free();
    CHECK(matcher.with_ids == nullptr);
    CHECK(matcher.without_count == 0);
    // Freeing twice is harmless.
    matcher.free();
}

TEST_CASE("ecs/archetype_matcher: create splits ids between mask and list") {
    const Id with[] = { LIKES_APPLE, COMPONENT_B, TAG_ENTITY, COMPONENT_A, 0 };
    const Id without[] = { COMPONENT_C, EATS_APPLE };
    ArchetypeMatcher matcher = ArchetypeMatcher::create(MEMORY::heap_allocator(), with, 5, without, 2);

    SUBCASE("component ids go into the masks only") {
        CHECK(matcher.with_mask.has(COMPONENT_A));
        CHECK(matcher.with_mask.has(COMPONENT_B));
        CHECK_FALSE(matcher.with_mask.has(COMPONENT_C));
        CHECK(matcher.without_mask.has(COMPONENT_C));
        CHECK_FALSE(matcher.without_mask.has(COMPONENT_A));
    }
    SUBCASE("everything else goes into a sorted list and the bloom") {
        REQUIRE(matcher.with_count == 2);
        CHECK(matcher.with_ids[0] == TAG_ENTITY);
        CHECK(matcher.with_ids[1] == LIKES_APPLE);
        REQUIRE(matcher.without_count == 1);
        CHECK(matcher.without_ids[0] == EATS_APPLE);

        BloomFilter tag_query;
        tag_query.add(TAG_ENTITY);
        CHECK(matcher.with_bloom.test(tag_query));
        BloomFilter pair_query;
        pair_query.add(EATS_APPLE);
        CHECK(matcher.without_bloom.test(pair_query));
    }
    SUBCASE("ids of 0 are ignored") {
        const Id zeros[] = { 0, 0 };
        ArchetypeMatcher only_zeros = ArchetypeMatcher::create(MEMORY::heap_allocator(), zeros, 2, zeros, 2);
        CHECK(only_zeros.with_count == 0);
        CHECK(only_zeros.with_ids == nullptr);
        CHECK(only_zeros.with_mask.is_empty());
        CHECK(matches_type(only_zeros, ArchetypeType()));
        only_zeros.free();
    }

    matcher.free();
}

TEST_CASE("ecs/archetype_matcher: ANY is folded to WILDCARD and class-wide patterns skip the bloom") {
    const Id with[] = { ECS::PAIR(LIKES, ECS::ANY), ECS::ANY, ECS::PAIR(ECS::ANY, ECS::ANY) };
    ArchetypeMatcher matcher = ArchetypeMatcher::create(MEMORY::heap_allocator(), with, 3, nullptr, 0);
    REQUIRE(matcher.with_count == 3);

    bool saw_wildcard = false;
    bool saw_likes_wildcard = false;
    bool saw_pair_wildcard = false;
    for (usz i = 0; i < matcher.with_count; i++) {
        const Id id = matcher.with_ids[i];
        saw_wildcard |= id == ECS::WILDCARD;
        saw_likes_wildcard |= id == ECS::PAIR(LIKES, ECS::WILDCARD);
        saw_pair_wildcard |= id == ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD);
        if (i > 0) {
            CHECK(matcher.with_ids[i - 1] < id);
        }
    }
    CHECK(saw_wildcard);
    CHECK(saw_likes_wildcard);
    CHECK(saw_pair_wildcard);

    // Only (Likes, *) can be tested through the bloom; the other two would
    // reject every archetype, so the bloom holds exactly that one pattern.
    BloomFilter expected;
    expected.add(ECS::PAIR(LIKES, ECS::WILDCARD));
    CHECK(matcher.with_bloom.bitset == expected.bitset);

    matcher.free();
}

// --- type_matches ------------------------------------------------------------

TEST_CASE("ecs/archetype_matcher: type_matches finds plain ids by binary search") {
    u64 ids[] = { COMPONENT_A, COMPONENT_B, TAG_ENTITY, LIKES_APPLE, LIKES_PEAR, EATS_APPLE };
    const ArchetypeType type(ids, 6);

    CHECK(ArchetypeMatcher::type_matches(type, COMPONENT_A));
    CHECK(ArchetypeMatcher::type_matches(type, COMPONENT_B));
    CHECK(ArchetypeMatcher::type_matches(type, TAG_ENTITY));
    CHECK(ArchetypeMatcher::type_matches(type, EATS_APPLE));
    CHECK_FALSE(ArchetypeMatcher::type_matches(type, COMPONENT_A + 1));
    CHECK_FALSE(ArchetypeMatcher::type_matches(type, COMPONENT_C));
    CHECK_FALSE(ArchetypeMatcher::type_matches(type, ECS::REST));
    CHECK_FALSE(ArchetypeMatcher::type_matches(type, TAG_ENTITY | (1ull << ECS::ENTITY_BITS)));

    SUBCASE("an empty type matches nothing concrete") {
        const ArchetypeType empty;
        CHECK_FALSE(ArchetypeMatcher::type_matches(empty, COMPONENT_A));
        CHECK_FALSE(ArchetypeMatcher::type_matches(empty, LIKES_APPLE));
    }
    SUBCASE("a single-id type") {
        u64 one[] = { COMPONENT_C };
        const ArchetypeType single(one, 1);
        CHECK(ArchetypeMatcher::type_matches(single, COMPONENT_C));
        CHECK_FALSE(ArchetypeMatcher::type_matches(single, COMPONENT_C - 1));
        CHECK_FALSE(ArchetypeMatcher::type_matches(single, COMPONENT_C + 1));
    }
}

TEST_CASE("ecs/archetype_matcher: type_matches plain wildcard needs any id at all") {
    u64 ids[] = { COMPONENT_A };
    const ArchetypeType one(ids, 1);
    const ArchetypeType empty;

    CHECK(ArchetypeMatcher::type_matches(one, ECS::WILDCARD));
    CHECK(ArchetypeMatcher::type_matches(one, ECS::ANY));
    CHECK_FALSE(ArchetypeMatcher::type_matches(empty, ECS::WILDCARD));
    CHECK_FALSE(ArchetypeMatcher::type_matches(empty, ECS::ANY));

    u64 pair_only[] = { LIKES_APPLE };
    CHECK(ArchetypeMatcher::type_matches(ArchetypeType(pair_only, 1), ECS::WILDCARD));
}

TEST_CASE("ecs/archetype_matcher: type_matches pair patterns") {
    u64 ids[] = { COMPONENT_A, TAG_ENTITY, LIKES_APPLE, LIKES_PEAR, EATS_APPLE };
    const ArchetypeType type(ids, 5);

    SUBCASE("a concrete pair") {
        CHECK(ArchetypeMatcher::type_matches(type, LIKES_APPLE));
        CHECK(ArchetypeMatcher::type_matches(type, LIKES_PEAR));
        CHECK(ArchetypeMatcher::type_matches(type, EATS_APPLE));
        CHECK_FALSE(ArchetypeMatcher::type_matches(type, ECS::PAIR(EATS, PEAR)));
        CHECK_FALSE(ArchetypeMatcher::type_matches(type, ECS::PAIR(APPLE, LIKES)));
    }
    SUBCASE("(R, *) matches any target of R") {
        CHECK(ArchetypeMatcher::type_matches(type, ECS::PAIR(LIKES, ECS::WILDCARD)));
        CHECK(ArchetypeMatcher::type_matches(type, ECS::PAIR(EATS, ECS::ANY)));
        CHECK_FALSE(ArchetypeMatcher::type_matches(type, ECS::PAIR(APPLE, ECS::WILDCARD)));
        CHECK_FALSE(ArchetypeMatcher::type_matches(type, ECS::PAIR(TAG_ENTITY, ECS::WILDCARD)));
    }
    SUBCASE("(*, T) matches any relation to T") {
        CHECK(ArchetypeMatcher::type_matches(type, ECS::PAIR(ECS::WILDCARD, APPLE)));
        CHECK(ArchetypeMatcher::type_matches(type, ECS::PAIR(ECS::ANY, PEAR)));
        CHECK_FALSE(ArchetypeMatcher::type_matches(type, ECS::PAIR(ECS::WILDCARD, LIKES)));
        CHECK_FALSE(ArchetypeMatcher::type_matches(type, ECS::PAIR(ECS::WILDCARD, TAG_ENTITY)));
    }
    SUBCASE("(*, *) matches any pair and only pairs") {
        CHECK(ArchetypeMatcher::type_matches(type, ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD)));
        u64 plain[] = { COMPONENT_A, TAG_ENTITY };
        CHECK_FALSE(ArchetypeMatcher::type_matches(ArchetypeType(plain, 2), ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD)));
        CHECK_FALSE(ArchetypeMatcher::type_matches(ArchetypeType(), ECS::PAIR(ECS::ANY, ECS::ANY)));
    }
    SUBCASE("a pair pattern never matches a plain id of the same value") {
        u64 plain[] = { LIKES, APPLE };
        const ArchetypeType plain_type(plain, 2);
        CHECK_FALSE(ArchetypeMatcher::type_matches(plain_type, ECS::PAIR(LIKES, ECS::WILDCARD)));
        CHECK_FALSE(ArchetypeMatcher::type_matches(plain_type, ECS::PAIR(ECS::WILDCARD, APPLE)));
        CHECK_FALSE(ArchetypeMatcher::type_matches(plain_type, LIKES_APPLE));
    }
    SUBCASE("(R, *) when R sorts after every held relation") {
        u64 only_likes[] = { LIKES_APPLE, LIKES_PEAR };
        const ArchetypeType likes_type(only_likes, 2);
        CHECK_FALSE(ArchetypeMatcher::type_matches(likes_type, ECS::PAIR(EATS, ECS::WILDCARD)));
        CHECK(ArchetypeMatcher::type_matches(likes_type, ECS::PAIR(LIKES, ECS::WILDCARD)));
    }
}

// --- matches(signature, type) ------------------------------------------------

TEST_CASE("ecs/archetype_matcher: with requires every id") {
    u64 ab_ids[] = { COMPONENT_A, COMPONENT_B };
    u64 a_ids[] = { COMPONENT_A };
    u64 abt_ids[] = { COMPONENT_A, COMPONENT_B, TAG_ENTITY };
    const ArchetypeType ab(ab_ids, 2);
    const ArchetypeType a(a_ids, 1);
    const ArchetypeType abt(abt_ids, 3);

    SUBCASE("components only") {
        const Id with[] = { COMPONENT_A, COMPONENT_B };
        ArchetypeMatcher matcher = ArchetypeMatcher::create(MEMORY::heap_allocator(), with, 2, nullptr, 0);
        CHECK(matches_type(matcher, ab));
        CHECK(matches_type(matcher, abt));
        CHECK_FALSE(matches_type(matcher, a));
        CHECK_FALSE(matches_type(matcher, ArchetypeType()));
        matcher.free();
    }
    SUBCASE("a component and a tag entity") {
        const Id with[] = { COMPONENT_A, TAG_ENTITY };
        ArchetypeMatcher matcher = ArchetypeMatcher::create(MEMORY::heap_allocator(), with, 2, nullptr, 0);
        CHECK(matches_type(matcher, abt));
        CHECK_FALSE(matches_type(matcher, ab));
        CHECK_FALSE(matches_type(matcher, a));
        matcher.free();
    }
    SUBCASE("a plain wildcard excludes only the empty type") {
        const Id with[] = { ECS::WILDCARD };
        ArchetypeMatcher matcher = ArchetypeMatcher::create(MEMORY::heap_allocator(), with, 1, nullptr, 0);
        CHECK(matches_type(matcher, a));
        CHECK(matches_type(matcher, abt));
        CHECK_FALSE(matches_type(matcher, ArchetypeType()));
        matcher.free();
    }
}

TEST_CASE("ecs/archetype_matcher: without rejects any id") {
    u64 ab_ids[] = { COMPONENT_A, COMPONENT_B };
    u64 at_ids[] = { COMPONENT_A, TAG_ENTITY };
    u64 ap_ids[] = { COMPONENT_A, LIKES_APPLE };
    const ArchetypeType ab(ab_ids, 2);
    const ArchetypeType at(at_ids, 2);
    const ArchetypeType ap(ap_ids, 2);

    SUBCASE("a component") {
        const Id with[] = { COMPONENT_A };
        const Id without[] = { COMPONENT_B };
        ArchetypeMatcher matcher = ArchetypeMatcher::create(MEMORY::heap_allocator(), with, 1, without, 1);
        CHECK_FALSE(matches_type(matcher, ab));
        CHECK(matches_type(matcher, at));
        CHECK(matches_type(matcher, ap));
        matcher.free();
    }
    SUBCASE("a tag entity") {
        const Id without[] = { TAG_ENTITY };
        ArchetypeMatcher matcher = ArchetypeMatcher::create(MEMORY::heap_allocator(), nullptr, 0, without, 1);
        CHECK(matches_type(matcher, ab));
        CHECK_FALSE(matches_type(matcher, at));
        CHECK(matches_type(matcher, ArchetypeType()));
        matcher.free();
    }
    SUBCASE("a wildcard pair pattern") {
        const Id without[] = { ECS::PAIR(LIKES, ECS::WILDCARD) };
        ArchetypeMatcher matcher = ArchetypeMatcher::create(MEMORY::heap_allocator(), nullptr, 0, without, 1);
        CHECK(matches_type(matcher, ab));
        CHECK(matches_type(matcher, at));
        CHECK_FALSE(matches_type(matcher, ap));
        matcher.free();
    }
    SUBCASE("(*, *) rejects every type holding a pair") {
        const Id without[] = { ECS::PAIR(ECS::ANY, ECS::ANY) };
        ArchetypeMatcher matcher = ArchetypeMatcher::create(MEMORY::heap_allocator(), nullptr, 0, without, 1);
        CHECK(matches_type(matcher, ab));
        CHECK(matches_type(matcher, at));
        CHECK_FALSE(matches_type(matcher, ap));
        CHECK(matches_type(matcher, ArchetypeType()));
        matcher.free();
    }
    SUBCASE("a plain wildcard leaves only the empty type") {
        const Id without[] = { ECS::WILDCARD };
        ArchetypeMatcher matcher = ArchetypeMatcher::create(MEMORY::heap_allocator(), nullptr, 0, without, 1);
        CHECK(matches_type(matcher, ArchetypeType()));
        CHECK_FALSE(matches_type(matcher, ab));
        CHECK_FALSE(matches_type(matcher, ap));
        matcher.free();
    }
}

TEST_CASE("ecs/archetype_matcher: with and without combine on pair patterns") {
    u64 likes_ids[] = { COMPONENT_A, LIKES_APPLE, LIKES_PEAR };
    u64 eats_ids[] = { COMPONENT_A, EATS_APPLE };
    u64 both_ids[] = { COMPONENT_A, LIKES_PEAR, EATS_APPLE };
    const ArchetypeType likes(likes_ids, 3);
    const ArchetypeType eats(eats_ids, 2);
    const ArchetypeType both(both_ids, 3);

    SUBCASE("(Likes, *) without (Eats, *)") {
        const Id with[] = { ECS::PAIR(LIKES, ECS::WILDCARD) };
        const Id without[] = { ECS::PAIR(EATS, ECS::WILDCARD) };
        ArchetypeMatcher matcher = ArchetypeMatcher::create(MEMORY::heap_allocator(), with, 1, without, 1);
        CHECK(matches_type(matcher, likes));
        CHECK_FALSE(matches_type(matcher, eats));
        CHECK_FALSE(matches_type(matcher, both));
        matcher.free();
    }
    SUBCASE("(*, Apple) without (Likes, Apple)") {
        const Id with[] = { ECS::PAIR(ECS::WILDCARD, APPLE) };
        const Id without[] = { LIKES_APPLE };
        ArchetypeMatcher matcher = ArchetypeMatcher::create(MEMORY::heap_allocator(), with, 1, without, 1);
        CHECK_FALSE(matches_type(matcher, likes));
        CHECK(matches_type(matcher, eats));
        CHECK(matches_type(matcher, both));
        matcher.free();
    }
    SUBCASE("two concrete pairs with a component") {
        const Id with[] = { COMPONENT_A, LIKES_PEAR, EATS_APPLE };
        ArchetypeMatcher matcher = ArchetypeMatcher::create(MEMORY::heap_allocator(), with, 3, nullptr, 0);
        CHECK(matches_type(matcher, both));
        CHECK_FALSE(matches_type(matcher, likes));
        CHECK_FALSE(matches_type(matcher, eats));
        matcher.free();
    }
}

// --- matches(Archetype*) with a world ----------------------------------------

TEST_CASE("ecs/archetype_matcher: matches real archetypes from a world") {
    World world;
    world.init();
    const Id position = world.component<Position>();
    const Id velocity = world.component<Velocity>();
    const Id tag_a = world.tag<TagA>();
    const EntityId apple = world.new_entity();
    const EntityId pear = world.new_entity();
    const Id likes_apple = world.pair<Likes>(apple);
    const Id likes_pear = world.pair<Likes>(pear);
    const Id eats_apple = world.pair<Eats>(apple);
    const Id likes = world.tag<Likes>();
    const Id eats = world.tag<Eats>();

    // One entity per archetype so each table is reachable by name.
    const EntityId only_position = world.new_entity();
    world.set(only_position, Position { });
    const EntityId moving = world.new_entity();
    world.set(moving, Position { });
    world.set(moving, Velocity { });
    const EntityId tagged = world.new_entity();
    world.set(tagged, Position { });
    world.add(tagged, tag_a);
    const EntityId fan = world.new_entity();
    world.add(fan, likes_apple);
    world.add(fan, likes_pear);
    const EntityId eater = world.new_entity();
    world.set(eater, Position { });
    world.add(eater, eats_apple);

    Archetype* root = world.root_archetype;
    Archetype* position_archetype = archetype_of(world, only_position);
    Archetype* moving_archetype = archetype_of(world, moving);
    Archetype* tagged_archetype = archetype_of(world, tagged);
    Archetype* fan_archetype = archetype_of(world, fan);
    Archetype* eater_archetype = archetype_of(world, eater);
    REQUIRE(position_archetype != root);
    REQUIRE(moving_archetype != position_archetype);
    REQUIRE(tagged_archetype != position_archetype);
    REQUIRE(fan_archetype != root);
    REQUIRE(eater_archetype != position_archetype);

    SUBCASE("a component selects every archetype holding it") {
        const Id with[] = { position };
        ArchetypeMatcher matcher = ArchetypeMatcher::create(world.allocator, with, 1, nullptr, 0);
        CHECK(matcher.matches(position_archetype));
        CHECK(matcher.matches(moving_archetype));
        CHECK(matcher.matches(tagged_archetype));
        CHECK(matcher.matches(eater_archetype));
        CHECK_FALSE(matcher.matches(fan_archetype));
        CHECK_FALSE(matcher.matches(root));
        matcher.free();
    }
    SUBCASE("without excludes a component or a tag") {
        const Id with[] = { position };
        const Id without[] = { velocity, tag_a };
        ArchetypeMatcher matcher = ArchetypeMatcher::create(world.allocator, with, 1, without, 2);
        CHECK(matcher.matches(position_archetype));
        CHECK(matcher.matches(eater_archetype));
        CHECK_FALSE(matcher.matches(moving_archetype));
        CHECK_FALSE(matcher.matches(tagged_archetype));
        matcher.free();
    }
    SUBCASE("a wildcard pair matches any target") {
        const Id with[] = { ECS::PAIR(likes, ECS::WILDCARD) };
        ArchetypeMatcher matcher = ArchetypeMatcher::create(world.allocator, with, 1, nullptr, 0);
        CHECK(matcher.matches(fan_archetype));
        CHECK_FALSE(matcher.matches(eater_archetype));
        CHECK_FALSE(matcher.matches(position_archetype));
        CHECK_FALSE(matcher.matches(root));
        matcher.free();
    }
    SUBCASE("a wildcard relation matches any relation to the target") {
        const Id with[] = { ECS::PAIR(ECS::ANY, apple) };
        ArchetypeMatcher matcher = ArchetypeMatcher::create(world.allocator, with, 1, nullptr, 0);
        CHECK(matcher.matches(fan_archetype));
        CHECK(matcher.matches(eater_archetype));
        CHECK_FALSE(matcher.matches(moving_archetype));

        const Id pear_with[] = { ECS::PAIR(ECS::WILDCARD, pear) };
        ArchetypeMatcher pear_matcher = ArchetypeMatcher::create(world.allocator, pear_with, 1, nullptr, 0);
        CHECK(pear_matcher.matches(fan_archetype));
        CHECK_FALSE(pear_matcher.matches(eater_archetype));
        pear_matcher.free();
        matcher.free();
    }
    SUBCASE("a concrete pair matches exactly") {
        const Id with[] = { likes_pear };
        ArchetypeMatcher matcher = ArchetypeMatcher::create(world.allocator, with, 1, nullptr, 0);
        CHECK(matcher.matches(fan_archetype));
        CHECK_FALSE(matcher.matches(eater_archetype));

        const Id without[] = { likes_apple };
        ArchetypeMatcher excluding = ArchetypeMatcher::create(world.allocator, with, 1, without, 1);
        CHECK_FALSE(excluding.matches(fan_archetype));
        excluding.free();
        matcher.free();
    }
    SUBCASE("(*, *) splits pair holders from the rest") {
        const Id any_pair[] = { ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD) };
        ArchetypeMatcher with_pairs = ArchetypeMatcher::create(world.allocator, any_pair, 1, nullptr, 0);
        ArchetypeMatcher without_pairs = ArchetypeMatcher::create(world.allocator, nullptr, 0, any_pair, 1);
        CHECK(with_pairs.matches(fan_archetype));
        CHECK(with_pairs.matches(eater_archetype));
        CHECK_FALSE(with_pairs.matches(moving_archetype));
        CHECK_FALSE(with_pairs.matches(root));
        CHECK_FALSE(without_pairs.matches(fan_archetype));
        CHECK_FALSE(without_pairs.matches(eater_archetype));
        CHECK(without_pairs.matches(moving_archetype));
        CHECK(without_pairs.matches(root));
        with_pairs.free();
        without_pairs.free();
    }
    SUBCASE("matches(Archetype*) agrees with matches(signature, type)") {
        const Id with[] = { position, ECS::PAIR(eats, ECS::WILDCARD) };
        ArchetypeMatcher matcher = ArchetypeMatcher::create(world.allocator, with, 2, nullptr, 0);
        Archetype* all[] = { root, position_archetype, moving_archetype, tagged_archetype, fan_archetype, eater_archetype };
        for (Archetype* archetype : all) {
            CHECK(matcher.matches(archetype) == matcher.matches(archetype->signature, archetype->type));
        }
        CHECK(matcher.matches(eater_archetype));
        CHECK_FALSE(matcher.matches(fan_archetype));
        matcher.free();
    }
    SUBCASE("the empty matcher also takes the root") {
        ArchetypeMatcher matcher = ArchetypeMatcher::create(world.allocator, nullptr, 0, nullptr, 0);
        CHECK(matcher.matches(root));
        CHECK(matcher.matches(fan_archetype));
        matcher.free();
    }

    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/archetype_matcher: a matcher built before an archetype exists still matches it") {
    World world;
    world.init();
    const Id health = world.component<Health>();
    const Id with[] = { health, ECS::PAIR(ECS::CHILD_OF, ECS::WILDCARD) };
    ArchetypeMatcher matcher = ArchetypeMatcher::create(world.allocator, with, 2, nullptr, 0);

    const EntityId parent = world.new_entity();
    const EntityId child = world.new_entity();
    world.set(child, Health { 1 });
    CHECK_FALSE(matcher.matches(archetype_of(world, child)));
    world.add(child, ECS::PAIR(ECS::CHILD_OF, parent));
    CHECK(matcher.matches(archetype_of(world, child)));
    CHECK_FALSE(matcher.matches(archetype_of(world, parent)));

    matcher.free();
    world.free();
    CHECK_ARENA_CLEAN();
}
