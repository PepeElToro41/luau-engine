#include "engine/ecs/query_iter.hpp"

usz QUERY::count(QueryIter& it) {
    usz total = 0;
    while (it.next(&it)) {
        total += it.count;
    }
    return total;
}

bool QUERY::empty(QueryIter& it) {
    while (it.next(&it)) {
        if (it.count > 0) {
            return false;
        }
    }
    return true;
}

EntityId QUERY::first(QueryIter& it) {
    while (it.next(&it)) {
        if (it.count > 0) {
            return it.entities[0];
        }
    }
    return 0;
}

u64 QUERY::next_random(u64& state) {
    u64 x = state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

EntityId QUERY::random(QueryIter& it, u64& rng_state) {
    if (rng_state == 0) {
        rng_state = 0x9E3779B97F4A7C15ull;
    }

    // Weighted reservoir over chunks: after seeing `seen` rows in total, the
    // current chunk replaces the pick with probability count / seen, and the
    // row inside it is uniform. Every row ends up equally likely.
    EntityId picked = 0;
    usz seen = 0;
    while (it.next(&it)) {
        if (it.count == 0) {
            continue;
        }
        seen += it.count;
        const usz roll = static_cast<usz>(QUERY::next_random(rng_state) % seen);
        if (roll < it.count) {
            picked = it.entities[roll];
        }
    }
    return picked;
}
