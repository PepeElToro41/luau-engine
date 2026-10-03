#include "support/test_support.hpp"

#include "engine/ecs/ecs.hpp"

#include <type_traits>

// Most of ecs.hpp is constexpr, so the layout contract is pinned with
// static_asserts; the CHECKs below exercise the same helpers with runtime
// values so a failure shows up in the doctest report as well.

namespace {

constexpr EntityId GENERATION_ONE = 1ull << ECS::ENTITY_BITS;
constexpr EntityId WITH_GENERATION_3 = 42 | (3ull << ECS::ENTITY_BITS);
constexpr EntityId WITH_GENERATION_7 = 7 | (7ull << ECS::ENTITY_BITS);

// --- Layout ------------------------------------------------------------------

static_assert(ECS::ENTITY_SIZE == (1ull << 32));
static_assert(ECS::ENTITY_MASK == 0xFFFFFFFFull);
static_assert(ECS::ID_FLAG_PAIR == (1ull << 63));
static_assert((ECS::ID_FLAGS_MASK & ECS::ID_FLAG_PAIR) == ECS::ID_FLAG_PAIR, "the pair flag is one of the reserved flag bits");
static_assert((ECS::ID_FLAGS_MASK & ECS::ID_COMPONENT_MASK) == 0, "component bits and flag bits do not overlap");
static_assert(ECS::REST == ECS::IS_A + 1);
static_assert(ECS::WILDCARD > ECS::MAX_COMPONENT_ID, "built-ins start after the component range");
static_assert(ECS::REST < (1ull << 28), "built-in ids fit in a pair's 28-bit relation slot");

// --- ENTITY_LOW --------------------------------------------------------------

static_assert(ECS::ENTITY_LOW(WITH_GENERATION_3) == 42);
static_assert(ECS::ENTITY_LOW(42) == 42);
static_assert(ECS::ENTITY_LOW(0) == 0);

// --- PAIR / IS_PAIR / PAIR_FIRST / PAIR_SECOND -------------------------------

constexpr Id SAMPLE_PAIR = ECS::PAIR(WITH_GENERATION_3, WITH_GENERATION_7);

static_assert(ECS::IS_PAIR(SAMPLE_PAIR));
static_assert(!ECS::IS_PAIR(42));
static_assert(!ECS::IS_PAIR(WITH_GENERATION_3), "a generation never sets the pair flag");
static_assert(!ECS::IS_PAIR(ECS::WILDCARD));
static_assert(ECS::PAIR_FIRST(SAMPLE_PAIR) == 42, "the relation's generation is stripped");
static_assert(ECS::PAIR_SECOND(SAMPLE_PAIR) == 7, "the target's generation is stripped");
static_assert(ECS::PAIR(42, 7) == SAMPLE_PAIR, "generations do not change the pair id");
static_assert(ECS::PAIR(42, 7) != ECS::PAIR(7, 42), "pairs are ordered");

// --- Wildcards ---------------------------------------------------------------

static_assert(ECS::IS_WILDCARD(ECS::WILDCARD));
static_assert(ECS::IS_WILDCARD(ECS::ANY));
static_assert(!ECS::IS_WILDCARD(ECS::THIS));
static_assert(!ECS::IS_WILDCARD(1));
static_assert(!ECS::IS_WILDCARD(0));

static_assert(ECS::PAIR_HAS_WILDCARD(ECS::PAIR(42, ECS::WILDCARD)));
static_assert(ECS::PAIR_HAS_WILDCARD(ECS::PAIR(ECS::WILDCARD, 42)));
static_assert(ECS::PAIR_HAS_WILDCARD(ECS::PAIR(42, ECS::ANY)));
static_assert(ECS::PAIR_HAS_WILDCARD(ECS::PAIR(ECS::ANY, ECS::ANY)));
static_assert(!ECS::PAIR_HAS_WILDCARD(ECS::PAIR(42, 7)));
static_assert(!ECS::PAIR_HAS_WILDCARD(ECS::PAIR(ECS::THIS, 7)), "THIS is a query variable, not a wildcard");

// --- FOLD_ANY ----------------------------------------------------------------

static_assert(ECS::FOLD_ANY(ECS::ANY) == ECS::WILDCARD);
static_assert(ECS::FOLD_ANY(ECS::WILDCARD) == ECS::WILDCARD);
static_assert(ECS::FOLD_ANY(42) == 42);
static_assert(ECS::FOLD_ANY(WITH_GENERATION_3) == WITH_GENERATION_3, "a plain id keeps its generation");
static_assert(ECS::FOLD_ANY(ECS::PAIR(42, ECS::ANY)) == ECS::PAIR(42, ECS::WILDCARD));
static_assert(ECS::FOLD_ANY(ECS::PAIR(ECS::ANY, 42)) == ECS::PAIR(ECS::WILDCARD, 42));
static_assert(ECS::FOLD_ANY(ECS::PAIR(ECS::ANY, ECS::ANY)) == ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD));
static_assert(ECS::FOLD_ANY(ECS::PAIR(42, 7)) == ECS::PAIR(42, 7));

