#include "support/test_support.hpp"

#include "engine/ecs/ecs.hpp"
#include "engine/ecs/utils/bloom.hpp"

#include <bit>

static_assert(BLOOM_FILTER_SIZE == 64, "the filter is one 64-bit word");

TEST_CASE("ecs/bloom: a fresh filter is empty") {
    BloomFilter filter;
    CHECK(filter.is_empty());
    CHECK(filter.bitset == 0);
}

TEST_CASE("ecs/bloom: the explicit constructor adopts the bitset") {
    const BloomFilter filter(0b1010ull);
    CHECK(filter.bitset == 0b1010ull);
    CHECK_FALSE(filter.is_empty());
}

TEST_CASE("ecs/bloom: mix is deterministic and spreads nearby inputs") {
    CHECK(mix(0) == mix(0));
    CHECK(mix(1) == mix(1));
    CHECK(mix(0) != mix(1));
    CHECK(mix(1) != mix(2));
    CHECK(mix(ECS::PAIR(1, 2)) != mix(ECS::PAIR(2, 1)));
}

TEST_CASE("ecs/bloom: add sets one or two bits") {
    BloomFilter filter;
    filter.add(ECS::WILDCARD);
    CHECK_FALSE(filter.is_empty());
    const int bits = std::popcount(filter.bitset);
    CHECK(bits >= 1);
    CHECK(bits <= 2);

    SUBCASE("adding the same value again changes nothing") {
        const u64 before = filter.bitset;
        filter.add(ECS::WILDCARD);
        CHECK(filter.bitset == before);
    }
    SUBCASE("adding a value is the same in any filter") {
        BloomFilter other;
        other.add(ECS::WILDCARD);
        CHECK(other.bitset == filter.bitset);
    }
}

TEST_CASE("ecs/bloom: the bits of a value are exactly those of its singleton filter") {
    BloomFilter filter;
    filter.add(ECS::REST + 1);
    filter.add(ECS::PAIR(ECS::REST + 1, ECS::REST + 2));

    BloomFilter singleton;
    singleton.add(ECS::REST + 1);
    CHECK((filter.bitset & singleton.bitset) == singleton.bitset);
    CHECK((filter.bitset | singleton.bitset) == filter.bitset);
}

TEST_CASE("ecs/bloom: test has no false negatives") {
    // Everything added to `held` must pass when queried as a subset, however
    // many values collide.
    BloomFilter held;
    const u64 base = ECS::REST + 1;
    for (u64 i = 0; i < 40; i++) {
        held.add(base + i);
        held.add(ECS::PAIR(base + i, base + (i * 7) % 13));
    }

    for (u64 i = 0; i < 40; i++) {
        BloomFilter query;
        query.add(base + i);
        CHECK(held.test(query));

        BloomFilter pair_query;
        pair_query.add(ECS::PAIR(base + i, base + (i * 7) % 13));
        CHECK(held.test(pair_query));

        BloomFilter both;
        both.add(base + i);
        both.add(ECS::PAIR(base + i, base + (i * 7) % 13));
        CHECK(held.test(both));
    }
    CHECK(held.test(held));
}

TEST_CASE("ecs/bloom: an empty query passes every filter") {
    const BloomFilter empty;
    BloomFilter held;
    held.add(ECS::REST + 5);
    CHECK(held.test(empty));
    CHECK(empty.test(empty));
}

TEST_CASE("ecs/bloom: test rejects a value whose bits are missing") {
    // An empty filter holds no bits at all, so any non-empty query must be
    // rejected: a false result is definitive.
    const BloomFilter empty;
    BloomFilter query;
    query.add(ECS::REST + 1);
    CHECK_FALSE(empty.test(query));

    SUBCASE("a filter with unrelated bits rejects a disjoint query") {
        const BloomFilter other(~query.bitset);
        CHECK_FALSE(other.test(query));
        CHECK_FALSE(other.intersects(query));
    }
}

TEST_CASE("ecs/bloom: intersects is false only for provably disjoint sets") {
    const BloomFilter empty;
    BloomFilter a;
    a.add(ECS::REST + 1);

    CHECK_FALSE(empty.intersects(a));
    CHECK_FALSE(a.intersects(empty));
    CHECK_FALSE(empty.intersects(empty));
    CHECK(a.intersects(a));

    SUBCASE("a superset intersects every subset") {
        BloomFilter superset = a;
        superset.add(ECS::REST + 2);
        superset.add(ECS::PAIR(ECS::REST + 1, ECS::REST + 2));
        CHECK(superset.intersects(a));
        CHECK(a.intersects(superset));
        CHECK(superset.test(a));
    }
    SUBCASE("the complement never intersects") {
        const BloomFilter complement(~a.bitset);
        CHECK_FALSE(complement.intersects(a));
        CHECK_FALSE(a.intersects(complement));
    }
}

TEST_CASE("ecs/bloom: test is a subset relation, intersects is symmetric") {
    BloomFilter a;
    a.add(ECS::REST + 10);
    BloomFilter b = a;
    b.add(ECS::REST + 11);
    b.add(ECS::REST + 12);

    CHECK(b.test(a));
    CHECK(a.intersects(b) == b.intersects(a));
    // `a` may still pass test(b) if the extra values collided onto a's bits,
    // which is allowed; only a superset relation is guaranteed.
    if (a.bitset != b.bitset) {
        CHECK_FALSE(a.test(b));
    }
}
