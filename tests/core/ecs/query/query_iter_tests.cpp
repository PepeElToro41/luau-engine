#include "support/test_support.hpp"

#include "engine/ecs/query/query_iter.hpp"

// The QUERY:: utilities only depend on the iterator contract, so they are
// tested against a hand-rolled producer that yields fixed chunks.

namespace {

struct Chunk {
    EntityId* entities;
    usz count;
};

struct ChunkSource {
    const Chunk* chunks;
    usz chunk_count;
    usz position;
};

bool next_chunk(QueryIter* it) {
    ChunkSource* source = static_cast<ChunkSource*>(it->state);
    if (source->position >= source->chunk_count) {
        it->entities = nullptr;
        it->count = 0;
        return false;
    }
    const Chunk& chunk = source->chunks[source->position];
    source->position++;
    it->entities = chunk.entities;
    it->count = chunk.count;
    return true;
}

QueryIter make_iter(ChunkSource* source) {
    source->position = 0;
    QueryIter it;
    it.next = next_chunk;
    it.state = source;
    return it;
}

} // namespace

TEST_CASE("ecs/query_iter: utilities over an empty iterator") {
    ChunkSource source { nullptr, 0, 0 };

    QueryIter it = make_iter(&source);
    CHECK(QUERY::count(it) == 0);
    it = make_iter(&source);
    CHECK(QUERY::empty(it));
    it = make_iter(&source);
    CHECK(QUERY::first(it) == 0);
    it = make_iter(&source);
    u64 rng = 1;
    CHECK(QUERY::random(it, rng) == 0);
}

TEST_CASE("ecs/query_iter: count, empty and first skip empty chunks") {
    EntityId a[] = { 10, 11, 12 };
    EntityId b[] = { 20 };
    const Chunk chunks[] = { { nullptr, 0 }, { a, 3 }, { nullptr, 0 }, { b, 1 } };
    ChunkSource source { chunks, 4, 0 };

    QueryIter it = make_iter(&source);
    CHECK(QUERY::count(it) == 4);
    // Once exhausted, next keeps saying no.
    CHECK_FALSE(it.next(&it));
    CHECK_FALSE(it.next(&it));

    it = make_iter(&source);
    CHECK_FALSE(QUERY::empty(it));
    // empty() stopped at the first non-empty chunk, leaving the rest.
    CHECK(source.position == 2);

    it = make_iter(&source);
    CHECK(QUERY::first(it) == 10);

    const Chunk only_empty[] = { { nullptr, 0 }, { nullptr, 0 } };
    ChunkSource empty_source { only_empty, 2, 0 };
    it = make_iter(&empty_source);
    CHECK(QUERY::empty(it));
    it = make_iter(&empty_source);
    CHECK(QUERY::count(it) == 0);
}

TEST_CASE("ecs/query_iter: field reads an output column of the chunk") {
    Position positions[] = { { 1, 2 }, { 3, 4 } };
    void* columns[] = { positions, nullptr };

    QueryIter it;
    it.columns = columns;
    it.field_count = 2;
    CHECK(it.field<0, Position>() == positions);
    CHECK(it.field<0, Position>()[1].x == 3);
    CHECK(it.field<1, Velocity>() == nullptr);
}

TEST_CASE("ecs/query_iter: random picks every entity and only matched ones") {
    EntityId a[] = { 1, 2, 3 };
    EntityId b[] = { 4 };
    EntityId c[] = { 5, 6 };
    const Chunk chunks[] = { { a, 3 }, { nullptr, 0 }, { b, 1 }, { c, 2 } };
    ChunkSource source { chunks, 4, 0 };

    usz hits[7] = {};
    u64 rng = 12345;
    const usz rounds = 6000;
    for (usz i = 0; i < rounds; i++) {
        QueryIter it = make_iter(&source);
        const EntityId picked = QUERY::random(it, rng);
        REQUIRE(picked >= 1);
        REQUIRE(picked <= 6);
        hits[picked]++;
    }
    // Uniform over 6 entities: ~1000 each. Allow a wide margin, this is a
    // sanity check on the weighting, not a statistical test.
    for (usz entity = 1; entity <= 6; entity++) {
        CHECK(hits[entity] > rounds / 6 / 2);
        CHECK(hits[entity] < rounds / 6 * 2);
    }

    SUBCASE("a zero state is reseeded instead of getting stuck") {
        u64 zero = 0;
        QueryIter it = make_iter(&source);
        const EntityId picked = QUERY::random(it, zero);
        CHECK(zero != 0);
        CHECK(picked >= 1);
        CHECK(picked <= 6);
    }

    SUBCASE("a single entity is always picked") {
        const Chunk single[] = { { b, 1 } };
        ChunkSource single_source { single, 1, 0 };
        for (usz i = 0; i < 20; i++) {
            QueryIter it = make_iter(&single_source);
            CHECK(QUERY::random(it, rng) == 4);
        }
    }
}

TEST_CASE("ecs/query_iter: next_random never returns to zero and varies") {
    u64 state = 1;
    const u64 first = QUERY::next_random(state);
    CHECK(state != 0);
    const u64 second = QUERY::next_random(state);
    CHECK(first != second);
    CHECK(state != 0);
}