// --- ID_MATCHES --------------------------------------------------------------

static_assert(ECS::ID_MATCHES(42, 42));
static_assert(!ECS::ID_MATCHES(42, 43));
static_assert(!ECS::ID_MATCHES(42, WITH_GENERATION_3), "plain ids compare with their generation");
static_assert(ECS::ID_MATCHES(ECS::WILDCARD, 42));
static_assert(ECS::ID_MATCHES(ECS::ANY, 42));
static_assert(ECS::ID_MATCHES(ECS::WILDCARD, ECS::PAIR(42, 7)), "a plain wildcard matches pairs too");
static_assert(!ECS::ID_MATCHES(ECS::THIS, 42));
static_assert(!ECS::ID_MATCHES(ECS::PAIR(42, ECS::WILDCARD), 42), "a pair pattern never matches a plain id");
static_assert(!ECS::ID_MATCHES(ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD), 42));
static_assert(!ECS::ID_MATCHES(42, ECS::PAIR(42, 7)), "a plain pattern never matches a pair");

static_assert(ECS::ID_MATCHES(ECS::PAIR(42, 7), ECS::PAIR(42, 7)));
static_assert(ECS::ID_MATCHES(ECS::PAIR(42, ECS::WILDCARD), ECS::PAIR(42, 7)));
static_assert(ECS::ID_MATCHES(ECS::PAIR(42, ECS::ANY), ECS::PAIR(42, 7)));
static_assert(ECS::ID_MATCHES(ECS::PAIR(ECS::WILDCARD, 7), ECS::PAIR(42, 7)));
static_assert(ECS::ID_MATCHES(ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD), ECS::PAIR(42, 7)));
static_assert(!ECS::ID_MATCHES(ECS::PAIR(42, ECS::WILDCARD), ECS::PAIR(43, 7)));
static_assert(!ECS::ID_MATCHES(ECS::PAIR(ECS::WILDCARD, 7), ECS::PAIR(42, 8)));
static_assert(!ECS::ID_MATCHES(ECS::PAIR(42, 7), ECS::PAIR(7, 42)));

// --- Pair / PairTraits / StorageType -----------------------------------------

static_assert(!ECS::PairTraits<Position>::is_pair);
static_assert(ECS::PairTraits<ECS::Pair<Position, Velocity>>::is_pair);
static_assert(std::is_same_v<ECS::StorageType<Position>, Position>);
static_assert(std::is_same_v<ECS::Pair<Position, Velocity>::type, Position>, "the relation's data wins");
static_assert(std::is_same_v<ECS::Pair<Likes, Position>::type, Position>, "a tag relation takes the target's data");
static_assert(std::is_same_v<ECS::Pair<Likes, Eats>::type, Likes>, "both empty: the pair is a tag spelled as First");
static_assert(std::is_same_v<ECS::Pair<Position, Likes>::type, Position>);
static_assert(std::is_same_v<ECS::StorageType<ECS::Pair<Likes, Health>>, Health>);
static_assert(std::is_same_v<ECS::Pair<Likes, Eats>::first, Likes>);
static_assert(std::is_same_v<ECS::Pair<Likes, Eats>::second, Eats>);

} // namespace

TEST_CASE("ecs/id: ENTITY_LOW strips the generation") {
    CHECK(ECS::ENTITY_LOW(WITH_GENERATION_3) == 42);
    CHECK(ECS::ENTITY_LOW(GENERATION_ONE) == 0);
    CHECK(ECS::ENTITY_LOW(ECS::ENTITY_MASK) == ECS::ENTITY_MASK);
}

TEST_CASE("ecs/id: PAIR packs two low ids under the pair flag") {
    const Id pair = ECS::PAIR(WITH_GENERATION_3, WITH_GENERATION_7);
    CHECK(ECS::IS_PAIR(pair));
    CHECK(ECS::PAIR_FIRST(pair) == 42);
    CHECK(ECS::PAIR_SECOND(pair) == 7);
    CHECK(pair == ECS::PAIR(42, 7));
    CHECK((pair & ECS::ID_FLAGS_MASK) == ECS::ID_FLAG_PAIR);

    SUBCASE("the full low range survives on the target side") {
        const Id wide = ECS::PAIR(1, ECS::ENTITY_MASK);
        CHECK(ECS::PAIR_SECOND(wide) == ECS::ENTITY_MASK);
        CHECK(ECS::PAIR_FIRST(wide) == 1);
    }
    SUBCASE("the relation side uses the bits below the flags") {
        const Id wide = ECS::PAIR((1ull << 28) - 1, 1);
        CHECK(ECS::PAIR_FIRST(wide) == (1ull << 28) - 1);
        CHECK(ECS::PAIR_SECOND(wide) == 1);
        CHECK(ECS::IS_PAIR(wide));
    }
}

TEST_CASE("ecs/id: IS_PAIR is false for every plain id") {
    CHECK_FALSE(ECS::IS_PAIR(0));
    CHECK_FALSE(ECS::IS_PAIR(1));
    CHECK_FALSE(ECS::IS_PAIR(ECS::MAX_COMPONENT_ID));
    CHECK_FALSE(ECS::IS_PAIR(ECS::REST));
    CHECK_FALSE(ECS::IS_PAIR(WITH_GENERATION_3));
    CHECK_FALSE(ECS::IS_PAIR(ECS::ID_COMPONENT_MASK));
}

TEST_CASE("ecs/id: IS_WILDCARD accepts WILDCARD and ANY only") {
    CHECK(ECS::IS_WILDCARD(ECS::WILDCARD));
    CHECK(ECS::IS_WILDCARD(ECS::ANY));
    CHECK_FALSE(ECS::IS_WILDCARD(ECS::THIS));
    CHECK_FALSE(ECS::IS_WILDCARD(ECS::COMPONENT));
    CHECK_FALSE(ECS::IS_WILDCARD(ECS::REST));
    CHECK_FALSE(ECS::IS_WILDCARD(ECS::MAX_COMPONENT_ID));
}

TEST_CASE("ecs/id: PAIR_HAS_WILDCARD looks at both sides") {
    CHECK(ECS::PAIR_HAS_WILDCARD(ECS::PAIR(ECS::CHILD_OF, ECS::WILDCARD)));
    CHECK(ECS::PAIR_HAS_WILDCARD(ECS::PAIR(ECS::WILDCARD, ECS::CHILD_OF)));
    CHECK(ECS::PAIR_HAS_WILDCARD(ECS::PAIR(ECS::ANY, 5)));
    CHECK(ECS::PAIR_HAS_WILDCARD(ECS::PAIR(5, ECS::ANY)));
    CHECK_FALSE(ECS::PAIR_HAS_WILDCARD(ECS::PAIR(ECS::CHILD_OF, 5)));
    CHECK_FALSE(ECS::PAIR_HAS_WILDCARD(ECS::PAIR(ECS::CHILD_OF, ECS::THIS)));
}

TEST_CASE("ecs/id: FOLD_ANY rewrites ANY as WILDCARD") {
    SUBCASE("plain ids") {
        CHECK(ECS::FOLD_ANY(ECS::ANY) == ECS::WILDCARD);
        CHECK(ECS::FOLD_ANY(ECS::WILDCARD) == ECS::WILDCARD);
        CHECK(ECS::FOLD_ANY(ECS::THIS) == ECS::THIS);
        CHECK(ECS::FOLD_ANY(WITH_GENERATION_3) == WITH_GENERATION_3);
    }
    SUBCASE("pairs") {
        CHECK(ECS::FOLD_ANY(ECS::PAIR(ECS::ANY, 7)) == ECS::PAIR(ECS::WILDCARD, 7));
        CHECK(ECS::FOLD_ANY(ECS::PAIR(7, ECS::ANY)) == ECS::PAIR(7, ECS::WILDCARD));
        CHECK(ECS::FOLD_ANY(ECS::PAIR(ECS::ANY, ECS::ANY)) == ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD));
        CHECK(ECS::FOLD_ANY(ECS::PAIR(ECS::WILDCARD, ECS::ANY)) == ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD));
    }
    SUBCASE("pairs without ANY are unchanged") {
        CHECK(ECS::FOLD_ANY(ECS::PAIR(42, 7)) == ECS::PAIR(42, 7));
        CHECK(ECS::FOLD_ANY(ECS::PAIR(ECS::WILDCARD, 7)) == ECS::PAIR(ECS::WILDCARD, 7));
    }
    SUBCASE("folding is idempotent") {
        const Id pattern = ECS::PAIR(ECS::ANY, ECS::ANY);
        CHECK(ECS::FOLD_ANY(ECS::FOLD_ANY(pattern)) == ECS::FOLD_ANY(pattern));
    }
}

TEST_CASE("ecs/id: ID_MATCHES compares plain ids by equality") {
    CHECK(ECS::ID_MATCHES(42, 42));
    CHECK(ECS::ID_MATCHES(WITH_GENERATION_3, WITH_GENERATION_3));
    CHECK_FALSE(ECS::ID_MATCHES(42, WITH_GENERATION_3));
    CHECK_FALSE(ECS::ID_MATCHES(42, 43));
    CHECK_FALSE(ECS::ID_MATCHES(42, ECS::PAIR(42, 42)));
}

TEST_CASE("ecs/id: ID_MATCHES plain wildcards match any id") {
    CHECK(ECS::ID_MATCHES(ECS::WILDCARD, 1));
    CHECK(ECS::ID_MATCHES(ECS::ANY, ECS::REST));
    CHECK(ECS::ID_MATCHES(ECS::WILDCARD, ECS::PAIR(1, 2)));
    CHECK(ECS::ID_MATCHES(ECS::ANY, ECS::PAIR(1, 2)));
    CHECK_FALSE(ECS::ID_MATCHES(ECS::THIS, 1));
}

TEST_CASE("ecs/id: ID_MATCHES pair patterns match side by side") {
    const Id likes_apple = ECS::PAIR(ECS::REST + 1, ECS::REST + 2);
    const Id likes_pear = ECS::PAIR(ECS::REST + 1, ECS::REST + 3);
    const Id eats_apple = ECS::PAIR(ECS::REST + 4, ECS::REST + 2);

    SUBCASE("(R, *) matches every pair with relation R") {
        const Id pattern = ECS::PAIR(ECS::REST + 1, ECS::WILDCARD);
        CHECK(ECS::ID_MATCHES(pattern, likes_apple));
        CHECK(ECS::ID_MATCHES(pattern, likes_pear));
        CHECK_FALSE(ECS::ID_MATCHES(pattern, eats_apple));
    }
    SUBCASE("(*, T) matches every pair with target T") {
        const Id pattern = ECS::PAIR(ECS::ANY, ECS::REST + 2);
        CHECK(ECS::ID_MATCHES(pattern, likes_apple));
        CHECK(ECS::ID_MATCHES(pattern, eats_apple));
        CHECK_FALSE(ECS::ID_MATCHES(pattern, likes_pear));
    }
    SUBCASE("(*, *) matches every pair and no plain id") {
        const Id pattern = ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD);
        CHECK(ECS::ID_MATCHES(pattern, likes_apple));
        CHECK(ECS::ID_MATCHES(pattern, eats_apple));
        CHECK_FALSE(ECS::ID_MATCHES(pattern, ECS::REST + 1));
    }
    SUBCASE("a concrete pair only matches itself") {
        CHECK(ECS::ID_MATCHES(likes_apple, likes_apple));
        CHECK_FALSE(ECS::ID_MATCHES(likes_apple, likes_pear));
        CHECK_FALSE(ECS::ID_MATCHES(likes_apple, eats_apple));
    }
    SUBCASE("a pair pattern never matches a plain id") {
        CHECK_FALSE(ECS::ID_MATCHES(ECS::PAIR(ECS::REST + 1, ECS::WILDCARD), ECS::REST + 1));
        CHECK_FALSE(ECS::ID_MATCHES(ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD), ECS::WILDCARD));
    }
}

TEST_CASE("ecs/id: built-in ids are distinct and sit after the component range") {
    const Id builtins[] = {
        ECS::WILDCARD, ECS::ANY, ECS::THIS, ECS::COMPONENT, ECS::EXCLUSIVE, ECS::TRAVERSABLE,
        ECS::ON_DELETE, ECS::ON_DELETE_TARGET, ECS::REMOVE, ECS::DELETE, ECS::PANIC,
        ECS::CHILD_OF, ECS::IS_A, ECS::REST,
    };
    const usz count = sizeof(builtins) / sizeof(builtins[0]);
    for (usz i = 0; i < count; i++) {
        CHECK(builtins[i] > ECS::MAX_COMPONENT_ID);
        CHECK(builtins[i] <= ECS::REST);
        CHECK_FALSE(ECS::IS_PAIR(builtins[i]));
        for (usz j = i + 1; j < count; j++) {
            CHECK(builtins[i] != builtins[j]);
        }
    }
}

TEST_CASE("ecs/id: PairTraits tells pairs from plain types") {
    CHECK_FALSE(ECS::PairTraits<Health>::is_pair);
    CHECK_FALSE(ECS::PairTraits<TagA>::is_pair);
    CHECK(ECS::PairTraits<ECS::Pair<TagA, Health>>::is_pair);
    CHECK(ECS::PairTraits<ECS::Pair<Health, TagA>>::is_pair);
    CHECK(sizeof(ECS::StorageType<ECS::Pair<TagA, Health>>) == sizeof(Health));
    CHECK(sizeof(ECS::StorageType<ECS::Pair<Health, Position>>) == sizeof(Health));
}
